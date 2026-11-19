#!/usr/bin/env python3
"""Temporary helper: list a boot image ramdisk or grep the hmos-preinit binary in it.

    tools/_boot_ramdisk.py <boot.img> [grep-string]
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from lib import bootimg  # noqa: E402


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    tpl = bootimg.parse_header(path)
    data = open(path, "rb").read()
    pg = tpl["page_size"]
    n = lambda x: (x + pg - 1) // pg
    off = pg * (1 + n(tpl["kernel_size"]))
    rd = data[off:off + tpl["ramdisk_size"]]
    cpio = bootimg.unpack_ramdisk(rd)
    for e in bootimg.read_cpio(cpio):
        body = e["data"]
        print(f"{len(body):9d}  {e['name']}")
        if want and want.encode() in body:
            print(f"           ^ contains {want!r}")


if __name__ == "__main__":
    main()
