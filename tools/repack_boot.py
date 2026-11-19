#!/usr/bin/env python3
"""Rebuild boot.img from itself: same header, device tree and command line, but
with the current prebuilt/ramdisk/init (hmos-preinit) and optional changes to the
hmos.* options.

This is the fast loop for the ramdisk only (a second); the full pipeline
(hmos2star2lte.py) is only needed when the HarmonyOS images change. The same
options are written to the kernel command line and to /hmos.conf inside the
ramdisk (the S9+ bootloader truncates long command lines, hmos-preinit re-reads
the options from /hmos.conf).

  python3 tools/repack_boot.py
  python3 tools/repack_boot.py --out /tmp/boot.img --del hmos.policytest --set hmos.autorecovery=150

`--set name=value` replaces the option, `--set name` adds a bare one, `--del name`
removes it.
"""
import argparse
import gzip
import os
import sys
import tempfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from lib import bootimg                                  # noqa: E402
from lib.util import die, info                           # noqa: E402


def parse_opt(s):
    name, _, value = s.partition("=")
    return name, (value if _ else None)


def rewrite_opts(text, sets, dels):
    """Apply --set/--del to a whitespace separated option string."""
    toks = text.split()
    for s in sets + dels:
        name = parse_opt(s)[0]
        toks = [t for t in toks if t != name and not t.startswith(name + "=")]
    for s in sets:
        name, value = parse_opt(s)
        toks.append(name if value is None else "%s=%s" % (name, value))
    return " ".join(toks)


def ramdisk_cpio(rd):
    """Read the newc cpio out of the gzip'd ramdisk of a boot image (page padded)."""
    start = rd.find(b"\x1f\x8b\x08")
    if start < 0 or start > 1 << 20:
        die("the ramdisk in the boot image is not gzip'd (unknown layout)")
    return zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(rd[start:])


def main():
    ap = argparse.ArgumentParser(description="repack boot.img with a new hmos-preinit")
    ap.add_argument("--boot", default=os.path.join(REPO, "out", "boot.img"), help="boot.img to use as template")
    ap.add_argument("--kernel", default=os.path.join(REPO, "kernel-out", "Image"), help="kernel Image")
    ap.add_argument("--preinit", default=os.path.join(REPO, "prebuilt", "ramdisk", "init"), help="hmos-preinit")
    ap.add_argument("--out", help="where to write (default: overwrite --boot)")
    ap.add_argument("--set", action="append", default=[], metavar="NAME[=VALUE]")
    ap.add_argument("--del", dest="dels", action="append", default=[], metavar="NAME")
    a = ap.parse_args()

    for p in (a.boot, a.kernel, a.preinit):
        if not os.path.isfile(p):
            die("%s does not exist" % p)
    tpl = bootimg.parse_header(a.boot)
    pg = tpl["page_size"]
    pad = lambda x: (x + pg - 1) // pg
    with open(a.boot, "rb") as fh:
        fh.seek(pg * (1 + pad(tpl["kernel_size"]) + pad(tpl["second_size"])))
        rd = fh.read(tpl["ramdisk_size"])
    if len(rd) != tpl["ramdisk_size"]:
        die("short read on the ramdisk in %s" % a.boot)

    entries = bootimg.read_cpio(ramdisk_cpio(rd))
    if "init" not in {e["name"] for e in entries}:
        die("%s has no /init entry: not a boot.img built by hmos2star2lte.py" % a.boot)
    conf = next((e["data"].decode(errors="replace") for e in entries if e["name"] == "hmos.conf"), "")
    conf = rewrite_opts(conf, a.set, a.dels)
    preinit = open(a.preinit, "rb").read()
    new_entries = []
    for e in entries:
        if e["name"] == "init":
            e = dict(e, data=preinit)
        elif e["name"] == "hmos.conf":
            e = dict(e, data=conf.encode() + b"\n")
        new_entries.append(e)

    out = a.out or a.boot
    dtb = None
    if tpl.get("dt_size"):                       # v0 header: the stock device tree travels in boot.img
        fd, dtb = tempfile.mkstemp(suffix=".dt")
        with os.fdopen(fd, "wb") as fh:
            fh.write(bootimg.extract_dt(a.boot, tpl))
    cmdline = rewrite_opts(tpl["cmdline"], a.set, a.dels)
    info("ramdisk: %d entries, /init %d bytes, hmos.conf: %s" % (len(new_entries), len(preinit), conf.strip()))
    bootimg.build_boot_from_template(tpl, a.kernel, gzip.compress(bootimg.write_cpio(new_entries), 9, mtime=0),
                                     cmdline, out, dtb=dtb, samsung_trailer=True)
    if dtb:
        os.unlink(dtb)
    info("wrote %s (command line %d bytes)" % (out, len(cmdline)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
