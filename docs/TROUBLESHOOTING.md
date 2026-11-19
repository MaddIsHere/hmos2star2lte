# Known issues and logs

## Reading the logs

Every boot leaves in the `cache` partition (`/log/hmos` on HarmonyOS):

| File | Content |
|---|---|
| `kmsg-N.log` | kernel log; the slot rotates (0...19), so the previous boots are kept until the partition is full |
| `boot-N/cmdline.txt` | `/proc/cmdline` of that boot: `sec_debug.reset_reason`/`rst_stat` say how the *previous* boot ended |
| `boot-N/hilog-stream-*.txt` | hilog |
| `boot-N/snapN-stacks.txt` | kernel stack/wchan of `render_service`, `composer_host`, `display_engine_service`, SceneBoard |
| `snapN-*.txt` | processes, threads, services, mounts, parameters, GPU/display state |
| `screen-*.jpeg` | screenshots |

HarmonyOS's own logs are in `/data/log` (hilog, faultlog). To download everything: recovery (Power + Volume up),
then `get_logs.bat` / `get_logs.sh`. `pstore/` holds the last kernel messages before a reboot.

Useful lines in kmsg:

- `sweet-rproc:` firmware partition, ADSP, modem, `wlan0`, sound card, mobile data;
- `sweet-oobedone:` setup keys written into the settings database;
- `sweet-sensors:` sensors HAL (SSC service found, SUID of every sensor, errors);
- `hmos: power off requested by` / `hmos: reboot("...") requested by`: who asked for a power off or a restart;
- `hmos-preinit: androidboot.mode=charger`: the phone started in off-mode charging;
- `cambridge:` / `sweetcam-launch:` camera bridge (work in progress).

Useful hilog tags:

- `SWEET_AUDIO`: audio HAL;
- `composer_host`: display;
- `C04201/foundation/DMS`: screens seen by the window manager;
- `USER_AUTH_HDI`, `PIN_AUTH_HDI`, `USER_AUTH_SA`: lock screen credentials.

## Known issues

| Symptom | Status |
|---|---|
| ADSP restarting every ~40 s (`sensor_process ... USER-PD DOG`) | needs patch 34 (hexagonrpcd); if it goes on, look for `hexagonrpcd` and `adsprpc` in kmsg |
| `wlan: /dev/wlan ON -> -22`, no `wlan0` | look for `sweet: board id` / `BDF download` (icnss) in kmsg: the kernel must send bdwlan to the firmware |
| Bluetooth does not turn on | `sweet-bt:` lines in kmsg: chip version, crbtfw/crnv files, HCI reset answer |
| Spontaneous reboots | run `get_logs` right after: `pstore/` and the `kmsg` of the previous boot show the cause; look for `crashed`, `critical`, `subsys-restart`, `Kernel panic`. Patch 15 removes the reboots for crashing services, kernel patch 0013 those for a slow ADSP/modem || "Power off" restarts the phone | with a cable plugged in this is off-mode charging: since this release the phone stays off and charges. Without a cable, look for `hmos: power off requested` in pstore |
| PIN asked at boot although none was set | fixed in iter42 (user_auth HDI built for the HarmonyOS 5.x interface). If it comes back: `USER_AUTH_HDI` in hilog, "VerifyAuthToken bad param" means the interface does not match |
| Sensors missing in apps | `sweet-sensors:` lines in kmsg: if it stays at "waiting for the SSC", the ADSP sensors domain did not start (see `hexagonrpcd`) |
| `tqftpserv: read/write <file> -> error` in kmsg | file the modem asks the TFTP server for: if `wlanmdsp.mbn` or a readwrite folder is missing, WiFi does not start |
| hdc does not see the phone | enable developer options and USB debugging; look for `a600000.dwc3` / `configfs` in kmsg |
| Setup stuck on the WiFi screen | enable "Remove the setup wizard" (`--no-setup-wizard`) |
| Camera | in progress (`src/camera`): Qualcomm CamX in an Android namespace + OpenHarmony camera host |
| Fingerprint | not ported: the sensor is handled by a trusted app in the Qualcomm TEE (QSEE) |
| Proximity | sweet's sensor is ultrasonic (handled by the audio DSP), not included |
| HEVC video above 1920 px | limit in the media service, patch 40 is not enough |

## Black screen (star2lte)

The composer is the DECON/fb0 VDI (`src/display/composer`), not DRM. It appends every step to
`/data/vendor/display/decon-hdi.log` (and `/dev/kmsg`):

