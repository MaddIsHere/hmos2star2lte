#!/bin/bash
# Build the HarmonyOS images + boot.img for the Galaxy S9+ from a Pura 70 OTA and the kernel from build_kernel.sh.
#
#   scripts/build_rom.sh --ota update_full_base.zip --stock-boot backup/boot_stock.img [--out out] [-- extra args]
#
# Needs kernel-out/Image and kernel-out/dtb.img (scripts/build_kernel.sh), Linux/WSL, e2fsprogs >= 1.43,
# aarch64-linux-gnu-gcc (for hmos-preinit), about 25 GB free. Extra args after "--" go to hmos2star2lte.py
# (for example --no-setup-wizard or --patches ...).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
OTA=""; STOCK=""; OUT="$REPO/out"; EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --ota) OTA=$2; shift 2;;
    --stock-boot) STOCK=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --) shift; EXTRA=("$@"); break;;
    *) echo "unknown argument: $1" >&2; exit 1;;
  esac
done
die() { echo "error: $*" >&2; exit 1; }
[ -f "$OTA" ]   || die "--ota <full OTA zip> is required"
[ -f "$STOCK" ] || die "--stock-boot <the phone's boot.img> is required (see docs/BUILD_AND_FLASH.md, step 3)"
[ -f "$REPO/kernel-out/Image" ] && [ -f "$REPO/kernel-out/dtb.img" ] || die "run scripts/build_kernel.sh first"
for t in mke2fs e2fsck debugfs python3; do command -v "$t" >/dev/null || die "missing tool: $t (apt install e2fsprogs python3)"; done

cd "$REPO"
[ -x tools/extract.erofs ] || [ -x tools/bin/extract.erofs ] || { echo "==> fetching erofs tools"; bash tools/fetch_tools.sh; }
if true; then   # always rebuild: it takes seconds and a stale init is hard to notice
  command -v aarch64-linux-gnu-gcc >/dev/null || die "aarch64-linux-gnu-gcc missing (apt install gcc-aarch64-linux-gnu)"
  echo "==> building hmos-preinit"; bash build/build_preinit.sh
fi

echo "==> building images"
python3 hmos2star2lte.py --ota "$OTA" --kernel kernel-out/Image --dtb kernel-out/dtb.img \
    --stock-boot "$STOCK" --out "$OUT" "${EXTRA[@]}"
echo
echo "Output in $OUT. Next: scripts/flash.sh backup, then scripts/flash.sh install"
