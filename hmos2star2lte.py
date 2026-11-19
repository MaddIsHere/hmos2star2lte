#!/usr/bin/env python3
"""hmos2star2lte: turn a HarmonyOS full OTA package into images for the Samsung Galaxy S9+ (star2lte).

  python3 hmos2star2lte.py --ota update_full_base.zip --kernel Image.gz-dtb --stock-boot stock_boot.img --out out/

Without --patches an interactive list lets you pick the patches. See README.md.
"""
import argparse
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib import builder, flashgen, hwota, patches as patchlib  # noqa: E402
from lib.device import Device  # noqa: E402
from lib.util import Fatal, step, info, warn, tool, human  # noqa: E402

VERSION = "0.1-star2lte"


def parse_args():
    ap = argparse.ArgumentParser(description="HarmonyOS full OTA -> Galaxy S9+ (star2lte) images")
    ap.add_argument("--device", default="star2lte", help="device description in devices/<name>/ (default: star2lte)")
    ap.add_argument("--kernel", help="device kernel for boot.img (Image.gz-dtb for a v0/v1 boot header)")
    ap.add_argument("--stock-boot", help="the phone's stock boot.img: header values and kernel command line")
    ap.add_argument("--dtb", help="DTB, only for a v2 boot header")
    ap.add_argument("--ota", help="HarmonyOS full OTA package (update_full_base.zip or update.bin)")
    ap.add_argument("--sweet-rom", help="donor-device ROM for patches that take firmware/config blobs from it "
                                        "(none of the star2lte patches do yet)")
    ap.add_argument("--out", default="out", help="output folder (default: out)")
    ap.add_argument("--work", help="work folder for extracted images (default: <out>/../work)")
    ap.add_argument("--patches", help="comma separated patch list instead of the menu: 'defaults', 'all', "
                                      "'none' or ids/numbers, e.g. defaults,-50 or 10,11,12")
    ap.add_argument("--no-setup-wizard", action="store_true",
                    help="force the 'remove setup wizard' patch on (use it when WiFi does not work)")
    ap.add_argument("--keep-setup-wizard", action="store_true", help="force the 'remove setup wizard' patch off")
    ap.add_argument("--list", action="store_true", help="list the patches and exit")
    ap.add_argument("--keep-work", action="store_true", help="do not delete the work folder at the end")
    return ap.parse_args()


def pick(all_patches, spec):
    toggle = [p for p in all_patches if not p.required]
    sel = set()
    for tok in [t.strip() for t in spec.split(",") if t.strip()]:
        neg = tok.startswith("-")
        tok = tok.lstrip("-+")
        if tok == "defaults":
            ids = {p.id for p in toggle if p.default}
        elif tok == "all":
            ids = {p.id for p in toggle}
        elif tok == "none":
            sel = set()
            continue
        else:
            ids = {patchlib.match(all_patches, tok).id}
        sel = sel - ids if neg else sel | ids
    return sel


def main():
    a = parse_args()
    all_patches = patchlib.load()
    wizard = next((p for p in all_patches if p.id.endswith("remove-setup-wizard")), None)

    if a.list:
        for p in all_patches:
            flag = "required" if p.required else ("on " if p.default else "off")
            print("%-28s [%s] %s" % (p.id, flag, p.name))
        return 0
    if not a.ota:
        print("--ota is required (see --help)")
        return 2

    dev = Device(a.device)
    print("hmos2star2lte %s - HarmonyOS -> %s" % (VERSION, dev.title))
    need = ["extract.erofs", "dump.erofs", "debugfs"] + (["mke2fs", "e2fsck"] if dev.fs == "ext4" else ["mkfs.erofs", "fsck.erofs"])
    for t in need:
        tool(t)

    ota = hwota.HwOta(a.ota)
    info("OTA: %s, %d components" % (ota.version or "unknown version", len(ota.components)))

    if a.patches:
        selected = pick(all_patches, a.patches)
    elif sys.stdin.isatty():
        selected = patchlib.menu(all_patches, {p.id for p in all_patches if p.default and not p.required})
    else:
        selected = {p.id for p in all_patches if p.default and not p.required}
    if wizard and a.no_setup_wizard:
        selected.add(wizard.id)
    if wizard and a.keep_setup_wizard:
        selected.discard(wizard.id)
    chosen, added = patchlib.resolve(all_patches, selected)
    for pid in sorted(added):
        warn("%s switched on: needed by another selected patch" % pid)
    if wizard and wizard.id not in {p.id for p in chosen}:
        warn("the setup wizard is kept: it can only be finished with a working WiFi connection")
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(os.path.dirname(out), "work"))
    if os.path.isdir(out) and os.listdir(out):
        stamp = time.strftime("%Y%m%d_%H%M%S")
        out = out.rstrip("/\\") + "_" + stamp
        warn("output folder not empty, writing to %s" % out)

    t0 = time.time()
    b = builder.Build(ota, a.sweet_rom, work, out, chosen, dev, kernel=a.kernel, stock_boot=a.stock_boot, dtb=a.dtb)
    b.extract_ota()
    b.fetch_blobs()
    b.build_vendor()
    b.build_system()
    b.build_boot()
    b.copy_untouched()

    step("Writing flash scripts")
    supers = [n for n in builder.SUPER_PARTS if os.path.isfile(os.path.join(out, n + ".img"))]
    summary = ["hmos2star2lte %s" % VERSION, "device: %s" % dev.title, "OTA: %s (%s)" % (os.path.basename(a.ota), ota.version or "?"),
               "donor ROM: %s" % (os.path.basename(a.sweet_rom) if a.sweet_rom else "-"),
               "built: %s" % time.strftime("%Y-%m-%d %H:%M"), "", "patches:"]
    summary += ["  [%s] %s - %s" % ("x" if p in chosen else " ", p.id, p.name) for p in all_patches]
    summary += ["", "images: " + ", ".join(supers + ["boot"])]
    flashgen.write(out, supers, "\n".join(summary) + "\n", dev)

    from lib.util import rmtree
    if a.keep_work:
        info("work folder kept as is: %s" % work)
    else:
        for d in ("vendor_tree", "system_tree", "tmp", "sweet_blobs", "_one"):
            rmtree(os.path.join(work, d))
        info("extracted OTA images kept in %s/ota for the next run (delete it to free space)" % work)

    step("Done in %d min: %s" % ((time.time() - t0) / 60, out))
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out) if f.endswith(".img"))
    info("%s of images (host partition %s must be larger)." % (human(size), dev.host_partition))
    info("First time on this phone: install_clean.sh (formats %s). Later builds: update_keep_data.sh" % dev.host_partition)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fatal as e:
        print("\nERROR: %s" % e, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
