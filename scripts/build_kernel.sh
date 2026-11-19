#!/bin/bash
# Build the Galaxy S9+ (star2lte) kernel with the OpenHarmony patch.
#
#   scripts/build_kernel.sh                 # clone, patch, configure, build -> kernel-out/Image + kernel-out/dtb.img
#   scripts/build_kernel.sh --configure     # stop after the configuration (fast check)
#
# Environment (all optional):
#   KERNEL_DIR   where the kernel source lives      (default: <repo>/kernel-build/exynos9810-kernel)
#   KERNEL_REPO  git URL to clone                   (default: https://github.com/duhansysl/exynos9810-kernel)
#   KERNEL_REV   commit the patch was made against  (default: 914eabe7b)
#   CR_COMPILER  0..8, which clang to use (downloaded to $CR_TC, default ~/toolchains); asks if unset. See the
#                toolchain section below. 4 = Android clang 20 r547379 (default, DS-ACK's compiler).
#   CLANG_DIR    use this clang directory instead (has bin/clang, bin/ld.lld)
#   JOBS         parallel jobs                      (default: all cores)
#   OFFLINE=1    do not let the kernel try to download WireGuard
#
# Needs: git, make, clang + lld + llvm tools (llvm-ar/nm/objcopy/objdump/strip), gcc (host tools), bc, bison,
#        flex, libssl-dev, libelf-dev, python3, device-tree-compiler (dtc is built from the tree).
set -euo pipefail

