#!/usr/bin/env python3
"""Writes src/app/meld-qt.ico, the Windows program's icon, from the scalable
icon in data/icons, at the sizes that Windows shows (in Explorer, the taskbar
and the Start menu, at 100% to 200% scale).

The images are PNG, which Windows reads in icons since Vista. Needs
rsvg-convert (librsvg) and only the standard library.

    tools/generate_windows_icon.py [output.ico]
"""

import pathlib
import struct
import subprocess
import sys

SOURCE_DIR = pathlib.Path(__file__).resolve().parent.parent
SVG = SOURCE_DIR / "data/icons/hicolor/scalable/apps/Meld-Qt.svg"
OUTPUT = SOURCE_DIR / "src/app/meld-qt.ico"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def render(size):
    return subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size), str(SVG)],
        check=True, capture_output=True).stdout


def main():
    output = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else OUTPUT
    images = [render(size) for size in SIZES]
    # ICONDIR, then an ICONDIRENTRY for each image (a size of 256 is 0)
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b""
    for size, data in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                               len(data), offset)
        offset += len(data)
    output.write_bytes(header + entries + b"".join(images))
    print(f"{output}: {', '.join(map(str, SIZES))} px")


if __name__ == "__main__":
    main()
