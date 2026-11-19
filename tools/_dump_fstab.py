#!/usr/bin/env python3
"""Dump fstab-like files from a HarmonyOS ramdisk.img (OTA first stage)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from lib import bootimg

path = sys.argv[1]
raw = open(path, "rb").read()
cpio = bootimg.unpack_ramdisk(raw)
entries = bootimg.read_cpio(cpio)
print("entries:", len(entries))
for e in entries:
    if "fstab" in e["name"] or e["name"].endswith(".cfg") or "init" in e["name"].rsplit("/", 1)[-1]:
        print("--- %s (%d bytes)" % (e["name"], len(e["data"])))
for e in entries:
    n = e["name"]
    if "fstab" in n:
        print("=" * 70)
        print(n)
        print("=" * 70)
        try:
            print(e["data"].decode(errors="replace"))
        except Exception:
            print("<binary>")