say() { printf '\n==> %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
PATCH="$REPO/src/kernel-star2lte/0001-ohos-support-exynos9810-4.9.337.patch"
# The kernel tree must be on a Linux filesystem: on /mnt/c, /mnt/e ... (NTFS) git reports a fresh clone as modified
# (no file modes, case-insensitive names such as xt_CONNMARK.h/xt_connmark.h) and the build is very slow.
case "$REPO" in /mnt/*) DEFAULT_KDIR=$HOME/hmos2star2lte-kernel/exynos9810-kernel;; *) DEFAULT_KDIR=$REPO/kernel-build/exynos9810-kernel;; esac
KERNEL_DIR=${KERNEL_DIR:-$DEFAULT_KDIR}
case "$KERNEL_DIR" in /mnt/*) die "KERNEL_DIR=$KERNEL_DIR is on a Windows drive: use a path inside WSL, e.g. \$HOME/exynos9810-kernel";; esac
KERNEL_REPO=${KERNEL_REPO:-https://github.com/duhansysl/exynos9810-kernel}
KERNEL_REV=${KERNEL_REV:-914eabe7b}
JOBS=${JOBS:-$(nproc)}
OUT="$REPO/kernel-out"
CONFIGURE_ONLY=0
[ "${1:-}" = "--configure" ] && CONFIGURE_ONLY=1


for t in git make gcc bc python3; do command -v "$t" >/dev/null || die "missing tool: $t"; done
for t in bison flex; do command -v "$t" >/dev/null || echo "warning: $t not found (only needed if a build step regenerates parsers)"; done
[ -f "$PATCH" ] || die "patch not found: $PATCH"

# ---- toolchain --------------------------------------------------------------------------------------------
# CR_COMPILER picks the clang (downloaded on first use into $CR_TC); unset = ask (4 = clang 20 when not interactive).
#   0 clang from PATH (the distro one)         4 Android clang 20  r547379
#   1 Android clang 12.0.4 r416183             5 Neutron clang 18 (05012024)
#   2 Android clang 14.0.4 r450784             6 Neutron clang 19 (10032024)
#   3 Android clang 18.0.1 r522817 (apollo)    7 neutron-clang-20.0.0 (already in $CR_TC, not downloaded)
#                                              8 clang-custom (already in $CR_TC, not downloaded)
# CLANG_DIR=/path still works and overrides the choice.
CR_TC=${CR_TC:-$HOME/toolchains}
if [ -z "${CLANG_DIR:-}" ]; then
  if [ -z "${CR_COMPILER:-}" ]; then
    if [ -t 0 ]; then
      cat <<'MENU'
Choose the compiler:
  0) clang from PATH (distro)
  1) Android clang 12.0.4 r416183
  2) Android clang 14.0.4 r450784
  3) Android clang 18.0.1 r522817   (what apollo uses)
  4) Android clang 20.0.0 r547379   (default, what DS-ACK uses)
  5) Neutron clang 18.0.0
  6) Neutron clang 19.0.0
  7) Neutron clang 20.0.0 (must already be in $CR_TC/neutron-clang-20.0.0)
  8) custom (must already be in $CR_TC/clang-custom)
MENU
      read -r -p "CR_COMPILER [4]: " CR_COMPILER
    fi
    CR_COMPILER=${CR_COMPILER:-4}
  fi
  CR_CLANG_URL=""
  case "$CR_COMPILER" in
    0) CR_CLANG="" ;;
    1) CR_CLANG_URL=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/llvm-r416183/clang-r416183.tar.gz
       CR_CLANG=$CR_TC/clang-12.0.4-r416183 ;;
    2) CR_CLANG_URL=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/llvm-r450784/clang-r450784b.tar.gz
       CR_CLANG=$CR_TC/clang-14.0.4-r450784 ;;
    3) CR_CLANG_URL=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/llvm-r522817/clang-r522817.tar.gz
       CR_CLANG=$CR_TC/clang-18.0.1-r522817 ;;
    4) CR_CLANG_URL=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r547379.tar.gz
       CR_CLANG=$CR_TC/clang-20.0.0-r547379 ;;
    5) CR_CLANG_URL=https://github.com/Neutron-Toolchains/clang-build-catalogue/releases/download/05012024/neutron-clang-05012024.tar.zst
       CR_CLANG=$CR_TC/neutron-clang-18.0.0 ;;
    6) CR_CLANG_URL=https://github.com/Neutron-Toolchains/clang-build-catalogue/releases/download/10032024/neutron-clang-10032024.tar.zst
       CR_CLANG=$CR_TC/neutron-clang-19.0.0 ;;
    7) CR_CLANG=$CR_TC/neutron-clang-20.0.0 ;;
    8) CR_CLANG=$CR_TC/clang-custom ;;
    *) die "CR_COMPILER must be 0..8 (got '$CR_COMPILER')" ;;
  esac
  if [ -n "$CR_CLANG" ] && [ ! -x "$CR_CLANG/bin/clang" ]; then
    [ -n "$CR_CLANG_URL" ] || die "CR_COMPILER=$CR_COMPILER expects a toolchain in $CR_CLANG (no download for this one)"
    say "downloading clang to $CR_CLANG"
    command -v curl >/dev/null || die "curl is needed to download the toolchain"
    case "$CR_CLANG_URL" in *.zst) command -v zstd >/dev/null || die "zstd is needed: sudo apt install zstd";; esac
    TMPD="$CR_CLANG.part"; rm -rf "$TMPD"; mkdir -p "$TMPD"
    case "$CR_CLANG_URL" in
      *.zst) curl -fL --retry 3 "$CR_CLANG_URL" | zstd -dc | tar -x -C "$TMPD" ;;
      *)     curl -fL --retry 3 "$CR_CLANG_URL" | tar -xz -C "$TMPD" ;;
    esac
    # some archives are flat, some have one top-level directory
    if [ ! -x "$TMPD/bin/clang" ]; then
      sub=$(find "$TMPD" -mindepth 2 -maxdepth 2 -path '*/bin/clang' -print -quit)
      [ -n "$sub" ] || die "downloaded archive has no bin/clang"
      TMPD=$(dirname "$(dirname "$sub")")
    fi
    rm -rf "$CR_CLANG"; mv "$TMPD" "$CR_CLANG"; rm -rf "$CR_CLANG.part"
  fi
  CLANG_DIR=$CR_CLANG
fi
if [ -n "${CLANG_DIR:-}" ]; then
  [ -x "$CLANG_DIR/bin/clang" ] || die "CLANG_DIR=$CLANG_DIR has no bin/clang"
  export PATH="$CLANG_DIR/bin:$PATH"
fi
command -v clang >/dev/null  || die "clang not found (install clang+lld, or set CR_COMPILER)"
command -v ld.lld >/dev/null || die "ld.lld not found (install lld, or set CR_COMPILER)"
say "clang: $(clang --version | head -n 1)"

# ---- source -----------------------------------------------------------------------------------------------
if [ ! -d "$KERNEL_DIR/.git" ]; then
  say "cloning $KERNEL_REPO"
  mkdir -p "$(dirname "$KERNEL_DIR")"
  git clone "$KERNEL_REPO" "$KERNEL_DIR"
fi
cd "$KERNEL_DIR"
if git apply -R --check "$PATCH" 2>/dev/null; then
  say "patch already applied"
else
  say "checking out $KERNEL_REV"
  git diff --quiet && git diff --cached --quiet || die "$KERNEL_DIR has local changes: commit or stash them first"
  git checkout -q "$KERNEL_REV"
  git apply --check "$PATCH" || die "the patch does not apply to $KERNEL_REV (wrong revision?)"
  say "applying $(basename "$PATCH")"
  git apply "$PATCH"
fi

# Further patches (applied on top of 0001, each one only if it is not in the tree yet):
#   0002  SELinux: read policy version 31 (HarmonyOS ships policy.31, the 4.9 kernel stops at 30)
for P in "$REPO"/src/kernel-star2lte/000[2-9]-*.patch; do
  [ -f "$P" ] || continue
  if git apply -R --check "$P" 2>/dev/null; then
    say "$(basename "$P"): already applied"
  else
    git apply --check "$P" || die "$(basename "$P") does not apply to this tree"
    say "applying $(basename "$P")"
    git apply "$P"
  fi
done

# statx(2) for HarmonyOS code signing (4.9 has none; libcode_sign_utils needs it, see add_statx.py). Idempotent.
python3 "$REPO/src/kernel-star2lte/add_statx.py" "$KERNEL_DIR" || die "add_statx.py failed"

# Apollo's tree links drivers/kernelsu -> ../KernelSU-Next/kernel (a submodule without a URL in .gitmodules)
if [ ! -f KernelSU-Next/kernel/Kconfig ] && [ ! -f KernelSU-Next/Kconfig ] && [ ! -f drivers/kernelsu/Kconfig ]; then
  say "fetching KernelSU-Next (Kconfig of drivers/kernelsu needs it)"
  rm -rf KernelSU-Next
  git clone --depth 1 https://github.com/KernelSU-Next/KernelSU-Next KernelSU-Next
  rm -rf KernelSU-Next/.git
fi
# The tree's Makefile runs scripts/fetch-latest-wireguard.sh, which deletes net/wireguard if the download fails
if [ "${OFFLINE:-0}" = 1 ]; then touch net/wireguard/.check; fi

# ---- build ------------------------------------------------------------------------------------------------
export ARCH=arm64 SUBARCH=arm64 ANDROID_MAJOR_VERSION=q PLATFORM_VERSION=13.0.0
export CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump
export READELF=llvm-readelf STRIP=llvm-strip KALLSYMS_EXTRA_PASS=1
export KCFLAGS="${KCFLAGS:-} -Wno-error -fintegrated-as" KAFLAGS="${KAFLAGS:-} -fintegrated-as"
# without a GNU aarch64 binutils the triple still has to be named; the 32-bit vDSO is not built
# The tools must be given on the make command line: the Makefile sets LD = $(CROSS_COMPILE)ld itself and that
# overrides environment variables (symptom: "aarch64-linux-gnu-ld: not found").
SHIM="$OUT/.toolshim"; mkdir -p "$SHIM"
for t in readelf nm objdump objcopy ar strip ranlib; do
  l=$(command -v llvm-$t || true); [ -n "$l" ] && ln -sf "$l" "$SHIM/aarch64-linux-gnu-$t"
done
export PATH="$SHIM:$PATH"
M=(make -j"$JOBS" ARCH=arm64 CC=clang HOSTCC=gcc CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-
   CLANG_TRIPLE=aarch64-linux-gnu- LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump
   READELF=llvm-readelf STRIP=llvm-strip)

say "configuring: exynos9810_defconfig + star2lte_defconfig + vendor/ohos.config"
"${M[@]}" exynos9810_defconfig
./scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/star2lte_defconfig \
    arch/arm64/configs/vendor/ohos.config >/dev/null
# The 32-bit compat vDSO needs a separate ARM32 clang/binutils ("arm-linux-gnueabi-clang: not found"). HarmonyOS is
# 64-bit and 32-bit tasks work without it, so it is switched off unless COMPAT_VDSO=1 (then CROSS_COMPILE_ARM32 must work).
if [ "${COMPAT_VDSO:-0}" != 1 ]; then ./scripts/config --file .config -d COMPAT_VDSO; fi
# Without a device target the kernel builds no device tree ("DTBTOOL ... error: no dtb files found").
# apollo.sh exports one MACH_EXYNOS9810_* per model; G965F (international) is the default, VARIANT=kor gives G965N.
case "${VARIANT:-eur}" in
  eur) MACH=STAR2LTE_EUR_OPEN ;;
  kor) MACH=STAR2LTE_KOR ;;
  *) die "VARIANT must be eur (SM-G965F/FD, default) or kor (SM-G965N)" ;;
