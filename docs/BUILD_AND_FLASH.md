# Build and flash (Galaxy S9+ / star2lte)

Status: the kernel patch compiles and the configuration is verified; the scripts are checked up to the
configuration step. **Nothing has been booted.** Read each script before you run it: they write to partitions.
Linux or WSL2 is needed for the build; the phone is flashed from TWRP/OrangeFox with adb.

## 0. What you need

| For | Packages (Debian/Ubuntu names) |
|---|---|
| kernel | `git make gcc bc bison flex libssl-dev libelf-dev python3 clang lld llvm` (clang 12-20; Android clang r522817 as used by `apollo.sh` also works: set `CLANG_DIR`) |
| images | `e2fsprogs` (>= 1.43), `python3`, `gcc-aarch64-linux-gnu` (hmos-preinit), `curl` |
| flashing | `adb`, TWRP/OrangeFox on the phone |
| disk | about 25 GB for the images, 8 GB for the kernel tree and build |
| OTA | the Huawei Pura 70 full OTA zip (`update_full_base.zip`, see `fetchfw.py`) |

## 1. Compile the kernel with the patch (what `scripts/build_kernel.sh` does)

```
scripts/build_kernel.sh --configure      # quick check: clone, patch, configure
scripts/build_kernel.sh                  # full build -> kernel-out/Image and kernel-out/dtb.img
```

By hand, the same steps:

```
git clone https://github.com/duhansysl/exynos9810-kernel && cd exynos9810-kernel
git checkout 914eabe7b                                   # the commit the patch was made and tested against
git apply /path/to/hmos2star2lte/src/kernel-star2lte/0001-ohos-support-exynos9810-4.9.337.patch

# the tree links drivers/kernelsu -> ../KernelSU-Next/kernel; make sure that directory exists:
git clone --depth 1 https://github.com/KernelSU-Next/KernelSU-Next KernelSU-Next && rm -rf KernelSU-Next/.git

export ARCH=arm64 SUBARCH=arm64 ANDROID_MAJOR_VERSION=q PLATFORM_VERSION=13.0.0
export KCFLAGS="-Wno-error -fintegrated-as" KAFLAGS="-fintegrated-as"
M="make -j$(nproc) ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- CLANG_TRIPLE=aarch64-linux-gnu- LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump READELF=llvm-readelf STRIP=llvm-strip"

$M exynos9810_defconfig                                            # base config
scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/star2lte_defconfig \
                                           arch/arm64/configs/vendor/ohos.config
scripts/config --file .config -d COMPAT_VDSO      # the 32-bit vDSO needs an ARM32 toolchain, not needed here
$M olddefconfig
$M                                                                 # Image + dtb.img
# result: arch/arm64/boot/Image  and  arch/arm64/boot/dtb.img
```

Notes
- `make exynos9810_defconfig star2lte_defconfig` does **not** merge: the second replaces the first. Use the merge above.
- Offline builds: the tree's Makefile runs `scripts/fetch-latest-wireguard.sh`, which **deletes `net/wireguard` when the
  download fails**. `touch net/wireguard/.check` first (the script's own freshness stamp) or use `OFFLINE=1`.
  If it already happened: `git checkout -- net/wireguard`.
- Check the result: `grep -E "ACCESS_TOKENID|OHOS_|ZRAM|PSI" .config` should show the OpenHarmony options as `=y`.
- The old kernel's `/proc/config.gz` said 4.9.118 and Mali r19p0; this one is 4.9.337 with r38p1. Always use the
  `dtb.img` built with the same tree (the boot image carries it, see step 4), not the one from your old boot.img.
- If your usual way of building this kernel is `apollo.sh` (menu): apply the patch and the two config fragments to the
  tree first, then build as usual; the outputs are the same two files.

## 2. Prepare the phone and back up

Boot TWRP/OrangeFox, enable adb, then:

```
scripts/flash.sh backup        # boot_stock.img (+ EFS, PERSISTENT) -> ./backup/
```

`SYSTEM` (sda18) is **overwritten** by the install, because the images live in it as files. Keep the firmware for your
model for Odin if you want to go back to the stock ROM. Your own boot.img with the 4.9.118 kernel is in `backup/`.

## 3. Build the images (`scripts/build_rom.sh`)

```
scripts/build_rom.sh --ota /path/update_full_base.zip --stock-boot backup/boot_stock.img
```

This extracts the OTA, converts the EROFS partitions to ext4, applies the active patches, builds hmos-preinit and
writes `boot.img` from your stock header (page size, load addresses, command line) with `kernel-out/Image` and
`kernel-out/dtb.img`. Output in `out/`: `system.img`, `vendor.img`, ... `boot.img`, install and log scripts.

## 4. Flash

```
scripts/flash.sh check         # do the images fit in SYSTEM, does boot.img fit in BOOT?
scripts/flash.sh install       # first time: formats SYSTEM, pushes the images, writes BOOT
# then in the recovery: Wipe -> Format Data (USERDATA fresh f2fs), reboot
```

Later builds: `scripts/flash.sh update` (images + boot.img, nothing formatted) or `scripts/flash.sh boot` (only a new
kernel). Back to your old kernel: `scripts/flash.sh restore-boot` (the old kernel then boots the *old* Android
only if SYSTEM still holds it, so this is for testing kernels before the first install).

## 5. First boot and logs

What a healthy first stage looks like: the phone leaves the Samsung logo, `hmos-preinit` runs (no picture yet:
display is not done), and writes its log to CACHE. In recovery:

```
scripts/flash.sh logs          # runs out/get_logs.sh, files appear in ./logs/
```

Look for `starting (HarmonyOS userspace, stage 1)`, `images from /dev/block/sda18 (ext4)` and one
`mapped <name> -> /dev/block/loopN` line per image. Send me those logs (and `/sys/fs/pstore` if the kernel panics):
that tells us whether the kernel, the loop mapping or HarmonyOS init is the next problem.

Quick fixes
- Black screen and no log at all: the kernel did not start. Check that `boot.img` fits `BOOT`, that the `dtb.img` comes
  from the same build, and restore with `flash.sh restore-boot`.
- `image partition SYSTEM not found`: partition names differ, fix `devices/star2lte/device.json`.
- `mount ... failed`: the image partition is not ext4 or `hmos.imghostfs` is wrong.
