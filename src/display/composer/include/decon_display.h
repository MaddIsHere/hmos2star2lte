/*
 * Samsung DECON (Exynos 9810) display backend for the HarmonyOS display composer VDI.
 *
 * The S9+ kernel has no DRM/KMS for DECON. The panel is driven through the vendor fbdev node
 * (/dev/graphics/fb0): FBIOBLANK for power and S3CFB_WIN_CONFIG (_IOW('F', 209, ...)) to scan out a dma-buf.
 * Every layer is composed by the client (render_service); the composer only presents the client target.
 *
 * Important: the DECON driver powers the panel down on EVERY close of fb0 (decon_release -> decon_disable), so
 * the fd is opened once and never closed.
 */
#ifndef DECON_DISPLAY_H
#define DECON_DISPLAY_H
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "hdi_composer.h"
#include "hdi_device_interface.h"
#include "hdi_display.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
/* Unbuffered trace to /data/vendor/display/decon-hdi.log and /dev/kmsg: the panel is dark while
 * composer_host starts, so the file is the only place to see how far the composer got. */
void DeconTrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

class DeconFb {
public:
    static DeconFb &Get();
    int32_t Init();
    uint32_t Width() const { return mWidth; }
    uint32_t Height() const { return mHeight; }
    bool IsOff() const { return mOff.load(); }
    int32_t SetPower(bool on);
    /* scan out the dma-buf of the buffer; *releaseFence gets a fence that signals when the buffer is free */
    int32_t Present(HdiLayerBuffer &buf, int32_t *releaseFence);
    int32_t PresentColor(uint32_t xrgb);
    int32_t SetBacklight(uint32_t level255);
    int32_t GetBacklight(uint32_t *level255);

private:
    DeconFb() = default;
    int Commit(void *data, const char *what);
    int mFd = -1;
    uint32_t mWidth = 1440;
    uint32_t mHeight = 2960;
    std::atomic<bool> mOff{false};
    std::mutex mMutex;
    uint32_t mMaxBl = 0;
    uint32_t mSavedBl = 0;
    uint64_t mFrames = 0;
};

/* the layers go to the client target, nothing is composed here */
class HdiClientComposition : public HdiComposition {
public:
    int32_t SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer) override;
};

class HdiDeconComposition : public HdiComposition {
public:
    int32_t SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer) override;
    int32_t Apply(bool modeSet) override;

private:
    HdiLayer *mClient = nullptr;
    std::mutex mApplyMutex;
};

class DeconDisplay : public HdiDisplay {
public:
    DeconDisplay() = default;
    ~DeconDisplay() override;
    int32_t Init() override;
    int32_t GetDisplayCapability(DisplayCapability *info) override;
    int32_t GetDisplaySupportedModes(uint32_t *num, DisplayModeInfo *modes) override;
    int32_t GetDisplayMode(uint32_t *modeId) override;
    int32_t SetDisplayMode(uint32_t modeId) override;
    int32_t GetDisplayPowerStatus(DispPowerStatus *status) override;
    int32_t SetDisplayPowerStatus(DispPowerStatus status) override;
    int32_t GetDisplayBacklight(uint32_t *value) override;
    int32_t SetDisplayBacklight(uint32_t value) override;
    bool IsConnected() override { return true; }
    int32_t RegDisplayVBlankCallback(VBlankCallback cb, void *data) override;
    int32_t SetDisplayVsyncEnabled(bool enabled) override;
    int32_t WaitForVBlank(uint64_t *ns) override;

private:
    void VsyncLoop();
    DispPowerStatus mPowerStatus = POWER_STATUS_ON;
    std::shared_ptr<VsyncCallBack> mVsyncCb;
    std::mutex mVsyncMutex;
    std::condition_variable mVsyncCv;
    bool mVsyncEnabled = false;
    bool mRunning = false;
    std::unique_ptr<std::thread> mThread;
};

class DeconDevice : public HdiDeviceInterface {
public:
    static std::shared_ptr<HdiDeviceInterface> Create();
    std::unordered_map<uint32_t, std::shared_ptr<HdiDisplay>> DiscoveryDisplay() override;
    bool HandleHotplug(uint32_t dispId, bool plugIn) override { return false; }
    int32_t Init() override;
    void DeInit() override {}

private:
    std::unordered_map<uint32_t, std::shared_ptr<HdiDisplay>> mDisplays;
};
} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
#endif
