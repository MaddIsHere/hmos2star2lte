# Kernel for HarmonyOS on star2lte

hmos2sweet ships a LineageOS 4.14 kernel with 18 patches (`src/kernel/patches/`). The S9+ has Samsung's 4.9
kernel, so these patches have to be re-applied there. The build here takes the kernel as an input
(`--kernel`), it does not build one.

## What HarmonyOS needs from the kernel (from hmos2sweet's 0001-0018)

| sweet patch | Needed on star2lte? |
|---|---|
| 0001 `/dev/access_token_id`, binder `GET_SENDER_INFO`/`ACCESS_TOKEN` ioctls, `hmos.*` options | yes, backport to 4.9 |
| 0002 binderfs off, sysrq/noreboot | yes (check what the Samsung config already has) |
| 0003 DRM auth for the HALs | only with a DRM display driver |
| 0004/0005 emulated hmdfs/sharefs | yes |
| 0006/0007 fs-verity stub | yes (installd enables code signing) |
| 0009 binder one-way sender pid | yes |
| 0012 `hmos.noreboot=1` | yes |
| 0015 memcg: HarmonyOS memmgr files (hyperhold/zswapd) | yes |
| 0018 reboot log | optional |
| 0008, 0010, 0011, 0013, 0014 (firmware paths, icnss, WLAN BDF, SSR) | no: Qualcomm-specific |
| 0016 vibrator, 0017 /sys/nfc | device specific |

Also needed: ext4 and f2fs built in (the images are ext4, userdata f2fs), loop devices with
`CONFIG_BLK_DEV_LOOP=y` (hmos-preinit uses `/dev/loop-control`), SELinux xattr support on ext4, and either
EROFS or ext4 for the partition images (device.json uses ext4 so EROFS is not required).

## Display

Samsung's 4.9 kernel drives the panel through its own DECON/DPP stack with a vendor HWC interface, not DRM/KMS.
`10-display` (hmos2sweet) is a DRM/KMS composer, so it cannot work here. Instead `src/display/composer/` is a
composer VDI that talks to the vendor framebuffer:

- `DeconFb` opens `/dev/graphics/fb0` once (never closes it: `decon_release()` in `decon_core.c` calls
  `decon_disable()` on every close, not just the last one), reads the geometry with `FBIOGET_VSCREENINFO`
  (1440x2960), powers the panel with `FBIOBLANK` and scans out the client target with
  `S3CFB_WIN_CONFIG` = `_IOW('F', 209, struct decon_win_config_data)`.
- The HDI client (render_service) composes every layer, so only one DECON window (state `2`, buffer) is used,
  plus a colour window for the first "black frame" that leaves `DECON_STATE_INIT`.
- Backlight is `/sys/class/backlight/panel/` (max 44800 on this panel); `max_brightness` scaling is in
  `DeconFb::SetBacklight()`.

The Huawei `displayengine_device` driver (`libdisplayengine_driver.z.so`, loaded by `composer_host` from
`/etc/hdfconfig/hdf_default.hcb`) also opens fb0, issues its own Kirin DE ioctls (`0x20`, `0x21`, `0x83a84413`)
and closes: every close disables the panel again. It must be removed from the host config (or the kernel must
stop disabling on close) before the panel can stay lit under HarmonyOS.

Bring-up trace: the VDI appends to `/data/vendor/display/decon-hdi.log` (created by
`/etc/init/displayengine.cfg`) and to `/dev/kmsg`. A test boot shows the library loading, `CreateComposerVdiV1_1`,
fb0 opened as fd 45, `1440x2960`, `FBIOBLANK(0)` and the first `WIN_CONFIG` all succeeding, two
`SetDisplayPowerStatus(ON)` calls from the client - and no `Present` entry, i.e. render_service never commits a
client buffer to the composer yet.