esac
./scripts/config --file .config -d MACH_EXYNOS9810_NONE -e MACH_EXYNOS9810_$MACH -e EXYNOS_DTBTOOL
# EXYNOS_FMP_FIPS (kept on): after the link scripts/fmp/fips_fmp_integrity.py calls "$CROSS_COMPILE"readelf, which the
# tool shim below maps to llvm-readelf. Do NOT turn it off: crypto/testmgr.c does not compile without it (Samsung's
# diskcipher self-test uses struct fields that do not exist: "no member named 'ptext'").
# Samsung's bootloader builds a command line of about 2 KB and the kernel cuts it at 2048 bytes, which drops whatever
# the boot.img header adds at the end (hmos.noreboot, hmos.permissive, ...). CONFIG_CMDLINE is placed FIRST, so the
# bring-up switches live there. ramoops uses the (unused) exynos_ss kernel-log area of the device tree as a RAM console:
# after a panic the next boot finds the crash log in /sys/fs/pstore (hmos-preinit copies it to cache:/hmos/pstore-N). Device-specific options (image partition, ...) go through /hmos.conf in the ramdisk.
./scripts/config --file .config --set-str CMDLINE "loop.max_part=7 hmos.noreboot=1 hmos.permissive=1 enforcing=0 audit=0 printk.devkmsg=on ramoops.mem_address=0xfda10000 ramoops.mem_size=0x200000 ramoops.console_size=0x100000 ramoops.record_size=0x20000 ramoops.pmsg_size=0x20000 ramoops.dump_oops=1"
# SELinux permissive by default: the kernel starts permissive unless ALWAYS_ENFORCE is on; the command line also
# carries enforcing=0 and hmos.permissive=1 (refuses set-enforce 1). ENFORCE=1 keeps the stock behaviour.
if [ "${ENFORCE:-0}" != 1 ]; then ./scripts/config --file .config -d ALWAYS_ENFORCE -e SECURITY_SELINUX_DEVELOP -e OHOS_BRINGUP_PERMISSIVE; fi
"${M[@]}" olddefconfig
for o in ACCESS_TOKENID BINDER_SENDER_INFO OHOS_FS_EMU OHOS_FSVERITY_STUB OHOS_BRINGUP_NOREBOOT \
         OHOS_BRINGUP_PERMISSIVE ZRAM PSI SECURITY_SELINUX_DEVELOP BLK_DEV_LOOP; do
  grep -q "^CONFIG_$o=y" .config || die "CONFIG_$o is not enabled after merging: check the config"
