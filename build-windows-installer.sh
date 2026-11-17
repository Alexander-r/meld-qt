#!/usr/bin/env bash
# Builds the Windows installer of Meld Qt, with NSIS, from a folder that the
# Windows build has been deployed to (see "Installing on Windows" in
# README.md): meld-qt.exe with the Qt libraries, plugins and translations that
# windeployqt copied next to it, and vc_redist.x64.exe, if it copied that
# (MSVC builds). The folder is installed as it is; the redistributable is run
# by the installer if the runtime it has isn't installed yet.
#
# makensis is used if it's installed (or MAKENSIS names it); otherwise it's
# run in a container, Ubuntu 24.04 with its nsis package, in an image that's
# built once and kept for later builds, which needs Docker or Podman, and
# network access the first time.
#
#   ./build-windows-installer.sh C:/meld-qt     # build-windows/Meld-Qt-<version>-x64-setup.exe
#
# Environment:
#   MELD_VERSION      the version the installer shows and is named for
#   MAKENSIS          the makensis to run (default: makensis in PATH)
#   CONTAINER_ENGINE  docker or podman (default: whichever is installed, docker first)

set -euo pipefail

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "$0"
    cat <<EOF
Usage: $0 [options] DEPLOYED_FOLDER

Options:
  --build-dir DIR    where the installer is written (default: build-windows in the sources)
  --container        run makensis in a container even if it's installed
  -h, --help         show this help
EOF
}

die() { echo "$0: $*" >&2; exit 1; }
info() { echo "==> $*" >&2; }

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=$source_dir/build-windows
use_container=0
dist_dir=

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#*=}; shift ;;
        --container) use_container=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "$0: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *) [[ -z $dist_dir ]] || die "only one folder can be packaged"; dist_dir=$1; shift ;;
    esac
done
[[ -n $dist_dir ]] || { usage >&2; exit 2; }
[[ -d $dist_dir ]] || die "$dist_dir isn't a folder"
dist_dir=$(cd "$dist_dir" && pwd)
mkdir -p "$build_dir"
build_dir=$(cd "$build_dir" && pwd)

# The program is in the folder itself (windeployqt's layout, and qmake's
# installation) or in its bin (CMake's installation)
if [[ -f $dist_dir/meld-qt.exe ]]; then
    app_exe=meld-qt.exe
elif [[ -f $dist_dir/bin/meld-qt.exe ]]; then
    app_exe='bin\meld-qt.exe'
else
    die "there's no meld-qt.exe in $dist_dir or its bin folder"
fi
[[ -f $dist_dir/Qt6Core.dll || -f $dist_dir/bin/Qt6Core.dll ]] ||
    die "$dist_dir has no Qt6Core.dll: run windeployqt on the program first (see \"Installing on Windows\" in README.md)"