Why nothing is presented: the same boot's hilog has `utils_base: path (/system/lib64/libEGL_impl.so) to realpath
error` and then the `InitializeEglContext`/`SetUpGpuContext` warnings. OpenHarmony's `/system/lib64/libEGL.so` is a
wrapper that loads the vendor driver from `/vendor/lib64/chipsetsdk/libEGL_impl.so` (+ `libGLESv*_impl.so`), and
in the Huawei OTA those are symlinks to `libhvgr_v200.so`, the Maleoon GPU driver. There is no GLES implementation
for Mali-G72 in the OTA, so render_service cannot create a context and produces no frame at all. A Mesa build with
the OHOS EGL platform (`src/mesa/0001-mesa-ohos-platform-for-sweet.patch`, `build/build_mesa.sh`) is needed;
start with the `softpipe`/`llvmpipe` gallium driver (no GPU kernel driver required, CPU rendering), then Panfrost
only once the kernel has a panfrost DRM driver (the 4.9 Samsung kernel has the Mali vendor driver, not DRM).

## Command line

`--stock-boot` supplies the phone's own command line and header addresses; `device.json` appends
`hmos.imgpart`, `hmos.imgdir`, `hmos.imgfs`, `hmos.logpart` and the generic `hmos.*` options. The stock
`androidboot.*` entries are kept, so keep the S9+'s bootloader-required ones.

## Findings from the phone (stock 4.9.118 config.gz + boot header, 2026-11-05)

Boot header: v0, page size 2048, kernel 37.7 MB (uncompressed Image), ramdisk 3.1 MB, **dt_size 309248 inside boot.img**
(the DTB/DTBO partitions exist too, but the loader also gets this table), kernel addr 0x10008000, ramdisk 0x11000000,
tags 0x10000100, cmdline `buildvariant=userdebug`. `--stock-boot` must be the full boot.img: the device tree is reused.

Already there: loop (8 devices, built in), ext4 + security + ACL, f2fs + security, SELinux, binder (binder, hwbinder,
vndbinder), memcg, namespaces, devtmpfs, FUSE, ashmem, ION, dm-verity, Mali Bifrost r19p0 (vendor driver, not Panfrost).

Missing, in order of importance: `CONFIG_DRM` (display is the Samsung DECON framebuffer: CONFIG_FB, EXYNOS_DECON_9810),
EROFS (not needed, images are ext4), fs-verity, ZRAM (HarmonyOS memmgr/zswapd expects it), overlayfs,
the OpenHarmony binder/access_token_id/hmdfs/sharefs patches. `CONFIG_AUDIT` is off (fine, `audit=0`).
Samsung's FIVE/integrity is on; no RKP/TIMA/UH in this config, so a rebuilt kernel should boot.

## Kernel: duhansysl/exynos9810-kernel (4.9.337) + OpenHarmony patch

`src/kernel-star2lte/0001-ohos-support-exynos9810-4.9.337.patch` is a port of hmos2sweet's kernel patches
0001, 0004-0007, 0009, 0012, 0015 and 0018 (4.14 -> 4.9.337). It applies cleanly to upstream commit `914eabe7b`
(apollo V1.12, 2026-05-06) and the touched files compile with clang 18 (selinuxfs, reboot, fork, sysrq, ioctl,
namespace, binder, accesstokenid, dm-ioctl, memcontrol). **Not booted.**

Not ported on purpose: 0002/0003 (DRM display auth: this kernel has no DRM), 0008/0010/0011/0013/0014 (Qualcomm
firmware, icnss and subsystem restart), 0016/0017 (sweet's vibrator and NFC chips; the S9+ has its own).
`CONFIG_OHOS_DM_PARTNAME` stays off (the loop layout needs no dm names).

Config: build `exynos9810_defconfig`, then merge `star2lte_defconfig`, then `arch/arm64/configs/vendor/ohos.config`
(enables ACCESS_TOKENID, BINDER_SENDER_INFO, OHOS_FS_EMU, OHOS_FSVERITY_STUB, the bring-up switches, pstore, and
turns ANDROID_PARANOID_NETWORK off). Merged, the config also has ZRAM, PSI, overlayfs, f2fs, SELinux develop,
loop (48 devices) and Mali Bifrost r38p1.

```
git clone https://github.com/duhansysl/exynos9810-kernel && cd exynos9810-kernel
git checkout 914eabe7b            # the commit the patch was tested against
git apply /path/to/0001-ohos-support-exynos9810-4.9.337.patch
make exynos9810_defconfig
scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/star2lte_defconfig arch/arm64/configs/vendor/ohos.config
make olddefconfig
```
(or put the three configs into the Apollo `apollo.sh` flow.) Notes from the build here: `make *_defconfig` runs
`scripts/fetch-latest-wireguard.sh` and deletes `net/wireguard` if the download fails; run `touch net/wireguard/.check`
first when building offline. The `KernelSU-Next` submodule must be present.

The phone currently runs 4.9.118 with Mali r19p0 (older release of the same line): rebuild and flash this kernel
together with its matching DTB, do not mix it with the stock DTB/DTBO partitions' vendor modules.
