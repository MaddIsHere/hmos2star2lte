# hmos2star2lte

HarmonyOS 5 (OpenHarmony) userspace from a Huawei Pura 70 OTA, repackaged for the **Samsung Galaxy S9+ (star2lte,
Exynos 9810)**. Fork of [tsryzenstech/hmos2sweet](https://github.com/tsryzenstech/hmos2sweet) (Apache-2.0, see
NOTICE.md), which targets the Redmi Note 10 Pro (Snapdragon).

A very early prototype, modified for the sole purpose of booting, not stable and won't be.

What changed compared to hmos2sweet:

- no `super` partition: the HarmonyOS partitions are **ext4 image files** inside one big ext4 partition
  (`SYSTEM`, device.json) and `hmos-preinit` attaches them as loop devices;
- EROFS from the OTA is converted to ext4 (owners, modes and SELinux labels are kept);
- `boot.img` is built from the phone's own boot.img header and command line (`--stock-boot`, `--kernel`);
- install scripts use recovery + adb instead of fastboot;
- only the hardware-independent patches are active; the Snapdragon ones are in `reference/sweet-patches/`.

See [docs/PORTING_STATUS.md](docs/PORTING_STATUS.md) for what works and what has to be written, and
[docs/KERNEL.md](docs/KERNEL.md) for the kernel side (patch in `src/kernel-star2lte/`).

## Status
| Feature | Status |
| --- | --- |
| Boot, boot animation, SceneBoard, launcher, setup wizard | ✅ Works |
| Display, touch | ✅ Works |
| Volume Up/Down/Power | ✅ Works |
| Boot on S9 & N9 | ⚠️ Probably |
| Boot on 4GB of RAM | ⚠️ Probably |
| GPU | ❌ No |
| Everything else | ❌ No |


## Building

```
tools/fetch_tools.sh                  # erofs tools (reads the OTA)
build/build_preinit.sh                # hmos-preinit -> prebuilt/ramdisk/init (needs aarch64-linux-gnu-gcc)
python3 hmos2star2lte.py --ota update_full_base.zip --kernel Image.gz-dtb --stock-boot stock_boot.img --out out
```

Needs Linux/WSL with Python 3.8+, `e2fsprogs` >= 1.43 (`mke2fs -d`, `debugfs`, `e2fsck`), about 25 GB free.
Before the first run, check the partition names in `devices/star2lte/device.json` against
`ls -l /dev/block/by-name` in TWRP.

Output: `system.img`, `vendor.img`, ... as ext4, `boot.img`, `install_clean.sh`, `update_keep_data.sh`,

## Flashing

!! The `SYSTEM` partition on the phone MUST be at least 11.3GB. !!                                                                                        
!! RUN IN ORDER !!
Replace "/path/to/...img" with your path!
```
adb shell "mkdir -p /mnt/hmos && mount -t ext4 /dev/block/by-name/SYSTEM /mnt/hmos && mkdir -p /mnt/hmos/hmosimg"
adb push /path/to/system.img /mnt/hmos/hmosimg
adb push /path/to/vendor.img /mnt/hmos/hmosimg
adb push /path/to/chip_prod.img /mnt/hmos/hmosimg
adb push /path/to/sys_prod.img /mnt/hmos/hmosimg
adb push /path/to/cust.img /mnt/hmos/hmosimg
adb push /path/to/patch.img /mnt/hmos/hmosimg
adb shell "sync; umount /mnt/hmos"
adb push path/to/boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=4096 && sync"
adb shell reboot
```

## Getting logs

Make sure to create hmos-logs folder first.                                                                                                                            

WINDOWS:
You can change the path! not restricted to just C:\

```
mkdir C:\hmos-logs
adb shell "mkdir -p /tmp/c && mount -t ext4 /dev/block/by-name/CACHE /tmp/c"
adb pull /tmp/c/hmos C:\hmos-logs\cache
adb pull /sys/fs/pstore C:\hmos-logs\pstore
adb shell "dmesg" > C:\hmos-logs\recovery_dmesg.txt
adb pull /data/log/faultlog "C:\hmos-logs\faultlog"
adb pull /data/log/hilog "C:\hmos-logs\hilog"
```

LINUX:
`get_logs.sh`. Boot logs are kept on the CACHE partition by hmos-preinit; `get_logs.sh` pulls them from recovery.

## Credits:
tsryzenstech                                                                                                                                                                                                                                          
duhansysl
