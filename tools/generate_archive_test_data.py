#!/usr/bin/env python3
"""Generate the archives that tst_archive opens (tests/data/archives)

The archives are made with the standard library only, with fixed times, so
they're the same each time.
"""

import gzip
import io
import pathlib
import tarfile
import zipfile

OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "archives"
MTIME = 1_700_000_000


def tar_member(name, content=b"", *, type=tarfile.REGTYPE, mode=0o644):
    info = tarfile.TarInfo(name)
    info.type = type
    info.mode = mode
    info.mtime = MTIME
    if type == tarfile.REGTYPE:
        info.size = len(content)
        return info, io.BytesIO(content)
    return info, None


def write_tar(path, members, *, compress=False):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for info, data in members:
            tar.addfile(info, data)
    data = buffer.getvalue()
    if compress:
        data = gzip.compress(data, mtime=MTIME)
    path.write_bytes(data)


def write_zip(path, members):
    with zipfile.ZipFile(path, "w") as archive:
        for name, content in members:
            info = zipfile.ZipInfo(name, date_time=(2023, 11, 14, 22, 13, 20))
            info.external_attr = 0o644 << 16
            archive.writestr(info, content)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)

    # The two sides of a folder comparison
    write_tar(
        OUTPUT / "left.tar.gz",
        [
            tar_member("dir/", type=tarfile.DIRTYPE, mode=0o755),
            tar_member("dir/same.txt", b"same\n"),
            tar_member("dir/changed.txt", b"left\n"),
            tar_member("only-left.txt", b"left only\n"),
        ],
        compress=True,
    )
    write_zip(
        OUTPUT / "right.zip",
        [
            ("dir/same.txt", b"same\n"),
            ("dir/changed.txt", b"right\n"),
            ("only-right.txt", b"right only\n"),
        ],
    )

    # A member outside the extracted folder, which mustn't be written
    write_tar(
        OUTPUT / "unsafe.tar",
        [
            tar_member("../escaped.txt", b"escaped\n"),
            tar_member("safe.txt", b"safe\n"),
        ],
    )

    # A FIFO, which would block reading; and an unreadable file and folder
    write_tar(
        OUTPUT / "special.tar",
        [
            tar_member("pipe", type=tarfile.FIFOTYPE),
            tar_member("unreadable.txt", b"unreadable\n", mode=0o000),
            tar_member("closed/", type=tarfile.DIRTYPE, mode=0o000),
            tar_member("closed/inside.txt", b"inside\n"),
        ],
    )

    # A compressed tar that ends early
    (OUTPUT / "broken.tar.gz").write_bytes(gzip.compress(bytes(range(256)) * 64, mtime=MTIME)[:100])

    # Zip files that are documents, rather than archives
    with zipfile.ZipFile(OUTPUT / "document.odt", "w") as archive:
        info = zipfile.ZipInfo("mimetype", date_time=(2023, 11, 14, 22, 13, 20))
        archive.writestr(info, "application/vnd.oasis.opendocument.text", zipfile.ZIP_STORED)
        info = zipfile.ZipInfo("content.xml", date_time=(2023, 11, 14, 22, 13, 20))
        archive.writestr(info, "<office:document-content/>")


if __name__ == "__main__":
    main()
