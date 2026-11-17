#!/usr/bin/env bash
# Builds Meld Qt as an AppImage, in the Steam Linux Runtime 4 SDK, so that the
# program and the libraries bundled with it are built against that runtime's
# libraries (Debian 13's glibc 2.41, GCC 14) rather than the build system's.
#
# The SDK is the container image registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk,
# the version of the Steam Linux Runtime 4 that Steam has installed, if it's
# found. Qt 6.8 is installed in it from Debian 13 (which the SDK's apt sources
# have), in an image that's built once and kept for later builds. The AppDir is
# filled by linuxdeploy and its Qt plugin, and packed by appimagetool, with the
# AppImage runtime that doesn't need libfuse2; these are downloaded once, into
# the build folder.
#
# Needs Docker or Podman, and network access the first time.
#
#   ./build-appimage.sh                          # build-appimage/Meld-Qt-<version>-x86_64.AppImage
#   ./build-appimage.sh --tests                  # and run the test suite in the runtime
#   ./build-appimage.sh -- -DMELD_SINGLE_INSTANCE_NETWORK=ON   # more CMake options
#
# Environment:
#   MELD_VERSION           the version the program reports and the AppImage is named for
#   CONTAINER_ENGINE       docker or podman (default: whichever is installed, docker first)
#   STEAMRT_SDK_IMAGE      the SDK image (default: the installed runtime's version,
#                          or :latest if it isn't found)
#   STEAM_LINUX_RUNTIME_4  the installed runtime (default: in Steam's library under ~)

set -euo pipefail

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "$0"
    cat <<EOF
Usage: $0 [options] [-- cmake options]

Options:
  --build-dir DIR    the build folder (default: build-appimage in the sources)
  --tests            build and run the test suite too
  --clean            remove the build (not the downloaded tools) first
  --rebuild-image    rebuild the image with Qt, with the latest packages
  -h, --help         show this help
EOF
}

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=$source_dir/build-appimage
run_tests=OFF
clean=0
rebuild_image=0
cmake_args=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-dir) build_dir=$2; shift 2 ;;
        --build-dir=*) build_dir=${1#*=}; shift ;;
        --tests) run_tests=ON; shift ;;
        --clean) clean=1; shift ;;
        --rebuild-image) rebuild_image=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; cmake_args=("$@"); break ;;
        *) echo "$0: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "$0: $*" >&2; exit 1; }
info() { echo "==> $*" >&2; }

# The container engine
engine=${CONTAINER_ENGINE:-}
if [[ -z $engine ]]; then
    for candidate in docker podman; do
        if command -v "$candidate" >/dev/null; then engine=$candidate; break; fi
    done
fi
[[ -n $engine ]] || die "Docker or Podman is needed to run the Steam Linux Runtime 4 SDK"
command -v "$engine" >/dev/null || die "$engine isn't installed"

# The SDK, of the version of the runtime that's installed
sdk_image=${STEAMRT_SDK_IMAGE:-}
if [[ -z $sdk_image ]]; then
    runtime=${STEAM_LINUX_RUNTIME_4:-}
    if [[ -z $runtime ]]; then
        for dir in "${XDG_DATA_HOME:-$HOME/.local/share}/Steam" "$HOME/.steam/steam" \
                   "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
            if [[ -f $dir/steamapps/common/SteamLinuxRuntime_4/VERSIONS.txt ]]; then
                runtime=$dir/steamapps/common/SteamLinuxRuntime_4
                break
            fi
        done
    fi
    sdk_version=
    if [[ -n $runtime && -f $runtime/VERSIONS.txt ]]; then
        sdk_version=$(awk '$1 == "steamrt4" { print $2 }' "$runtime/VERSIONS.txt")
    fi
    if [[ -n $sdk_version ]]; then
        info "Steam Linux Runtime 4 $sdk_version found in $runtime"
    else
        info "Steam Linux Runtime 4 isn't installed (or STEAM_LINUX_RUNTIME_4 isn't it): using the latest SDK"
        sdk_version=latest
    fi
    sdk_image=registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk:$sdk_version
fi

