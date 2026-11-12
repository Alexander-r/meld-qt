#!/bin/sh
# Run at installation by the qmake project (update-caches.cmake is the CMake
# project's): update the caches of desktop files, MIME types and icons for the
# installed files in the data folder given.

datadir=$1

update() {
    command -v "$1" > /dev/null || return 0
    echo "Updating: $*"
    "$@" || echo "$1 failed; menus, file types or icons may need it run again" >&2
}

update update-desktop-database -q "$datadir/applications"
update update-mime-database "$datadir/mime"
update gtk-update-icon-cache -q -t -f "$datadir/icons/hicolor"
