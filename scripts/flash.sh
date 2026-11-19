#!/bin/bash
# Flash helper for the Galaxy S9+ in TWRP/OrangeFox (adb). Wraps the scripts generated next to the images.
#
#   scripts/flash.sh backup               pull the stock boot.img (and EFS) to ./backup/   <- do this first
#   scripts/flash.sh check  [out]         compare the image size with the SYSTEM partition
#   scripts/flash.sh install [out]        CLEAN install: formats SYSTEM, pushes images, writes boot.img
#   scripts/flash.sh update [out]         replace images + boot.img, keep what is on SYSTEM and data
#   scripts/flash.sh boot [out]           only write boot.img (after changing the kernel)
#   scripts/flash.sh restore-boot         write backup/boot_stock.img back to BOOT
#   scripts/flash.sh logs                 pull the boot log (CACHE partition) with get_logs.sh
#
# The phone must be in recovery with adb working. Partition names come from devices/star2lte/device.json.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
CMD=${1:-help}; OUT=${2:-$REPO/out}
BK="$REPO/backup"
die() { echo "error: $*" >&2; exit 1; }
[ "$CMD" = help ] || command -v adb >/dev/null || die "adb not found"
dev() { python3 -I -c "import sys; sys.path.insert(0, '$REPO'); from lib.device import Device; d = Device('star2lte'); print(d.$1)"; }
HOST=$(dev host_partition); BOOT=$(dev "part_name('boot')")
part() { adb shell "ls /dev/block/by-name/$1 /dev/block/bootdevice/by-name/$1 2>/dev/null | head -n 1" | tr -d '\r'; }
need_recovery() { adb wait-for-recovery; [ -n "$(part "$BOOT")" ] || die "partition $BOOT not found: is this the S9+ in recovery?"; }

case "$CMD" in
  backup)
    need_recovery; mkdir -p "$BK"
    adb shell "dd if=$(part "$BOOT") of=/tmp/boot_stock.img bs=4096 && sync"
    adb pull /tmp/boot_stock.img "$BK/boot_stock.img"
    for p in EFS PERSISTENT; do                 # IMEI / sensor calibration: small, worth keeping
      [ -n "$(part $p)" ] && adb shell "dd if=$(part $p) of=/tmp/$p.img bs=4096" && adb pull "/tmp/$p.img" "$BK/$p.img" || true
    done
    ls -l "$BK"
    echo "SYSTEM is NOT backed up (it is overwritten by the install). To go back to the stock ROM, keep the"
    echo "firmware for your model and flash it with Odin."
    ;;
  check)
    need_recovery
    SZ=$(adb shell "blockdev --getsize64 $(part "$HOST")" | tr -d '\r')
    TOT=$(du -cb "$OUT"/*.img 2>/dev/null | tail -n 1 | cut -f1)
    echo "images: $((TOT/1048576)) MiB    $HOST partition: $((SZ/1048576)) MiB"
    [ "$TOT" -lt "$SZ" ] || die "the images do not fit into $HOST"
    BS=$(adb shell "blockdev --getsize64 $(part "$BOOT")" | tr -d '\r'); BI=$(stat -c %s "$OUT/boot.img")
    echo "boot.img: $BI bytes    $BOOT partition: $BS bytes"
    [ "$BI" -le "$BS" ] || die "boot.img is larger than the $BOOT partition"
    echo "sizes ok"
    ;;
  install|update)
    [ -f "$OUT/boot.img" ] && [ -f "$OUT/system.img" ] || die "no images in $OUT (run scripts/build_rom.sh)"
    [ -f "$BK/boot_stock.img" ] || die "no stock boot backup: run scripts/flash.sh backup first"
    "$0" check "$OUT"
    if [ "$CMD" = install ]; then bash "$OUT/install_clean.sh"; else bash "$OUT/update_keep_data.sh"; fi
    echo
    [ "$CMD" = install ] && echo "Now in the recovery: Wipe -> Format Data (f2fs) so USERDATA is fresh, then reboot."
    echo "If it does not boot: scripts/flash.sh logs (read CACHE), or scripts/flash.sh restore-boot."
    ;;
  boot)
    need_recovery; [ -f "$OUT/boot.img" ] || die "$OUT/boot.img missing"
    adb push "$OUT/boot.img" /tmp/boot.img
    adb shell "dd if=/tmp/boot.img of=$(part "$BOOT") bs=4096 && sync" && echo "boot.img written"
    ;;
  restore-boot)
    need_recovery; [ -f "$BK/boot_stock.img" ] || die "$BK/boot_stock.img missing"
    adb push "$BK/boot_stock.img" /tmp/boot_stock.img
    adb shell "dd if=/tmp/boot_stock.img of=$(part "$BOOT") bs=4096 && sync" && echo "stock boot restored"
    ;;
  logs)
    need_recovery; bash "$OUT/get_logs.sh"
    ;;
  *) sed -n 2,13p "$0";;
esac
