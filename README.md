# hmos2star2lte

HarmonyOS 5 (OpenHarmony) userspace from a Huawei Pura 70 OTA, repackaged for the **Samsung Galaxy S9+ (star2lte,
Exynos 9810)**. Fork of [tsryzenstech/hmos2sweet](https://github.com/tsryzenstech/hmos2sweet) (Apache-2.0, see
NOTICE.md), which targets the Redmi Note 10 Pro (Snapdragon). Work in progress

What changed compared to hmos2sweet:

- no `super` partition: the HarmonyOS partitions are **ext4 image files** inside one big ext4 partition
  (`SYSTEM`, device.json) and `hmos-preinit` attaches them as loop devices;
- EROFS from the OTA is converted to ext4 (owners, modes and SELinux labels are kept);
- `boot.img` is built from the phone's own boot.img header and command line (`--stock-boot`, `--kernel`);
- install scripts use recovery + adb instead of fastboot;
- only the hardware-independent patches are active; the Snapdragon ones are in `reference/sweet-patches/`.

See [docs/PORTING_STATUS.md](docs/PORTING_STATUS.md) for what works and what has to be written, and
[docs/KERNEL.md](docs/KERNEL.md) for the kernel side (patch in `src/kernel-star2lte/`).

## Use

```
tools/fetch_tools.sh                  # erofs tools (reads the OTA)
build/build_preinit.sh                # hmos-preinit -> prebuilt/ramdisk/init (needs aarch64-linux-gnu-gcc)
python3 hmos2star2lte.py --ota update_full_base.zip --kernel Image.gz-dtb --stock-boot stock_boot.img --out out
```

Needs Linux/WSL with Python 3.8+, `e2fsprogs` >= 1.43 (`mke2fs -d`, `debugfs`, `e2fsck`), about 25 GB free.
Before the first run, check the partition names in `devices/star2lte/device.json` against
`ls -l /dev/block/by-name` in TWRP.

Output: `system.img`, `vendor.img`, ... as ext4, `boot.img`, `install_clean.sh`, `update_keep_data.sh`,
`get_logs.sh`. Boot logs are kept on the CACHE partition by hmos-preinit; `get_logs.sh` pulls them from recovery.
