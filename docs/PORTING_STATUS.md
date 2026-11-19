# star2lte (Galaxy S9+) porting status

Derived from hmos2sweet's patch list. "Reference" patches live in `reference/sweet-patches/` and are NOT loaded;
they show how the sweet (Snapdragon 732G) port solved the same problem.

Legend: ✅ kept as is · 🔧 kept, needs a check · 🆕 must be written for Exynos 9810 · ❌ dropped

## Build pipeline

| Piece | State |
|---|---|
| OTA extraction (`lib/hwota.py`, EROFS read) | ✅ unchanged |
| ext4 images instead of EROFS (`lib/erofs.py`, debugfs for owners/modes/SELinux labels) | ✅ tested on a synthetic tree (e2fsck clean) |
| Device config (`devices/star2lte/device.json`) | 🔧 partition names are guesses (UPPER case as on Samsung): check `ls -l /dev/block/by-name` in TWRP |
| boot.img from the stock header (`--stock-boot`, `--kernel`) | ✅ round-trip tested on a synthetic header; 🔧 real S9+ boot.img untested |
| hmos-preinit loop-image mode (no `super`) | 🔧 compiles (syntax check on x86); never run on a device |
| Install scripts (TWRP + adb, dd to BOOT) | 🔧 generated, `sh -n` clean, never run on a device |

## Patches

| Patch | State | Notes for star2lte |
|---|---|---|
| 00-core | 🔧 | preinit + fstab now come from device.json; kernel is external (see KERNEL.md) |
| 15-no-crash-reboot | ✅ | pure HarmonyOS userspace |
| 20-userauth-no-tee, 21-huks-software, 46-pin-lock | ✅ | no TEE on this phone either |
| 37-kirin-daemons-off | ✅ | removes Kirin-only daemons and their 5 s waits |
| 41-chipset-vsync-stub | ✅ | stops render_service polling the Kirin camera vsync service |
| 50-remove-setup-wizard, 60-debug-tools | ✅ | needed until Wi-Fi works |
| 10-display | 🆕 | DECON composer VDI written against the vendor fbdev (`S3CFB_WIN_CONFIG`), see KERNEL.md; the Huawei DE driver in `composer_host` still powers the panel off on fb0 close |
| 11-gpu-mesa | 🆕 | Mali-G72 (Bifrost): Panfrost instead of freedreno; redo the OHOS EGL/YUV patches against it |
| 12-display-geometry, 13-refresh-120hz | 🆕 | regenerate for 1440x2960, 60 Hz, cutout-free panel; the hooks are the template |
| 14-boot-performance, 16-performance | 🆕 | same idea, Exynos cpufreq/devfreq paths |
| 36-thermal, 38-usb, 33-haptics | 🆕 | Samsung sysfs nodes and USB controller |
| 30-remoteproc | ❌ | Qualcomm-only (qrtr, rmtfs, tqftpserv, pd-mapper); no Exynos equivalent |
| 31-audio | 🆕 | tinyalsa VDI is reusable; mixer paths and the DSP/codec bring-up are Samsung's |
| 32-wifi, 35-bluetooth | 🆕 | Broadcom (bcmdhd, BT over UART) instead of WCN3990 |
| 34-sensors-pd, 39-sensors-hal | 🆕 | Samsung sensor hub instead of the Snapdragon Sensor Core |
| 42-ril | 🆕 | largest item: Samsung IPC modem instead of QMI |
| 44-gnss, 45-nfc | 🆕 | Broadcom GNSS, Samsung NFC chip |
| 47-camera | 🆕 | needs its own bridge to Samsung's camera HAL |
| 43-consumer-ir | ❌ | the S9+ has no IR blaster |
| 22-camera-daemon-idle | 🆕 | add back together with a camera patch |
| 40-hevc-4k-experimental | ❌ | had no effect on sweet either |

## Bring-up findings (as of 2026-11-16)

The phone boots HarmonyOS 5.0 userspace from the loop images and stays up; SceneBoard, apps and the HDF hosts
start. What had to change in `device.json`/the patches and what is still open:

| Symptom | Cause | State |
|---|---|---|
| `init` stops, "policy magic/version mismatch" | the OTA `policydb` is v31, the 4.9 kernel knew v30 | ✅ kernel patch accepts v31, `load_policy success` |
| `/data` mount fails with f2fs | TWRP/userdata on this phone is ext4 | ✅ `"data": {"fs": "ext4"}` |
| second-stage mounts fail, no `/dev/block/by-name` | HarmonyOS ueventd never populates it | ✅ fstab uses `/dev/block/platform/11120000.ufs/by-name/...` (`by_name_dir`) |
| boot times out writing logs to the 7.6 MB CACHE | log budget larger than the partition | ✅ preinit caps the copy at the free space (`free_kb()`) |
| reboots ~10 s after `boot` | the `boot`-triggered `stop watchdog_service` job | ✅ removed, only `bootevent.boot.completed=true` stops it |
| screen black at full brightness | DECON was disabled: Huawei DE driver closes fb0 (`decon_release` -> `decon_disable`); composer never presents a frame | 🔧 fb0 kept open, geometry/WIN_CONFIG/backlight verified; `composer_host` DE driver and the missing client commit remain |
| render_service never commits a frame | no GLES driver for Mali-G72: `libEGL_impl.so`/`libGLESv*_impl.so` in the OTA are symlinks to Huawei's `libhvgr_v200.so` (Maleoon), EGL init fails | ❌ patch 11-gpu-mesa: Mesa + OHOS EGL platform, `softpipe` first (CPU), Panfrost only with a kernel DRM driver |
| lock screen reached after ~15 min, reset 20-30 s later | SceneBoard UI thread blocked 6 s in `requestSystemSceneSession` (sync event), handled on `OS_SceneSession` which is stuck in `RSRenderServiceConnectionProxy::CreateNodeAndSurface`: RenderService does not answer | 🔧 softpipe at 0.75 fps; `RENDER_DIV` lowered render resolution, composer frame timing + RenderService stacks are logged (`docs/TROUBLESHOOTING.md`) |

Logs: CACHE `boot-N/snap*` and `kmsg-*.log` (preinit), `/data/vendor/display/decon-hdi.log` (composer VDI),
`/data/log/hilog/hilog.*.gz` (persister; decode with `tools/_hilog_decode.py`, no args needed for records whose
format string is in the persister dictionary).

## Suggested order

1. Kernel boots to hmos-preinit with the images found (log on CACHE).
2. Display: any picture from SceneBoard, software rendering first.
3. Panfrost GPU, touch.
4. Wi-Fi (needed for the setup wizard, or keep patch 50 on), audio.
5. Modem, sensors, camera last.