# The SDK with Qt, built once. Its tag names the SDK and the packages
# installed, so that changing either makes a new one.
dockerfile="FROM $sdk_image
RUN apt-get update \\
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \\
        qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \\
        qt6-svg-plugins qt6-wayland qt6-translations-l10n \\
        desktop-file-utils librsvg2-bin \\
 && rm -rf /var/lib/apt/lists/*
"
image_hash=$(printf '%s' "$dockerfile" | sha256sum | cut -c1-12)
image=localhost/meld-qt-appimage:steamrt4-$image_hash
if [[ $rebuild_image == 1 ]] || ! "$engine" image inspect "$image" >/dev/null 2>&1; then
    info "Building the image with Qt ($image), from $sdk_image"
    context=$(mktemp -d)
    trap 'rm -rf "$context"' EXIT
    printf '%s' "$dockerfile" > "$context/Dockerfile"
    build_options=()
    [[ $rebuild_image == 1 ]] && build_options+=(--pull --no-cache)
    "$engine" build "${build_options[@]}" -t "$image" "$context"
    rm -rf "$context"
    trap - EXIT
fi

mkdir -p "$build_dir"
build_dir=$(cd "$build_dir" && pwd)
if [[ $clean == 1 ]]; then
    info "Removing the previous build"
    rm -rf "$build_dir/build" "$build_dir/AppDir"
fi

# The build, in the container: the sources are read-only, and the files made
# belong to the user running this
run_options=(--rm -i
    -v "$source_dir:/src:ro"
    -v "$build_dir:/out"
    -e HOME=/tmp
    -e LANG=C.UTF-8
    -e MELD_VERSION="${MELD_VERSION:-}"
    -e RUN_TESTS="$run_tests")
if [[ $engine == podman ]]; then
    run_options+=(--userns=keep-id)
else
    run_options+=(--user "$(id -u):$(id -g)")
fi

info "Building in $image"
"$engine" run "${run_options[@]}" "$image" bash -s -- "${cmake_args[@]}" <<'IN_CONTAINER'
set -euo pipefail
info() { echo "==> $*" >&2; }

tools=/out/tools
mkdir -p "$tools"
download() {
    if [[ ! -x $tools/$1 ]]; then
        info "Downloading $1"
        curl -fL --retry 3 -o "$tools/$1.part" "$2"
        chmod +x "$tools/$1.part"
        mv "$tools/$1.part" "$tools/$1"
    fi
}
download linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
download linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
download appimagetool-x86_64.AppImage \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
download runtime-x86_64 \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
# There's no FUSE in the container
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH=$tools:$PATH

version=${MELD_VERSION:-$(sed -nE '/^project\(/,/\)/s/^ *VERSION +([^ )]+).*/\1/p' /src/CMakeLists.txt)}

info "Configuring"
cmake -S /src -B /out/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMELD_BUILD_TESTS="$RUN_TESTS" \
    ${MELD_VERSION:+-DMELD_VERSION="$MELD_VERSION"} \
    "$@"
info "Building"
cmake --build /out/build

if [[ $RUN_TESTS == ON ]]; then
    info "Testing"
    ctest --test-dir /out/build --output-on-failure -j "$(nproc)"
fi

info "Filling the AppDir"
appdir=/out/AppDir
rm -rf "$appdir"
DESTDIR=$appdir cmake --install /out/build

# Qt's plugins: xcb is deployed anyway; Wayland, offscreen (for running
# without a display), and SVG for the icons, which are loaded as plugins
export QMAKE=/usr/bin/qmake6
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so;libqoffscreen.so"
export EXTRA_QT_MODULES="svg"
# Libraries that linuxdeploy leaves to the system, which isn't sure to have
# them: Debian's Qt GUI links glvnd's libOpenGL (only a dispatcher to the
# system's OpenGL, not part of the driver; Ubuntu's libopengl0 isn't always
# installed), and xcb's plugin the X session management libraries
libraries=()
for library in libOpenGL.so.0 libSM.so.6 libICE.so.6; do
    libraries+=(--library "/usr/lib/x86_64-linux-gnu/$library")
done
linuxdeploy-x86_64.AppImage \
    --appdir "$appdir" \
    "${libraries[@]}" \
    --executable "$appdir/usr/bin/meld-qt" \
    --desktop-file "$appdir/usr/share/applications/Meld-Qt.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/Meld-Qt.svg" \
    --plugin qt

# The AppImage's own icon (.DirIcon, shown by file managers), which
# appimagetool makes from a PNG of the desktop file's icon
rsvg-convert -w 256 -h 256 -o "$appdir/Meld-Qt.png" "$appdir/Meld-Qt.svg"

# The newest glibc symbol version that anything in the AppDir needs, which is
# the oldest glibc the AppImage runs with
glibc=$(find "$appdir" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null \
            | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sed 's/GLIBC_//' | sort -uV | tail -n1)

info "Packing the AppImage"
appimage=/out/Meld-Qt-$version-x86_64.AppImage
rm -f "$appimage"
ARCH=x86_64 VERSION=$version appimagetool-x86_64.AppImage \
    --runtime-file "$tools/runtime-x86_64" \
    "$appdir" "$appimage"

info "Built ${appimage#/out/} (it needs glibc $glibc or newer)"
IN_CONTAINER

info "The AppImage is in $build_dir"
