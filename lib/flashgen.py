"""Install scripts written next to the generated images (Windows .bat and Linux .sh).

The phone has no Android "super" partition: the partition images (system.img, vendor.img, ...) are plain
files inside one big ext4 host partition (device.json: storage.host_partition) and hmos-preinit attaches
them as loop devices at boot. Installing is therefore: recovery (TWRP/OrangeFox, adb) -> format the host
partition -> push the images -> write boot.img. No fastboot, no LP metadata.

The scripts are generated, not yet tested on a phone: read them before running them.
"""
import os
import shutil

from .util import REPO, info

HMOS_PARTS = ["system", "vendor", "chip_prod", "sys_prod", "cust", "patch", "version", "preload"]



def _sh_body(dev, parts, clean):
    host, d = dev.host_partition, dev.image_dir
    s = ['H=$(adb shell "ls /dev/block/by-name/%s /dev/block/bootdevice/by-name/%s 2>/dev/null | head -n 1" | tr -d \'\\r\')'
         % (host, host),
         '[ -n "$H" ] || { echo "partition %s not found: list /dev/block/by-name from recovery and fix devices/%s/device.json"; exit 1; }'
         % (host, dev.name),
         'B=$(adb shell "ls /dev/block/by-name/%s /dev/block/bootdevice/by-name/%s 2>/dev/null | head -n 1" | tr -d \'\\r\')'
         % (dev.part_name("boot"), dev.part_name("boot")),
         '[ -n "$B" ] || { echo "boot partition not found"; exit 1; }',
         'echo "host partition: $H   boot partition: $B"']
    if clean:
        s.append('adb shell "umount /mnt/hmos 2>/dev/null; mke2fs -t ext4 -F -L hmosimg $H" || exit 1')
    s += ['adb shell "mkdir -p /mnt/hmos && mount -t %s $H /mnt/hmos && mkdir -p /mnt/hmos/%s" || exit 1' % (dev.host_fs, d)]
    for p in parts:
        s.append('echo "pushing %s.img"; adb push "$OUT/%s.img" /mnt/hmos/%s/%s.img.new && '
                 'adb shell "mv /mnt/hmos/%s/%s.img.new /mnt/hmos/%s/%s.img" || exit 1' % (p, p, d, p, d, p, d, p))
    s += ['adb shell "sync; umount /mnt/hmos"',
          'adb push "$OUT/boot.img" /tmp/boot.img && adb shell "dd if=/tmp/boot.img of=$B bs=4096 && sync" || exit 1']
    return s


def write(out, parts, summary, dev):
    parts = [p for p in HMOS_PARTS if p in parts]
    ud = dev.part_name("userdata")

    sh = ["#!/bin/sh", "# GENERATED for %s - not yet tested on a phone." % dev.title,
          "# CLEAN INSTALL: formats the '%s' partition (%s) and writes boot.img." % (dev.host_partition, dev.host_fs),
          "# Phone in recovery (TWRP/OrangeFox) with adb. Afterwards wipe/format DATA in the recovery",
          "# (%s must be a fresh f2fs) before the first boot." % ud,
          'OUT=$(dirname "$0")',
          'printf "This ERASES the %s partition (Android system). Type YES to continue: "; read OK; [ "$OK" = YES ] || exit 1' % dev.host_partition,
          "adb wait-for-recovery"] + _sh_body(dev, parts, True) + [
          'echo "Done. Format data in the recovery now, then reboot."']
    open(os.path.join(out, "install_clean.sh"), "w").write("\n".join(sh) + "\n")

    up = ["#!/bin/sh", "# GENERATED for %s - not yet tested on a phone." % dev.title,
          "# UPDATE: replaces the image files and boot.img, keeps user data (host partition is not formatted).",
          'OUT=$(dirname "$0")', "adb wait-for-recovery"] + _sh_body(dev, parts, False) + ['echo Done.']
    open(os.path.join(out, "update_keep_data.sh"), "w").write("\n".join(up) + "\n")

    # Windows: a thin wrapper that runs the .sh through WSL/Git Bash, so there is one place to maintain.
    for name in ("install_clean", "update_keep_data"):
        open(os.path.join(out, name + ".bat"), "w", newline="").write(
            "@echo off\r\nREM Runs the shell script with Git Bash or WSL (adb must be in PATH there).\r\n"
            "where bash >nul 2>&1 || (echo bash not found: install Git for Windows or WSL & exit /b 1)\r\n"
            "bash \"%~dp0" + name + ".sh\"\r\n")

    for f in ("get_logs.sh", "get_logs.bat"):
        text = open(os.path.join(REPO, "flash", f), newline="").read()
        text = text.replace("by-name/cache", "by-name/" + (dev.part_name("cache") or "cache"))
        text = text.replace("by-name/userdata", "by-name/" + ud)
        open(os.path.join(out, f), "w", newline="").write(text)
    for f in os.listdir(out):
        if f.endswith(".sh"):
            os.chmod(os.path.join(out, f), 0o755)

    with open(os.path.join(out, "BUILD_INFO.txt"), "w") as fh:
        fh.write(summary)
    info("install_clean.(sh|bat), update_keep_data.(sh|bat), get_logs.(sh|bat), BUILD_INFO.txt")