```
library loaded, pid ...
CreateComposerVdiV1_1 (pid ...)
opened /dev/graphics/fb0 as fd ... (kept open for the lifetime of the composer)
FBIOGET_VSCREENINFO -> 0, 1440x2960
panel 1440x2960, backlight /sys/class/backlight/panel/ max 44800
FBIOBLANK(0) -> 0
WIN_CONFIG first black frame ok
SetDisplayPowerStatus 0 (was 0)      <- twice, from powermgr
```

- Missing/failed step: that ioctl or node is the bug (check permissions and the kernel log).
- No `Commit`/`Present buf` lines after the power status: render_service never handed a frame to the composer.
  In the 2026-11-16 boot hilog showed `utils_base: path (/system/lib64/libEGL_impl.so) to realpath error`
  followed by `eglGetDisplay`/`eglCreateContext` and the `InitializeEglContext`/`SetUpGpuContext` warnings:
  OpenHarmony's `libEGL.so` wrapper loads the vendor driver from `/vendor/lib64/chipsetsdk/`, and the OTA's
  `libEGL_impl.so`/`libGLESv*_impl.so` are symlinks to `libhvgr_v200.so` (Huawei Maleoon). There is no GLES
  driver for Mali-G72 in the OTA, so render_service cannot draw at all; patch 11-gpu-mesa (Mesa with the OHOS
  EGL platform, `softpipe` first) is what has to come next.
- Panel dark while composer_host runs: the Huawei `displayengine_device` driver listed in
  `/etc/hdfconfig/hdf_default.hcb` opens fb0, issues its Kirin ioctls (`0x20`, `0x21`, `0x83a84413`, all
  "invalid cmd" in kmsg) and closes; `decon_release()` calls `decon_disable()` on *every* close. Remove that
  driver from the host config (or stop the panel-off in the kernel) before the panel can stay lit.
- **Black after the boot animation** (sweet): if SceneBoard restarts in a loop with
  `GetDefaultDisplaySync: Get display info failed`, the window manager did not get the screen. Check that the
  composer created a single display with id 0 (our `DeconDisplay::IsConnected()` must return true).

## Freeze loop, then a reset 20-30 s after the lock screen (star2lte)

With the images installing again, the phone reaches the lock screen (about 15 min of boot) and then resets.
`/data/log/faultlog/faultlogger/appfreeze-com.ohos.sceneboard-*` shows why:

```
Reason:THREAD_BLOCK_6S
#04 libeventhandler.z.so EventHandler::SendSyncEvent(...)
#05 libscene_session_manager.z.so SceneSessionManager::RequestSceneSession(...)
#06 libscenesessionmanager_napi.z.so JsSceneSessionManager::OnRequestSceneSession(...)   <- requestSystemSceneSession()
```

SceneBoard's UI thread waits in a *synchronous* event (`aboutToAppear` of `SCBSystemScene.ts` ->
`requestSystemSceneSession`). The event runs on the `OS_SceneSession` thread of the same process, and that thread
is inside a blocking binder call to RenderService:

```
Tid:8354, Name:OS_SceneSession
#0.. libipc_core.z.so IPCObjectProxy::SendRequest / WaitForCompletion / ioctl
RSRenderServiceConnectionProxy::CreateNodeAndSurface
```

So RenderService does not answer `CreateNodeAndSurface`; the UI thread times out after 6 s, SceneBoard is reported
frozen, the request is retried, and the report repeats every ~33 s. After about ten reports the phone resets with no
panic in `pstore` and no reboot request in hilog, i.e. not a kernel panic and not a userspace `reboot()`.

First thing to check is therefore RenderService itself: `boot-N/snapN-stacks.txt` has the kernel stack and `wchan`
of its threads, and Composer logs every 30th frame as `frame N: X ms since last, Y ms in present` - X is what
RenderService takes, Y what the DECON composer takes. On 2026-11-18 composition ran at 0.75 fps with `softpipe`
(no GPU driver); `RENDER_DIV` in `src/display/composer/src/decon_display.cpp` is the trade between sharpness and
frame rate, and RenderService stops answering once a frame takes seconds.

Note for log collection: preinit used to delete all logs at every boot start, so a crash-reboot (which starts a few
seconds after the crash) always destroyed the evidence. It now rotates `kmsg-0...19`, keeps `pstore-N`/`boot-N` and
only reclaims space below 64 MB free.

## Going back to Android

Flash a sweet ROM from recovery. The ROM recreates Android's logical partitions in `super`.