done
[ "${ENFORCE:-0}" = 1 ] || { ! grep -q "^CONFIG_ALWAYS_ENFORCE=y" .config || die "CONFIG_ALWAYS_ENFORCE is on: SELinux would be enforcing"; }
grep -q "^CONFIG_MACH_EXYNOS9810_$MACH=y" .config || die "device target MACH_EXYNOS9810_$MACH did not stick"
echo "config ok (OpenHarmony options, ZRAM, PSI, loop are on)"
[ $CONFIGURE_ONLY = 1 ] && exit 0

say "building (this takes a while)"
"${M[@]}"
[ -f arch/arm64/boot/Image ]   || die "arch/arm64/boot/Image was not produced"
[ -f arch/arm64/boot/dtb.img ] || die "arch/arm64/boot/dtb.img was not produced (dtbtool_exynos step)"

mkdir -p "$OUT"
cp arch/arm64/boot/Image "$OUT/Image"
cp arch/arm64/boot/dtb.img "$OUT/dtb.img"
cp .config "$OUT/kernel.config"
say "done"
ls -l "$OUT"
echo
echo "Image + dtb.img must fit into the BOOT partition together with the ramdisk (check: blockdev --getsize64"
echo "/dev/block/by-name/BOOT in TWRP). Next: scripts/build_rom.sh"