project_version=$(sed -nE '/^project\(/,/\)/s/^ *VERSION +([^ )]+).*/\1/p' "$source_dir/CMakeLists.txt")
[[ $project_version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "can't read the project's version from CMakeLists.txt"
version=${MELD_VERSION:-$project_version}
output=Meld-Qt-$version-x64-setup.exe

# The commands that install the folder's files and remove them, the
# redistributable apart. The paths are relative to the folder, which is
# makensis's current folder.
nsis_dir=$build_dir/nsis
mkdir -p "$nsis_dir"
files_nsh=$nsis_dir/files.nsh
unfiles_nsh=$nsis_dir/uninstall-files.nsh
vcredist=
nsis_escape() { local s=${1//\$/\$\$}; printf '%s' "${s//\"/\$\\\"}"; }
windows_path() { local s=${1//\//\\}; nsis_escape "$s"; }
(
    cd "$dist_dir"
    : > "$files_nsh"
    : > "$unfiles_nsh"
    while IFS= read -r -d '' dir; do
        dir=${dir#./}
        if [[ $dir == . ]]; then
            printf 'SetOutPath "$INSTDIR"\n' >> "$files_nsh"
        else
            printf 'SetOutPath "$INSTDIR\\%s"\n' "$(windows_path "$dir")" >> "$files_nsh"
        fi
        while IFS= read -r -d '' file; do
            file=${file#./}
            [[ ${file,,} == vc_redist.*.exe || ${file,,} == */vc_redist.*.exe ]] && continue
            printf 'File "%s"\n' "$(nsis_escape "$file")" >> "$files_nsh"
            printf 'Delete "$INSTDIR\\%s"\n' "$(windows_path "$file")" >> "$unfiles_nsh"
        done < <(find "$dir" -mindepth 1 -maxdepth 1 -type f -print0 | sort -z)
    done < <(find . -type d -print0 | sort -z)
    # The folders, the deepest first, if they're empty then
    while IFS= read -r -d '' dir; do
        printf 'RMDir "$INSTDIR\\%s"\n' "$(windows_path "${dir#./}")" >> "$unfiles_nsh"
    done < <(find . -mindepth 1 -type d -print0 | sort -rz)
)
vcredist=$(cd "$dist_dir" && find . -iname 'vc_redist.x64.exe' -type f | head -n 1)
vcredist=${vcredist#./}
if [[ -n $vcredist ]]; then
    info "The installer runs $vcredist if the Visual C++ runtime isn't installed"
fi
info "$(grep -c '^File ' "$files_nsh") files from $dist_dir"

nsis_args=(-V3 -NOCD
    "-DVERSION=$version"
    "-DVERSION_NUMBER=$project_version"
    "-DDIST_DIR=$dist_dir"
    "-DAPP_EXE=$app_exe"
    "-DFILES_NSH=$files_nsh"
    "-DUNFILES_NSH=$unfiles_nsh"
    "-DOUTFILE=$build_dir/$output")
[[ -n $vcredist ]] && nsis_args+=("-DVCREDIST=$dist_dir/$vcredist")
nsis_args+=("$source_dir/packaging/windows/meld-qt.nsi")

makensis=${MAKENSIS:-}
if [[ -z $makensis && $use_container == 0 ]] && command -v makensis >/dev/null; then
    makensis=makensis
fi

if [[ -n $makensis ]]; then
    info "Building with $makensis"
    (cd "$dist_dir" && "$makensis" "${nsis_args[@]}")
else
    engine=${CONTAINER_ENGINE:-}
    if [[ -z $engine ]]; then
        for candidate in docker podman; do
            if command -v "$candidate" >/dev/null; then engine=$candidate; break; fi
        done
    fi
    [[ -n $engine ]] || die "makensis isn't installed, and Docker or Podman is needed to run it in a container"
    command -v "$engine" >/dev/null || die "$engine isn't installed"

    dockerfile="FROM docker.io/library/ubuntu:24.04
RUN apt-get update \\
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nsis \\
 && rm -rf /var/lib/apt/lists/*
"
    image=localhost/meld-qt-nsis:$(printf '%s' "$dockerfile" | sha256sum | cut -c1-12)
    if ! "$engine" image inspect "$image" >/dev/null 2>&1; then
        info "Building $image"
        context=$(mktemp -d)
        trap 'rm -rf "$context"' EXIT
        printf '%s' "$dockerfile" > "$context/Dockerfile"
        "$engine" build -t "$image" "$context"
    fi

    # The folders are at the same paths in the container, so the paths given
    # to makensis are the same; only the build folder is written to
    run_options=(--rm
        -v "$source_dir:$source_dir:ro"
        -v "$dist_dir:$dist_dir:ro"
        -v "$build_dir:$build_dir"
        -w "$dist_dir"
        -e LANG=C.UTF-8)
    if [[ $engine == podman ]]; then
        run_options+=(--userns=keep-id)
    else
        run_options+=(--user "$(id -u):$(id -g)")
    fi
    info "Building in $image"
    "$engine" run "${run_options[@]}" "$image" makensis "${nsis_args[@]}"
fi

info "Built $build_dir/$output"
