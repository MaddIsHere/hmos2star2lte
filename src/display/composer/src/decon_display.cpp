#include "decon_display.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <securec.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "display_log.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
namespace {
/* Samsung DECON window-config ABI (exynos9810 dpu decon.h without CONFIG_SUPPORT_DSU) */
struct DeconRect { int x, y; uint32_t w, h; };
struct DeconFrame { int x, y; uint32_t w, h, fw, fh; };
struct DeconDpp { uint64_t addr[3]; int rot, eqMode, compSrc, hdrStd; uint32_t minLum, maxLum; };
struct DeconWin {
    int state; /* 0 disabled, 1 color, 2 buffer, 3 update, 4 cursor */
    union {
        uint32_t color;
        struct {
            int fdIdma[3];
            int acqFence, relFence, planeAlpha, blending, idmaType, format;
            DeconDpp dpp;
            DeconRect block, transparent, opaque;
            DeconFrame src;
        };
    };
    DeconFrame dst;
    bool protection, compression;
};
struct DeconData {
    int retireFence, fdOdma;
    DeconWin config[7];
    struct { int remainedFrames; uint32_t reserved[7]; } extra;
};
constexpr unsigned long S3CFB_WIN_CONFIG = _IOW('F', 209, DeconData);
constexpr unsigned long FBIOBLANK_ = 0x4611;
/* Software GL is slow (a frame takes seconds at 1440x2960): render_service draws at panel/RENDER_DIV and the
 * CPU enlarges it into a panel sized buffer (the DECON window of this panel does not scale). DIV=8 trades sharpness
 * for frame rate: the SceneBoard freeze reports of the first bring-up showed RenderService unable to answer
 * CreateNodeAndSurface while it was busy in softpipe. */
constexpr uint32_t RENDER_DIV = 8;
constexpr int WIN_COLOR = 1;
constexpr int WIN_BUFFER = 2;

enum {
    DPU_ARGB_8888 = 0, DPU_ABGR_8888, DPU_RGBA_8888, DPU_BGRA_8888,
    DPU_XRGB_8888, DPU_XBGR_8888, DPU_RGBX_8888, DPU_BGRX_8888,
    DPU_RGBA_5551, DPU_BGRA_5551, DPU_ABGR_4444, DPU_RGBA_4444, DPU_BGRA_4444, DPU_RGB_565, DPU_BGR_565,
};

/* Measured on the S9+ panel: the DPU format names list the bytes in MEMORY order (like the HDI names), so
 * HDI RGBA_8888 (R,G,B,A in memory) is DPU_RGBA_8888. ABGR made the panel read R=byte3, G=byte2, B=byte1. */
int ToDpuFormat(int32_t fmt)
{
    switch (fmt) {
        case PIXEL_FMT_RGBA_8888: return DPU_RGBA_8888;
        case PIXEL_FMT_BGRA_8888: return DPU_BGRA_8888;
        case PIXEL_FMT_RGBX_8888: return DPU_RGBX_8888;
        case PIXEL_FMT_BGRX_8888: return DPU_BGRX_8888;
        case PIXEL_FMT_RGB_565: return DPU_BGR_565;
        default: return -1;
    }
}

uint32_t BytesPerPixel(int32_t fmt)
{
    return fmt == PIXEL_FMT_RGB_565 ? 2 : 4;
}

void Klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Klog(const char *fmt, ...)
{
    char buf[220];
    int n = snprintf(buf, sizeof(buf), "<14>decon-hdi: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > static_cast<int>(sizeof(buf)) - 2) {
        n = sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    /* hilog is the reliable log channel (/data/log/hilog survives into TWRP); kmsg only when the logger is alive */
    DISPLAY_LOGE("DECON %{public}s", buf + 4);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, buf, n);
        close(fd);
    }
}

const char *const BL_DIRS[] = { "/sys/class/backlight/panel/", "/sys/class/backlight/backlight/" };

int OpenBl(const char *file, int flags, const char **dirOut = nullptr)
{
    char path[128];
    for (const char *d : BL_DIRS) {
        snprintf(path, sizeof(path), "%s%s", d, file);
        int fd = open(path, flags | O_CLOEXEC);
        if (fd >= 0) {
            if (dirOut) {
                *dirOut = d;
            }
            return fd;
        }
    }
    return -1;
}

uint64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}
constexpr uint64_t VSYNC_PERIOD_NS = 16666667;
/* Software GL renders at panel/RENDER_DIV; the DECON window of this panel does not scale (the scaled window showed
 * black), so enlarge into a panel sized ION buffer on the CPU (nearest neighbour). */
struct IonAllocReq { size_t len; size_t align; unsigned int heapMask; unsigned int flags; int handle; };
struct IonFdReq { int handle; int fd; };
struct IonHandleReq { int handle; };

int ScanoutAlloc(size_t len)
{
    static int ion = -1;
    if (ion < 0) {
        ion = open("/dev/ion", O_RDWR | O_CLOEXEC);
    }
    if (ion < 0) {
        return -1;
    }
    IonAllocReq a = { len, 4096, 1u, 0, 0 };
    if (ioctl(ion, _IOWR('I', 0, IonAllocReq), &a) != 0) {
        return -1;
    }
    IonFdReq sh = { a.handle, -1 };
    int ret = ioctl(ion, _IOWR('I', 4, IonFdReq), &sh);
    IonHandleReq fh = { a.handle };
    ioctl(ion, _IOWR('I', 1, IonHandleReq), &fh);
    return (ret == 0) ? sh.fd : -1;
}

void DmaSync(int fd, uint64_t flags)
{
    ioctl(fd, _IOW('b', 0, uint64_t), &flags);
}

/* returns the dma-buf fd of the enlarged frame, -1 on failure */
int UpscaleToScanout(int srcFd, uint32_t w, uint32_t h, uint32_t srcStride, uint32_t div, uint32_t outW,
    uint32_t outH)
{
    static int fds[2] = { -1, -1 };
    static uint8_t *maps[2] = { nullptr, nullptr };
    static unsigned cur = 0;
    const size_t outLen = static_cast<size_t>(outW) * outH * 4;
    cur ^= 1;
    if (fds[cur] < 0) {
        int fd = ScanoutAlloc(outLen);
        if (fd < 0) {
            return -1;
        }
        void *m = mmap(nullptr, outLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            return -1;
        }
        fds[cur] = fd;
        maps[cur] = static_cast<uint8_t *>(m);
    }
    const size_t srcLen = static_cast<size_t>(srcStride) * h;
    void *sm = mmap(nullptr, srcLen, PROT_READ, MAP_SHARED, srcFd, 0);
    if (sm == MAP_FAILED) {
        return -1;
    }
    DmaSync(srcFd, 1);
    DmaSync(fds[cur], 2);
    const uint8_t *src = static_cast<const uint8_t *>(sm);
    uint8_t *dst = maps[cur];
    for (uint32_t y = 0; y < outH; y++) {
        uint32_t sy = y / div;
        if (sy >= h) {
            sy = h - 1;
        }
        uint32_t *o = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * outW * 4);
        if (y % div != 0) {
            memcpy(o, dst + static_cast<size_t>(y - 1) * outW * 4, static_cast<size_t>(outW) * 4);
            continue;
        }
        const uint32_t *in = reinterpret_cast<const uint32_t *>(src + static_cast<size_t>(sy) * srcStride);
        for (uint32_t x = 0; x < outW; x++) {
            uint32_t sx = x / div;
            o[x] = in[sx < w ? sx : w - 1];
        }
    }
    DmaSync(fds[cur], 6);
    DmaSync(srcFd, 5);
    munmap(sm, srcLen);
    return fds[cur];
}

} // namespace

void DeconTrace(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Klog("%s", msg);
}

DeconFb &DeconFb::Get()
{
    static DeconFb inst;
    return inst;
}

int32_t DeconFb::Init()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mFd >= 0) {
        return DISPLAY_SUCCESS;
    }
    const char *nodes[] = { "/dev/graphics/fb0", "/dev/fb0" };
    for (const char *n : nodes) {
        mFd = open(n, O_RDWR | O_CLOEXEC);
        if (mFd >= 0) {
            Klog("opened %s as fd %d (kept open for the lifetime of the composer)", n, mFd);
            break;
        }
    }
    DISPLAY_CHK_RETURN((mFd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("can not open fb0: %{public}d", errno));
    /* the geometry comes from the kernel: FBIOGET_VSCREENINFO, xres/yres are the first two u32 */
    uint32_t var[40] = {};
    if (ioctl(mFd, 0x4600 /* FBIOGET_VSCREENINFO */, var) == 0 && var[0] > 0 && var[1] > 0) {
        mWidth = var[0];
        mHeight = var[1];
    }
    int fd = OpenBl("max_brightness", O_RDONLY);
    if (fd >= 0) {
        char b[16] = {};
        (void)read(fd, b, sizeof(b) - 1);
        close(fd);
        mMaxBl = static_cast<uint32_t>(atoi(b));
    }
    Klog("panel %ux%u, backlight max %u", mWidth, mHeight, mMaxBl);
    /* DECON stays in INIT until the first window config: light it with a black frame */
    DeconData d;
    memset(&d, 0, sizeof(d));
    d.retireFence = d.fdOdma = -1;
    d.config[0].state = WIN_COLOR;
    d.config[0].color = 0;
    d.config[0].dst = { 0, 0, mWidth, mHeight, mWidth, mHeight };
    ioctl(mFd, FBIOBLANK_, 0);
    int ret = Commit(&d, "first black frame");
    return ret == 0 ? DISPLAY_SUCCESS : DISPLAY_FAILURE;
}

int DeconFb::Commit(void *data, const char *what)
{
    DeconData *d = static_cast<DeconData *>(data);
    int ret = ioctl(mFd, S3CFB_WIN_CONFIG, d);
    if (ret != 0) {
        static int fails;
        if (fails++ < 20) {
            Klog("WIN_CONFIG %s failed: errno %d", what, errno);
        }
    }
    return ret;
}

int32_t DeconFb::SetPower(bool on)
{
    std::lock_guard<std::mutex> lock(mMutex);
    DISPLAY_CHK_RETURN((mFd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("fb0 not open"));
    int ret = ioctl(mFd, FBIOBLANK_, on ? 0 : 4);
    Klog("power %s -> %d (errno %d)", on ? "on" : "off", ret, ret ? errno : 0);
    if (ret == 0) {
        mOff.store(!on);
    }
    return ret == 0 ? DISPLAY_SUCCESS : DISPLAY_FAILURE;
}

static 
/* Debug: save a 1/4 scale PPM of a presented frame so the picture can be inspected from TWRP. */
void DumpFrame(int fd, uint32_t w, uint32_t h, uint32_t strideBytes, int32_t fmt, uint64_t n)
{
    size_t len = static_cast<size_t>(strideBytes) * h;
    void *m = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        Klog("frame dump: mmap failed errno %d", errno);
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/data/local/tmp/decon-frame-%llu.ppm", static_cast<unsigned long long>(n));
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        Klog("frame dump: cannot open %s errno %d (statistics only)", path, errno);
    }
    uint32_t ow = w / 4, oh = h / 4;
    if (f != nullptr) {
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
    }
    const uint8_t *src = static_cast<const uint8_t *>(m);
    uint64_t sum[3] = {0, 0, 0};
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            const uint8_t *px = src + static_cast<size_t>(y * 4) * strideBytes + static_cast<size_t>(x * 4) * 4;
            uint8_t rgb[3];
            if (fmt == DPU_BGRA_8888 || fmt == DPU_BGRX_8888) {
                rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0];
            } else {
                rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2];
            }
            sum[0] += rgb[0]; sum[1] += rgb[1]; sum[2] += rgb[2];
            if (f != nullptr) {
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    if (f != nullptr) {
        fclose(f);
    }
    const uint8_t *c = src + static_cast<size_t>(h / 2) * strideBytes + static_cast<size_t>(w / 2) * 4;
    Klog("frame centre pixel bytes %02x %02x %02x %02x, top-left %02x %02x %02x %02x", c[0], c[1], c[2], c[3], src[0],
        src[1], src[2], src[3]);
    munmap(m, len);
    uint64_t cnt = static_cast<uint64_t>(ow) * oh;
    Klog("frame dump %s fmt %d stride %u: mean RGB %llu %llu %llu", path, fmt, strideBytes,
        static_cast<unsigned long long>(sum[0] / cnt), static_cast<unsigned long long>(sum[1] / cnt),
        static_cast<unsigned long long>(sum[2] / cnt));
}

int32_t DeconFb::Present(HdiLayerBuffer &buf, int32_t *releaseFence)
{
    *releaseFence = -1;
    std::lock_guard<std::mutex> lock(mMutex);
    uint64_t t0 = NowNs();
    if (mFd < 0 || mOff.load()) {
        return DISPLAY_SUCCESS;
    }
    int fmt = ToDpuFormat(buf.GetFormat());
    DISPLAY_CHK_RETURN((fmt < 0), DISPLAY_FAILURE, DISPLAY_LOGE("unsupported format %{public}d", buf.GetFormat()));
    uint32_t bpp = BytesPerPixel(buf.GetFormat());
    uint32_t w = static_cast<uint32_t>(buf.GetWight());
    uint32_t h = static_cast<uint32_t>(buf.GetHeight());
    if (w > mWidth) {
        w = mWidth;
    }
    if (h > mHeight) {
        h = mHeight;
    }
    DeconData d;
    memset(&d, 0, sizeof(d));
    d.retireFence = d.fdOdma = -1;
    DeconWin &c = d.config[0];
    c.state = WIN_BUFFER;
    int scanFd = buf.GetFb();
    uint32_t sw = w, sh = h, sfw = static_cast<uint32_t>(buf.GetStride()) / bpp, sfh = static_cast<uint32_t>(buf.GetHeight());
    if (RENDER_DIV > 1 && bpp == 4 && w * RENDER_DIV >= mWidth && h * RENDER_DIV >= mHeight && w < mWidth) {
        /* enlarge by the ratio the client buffer actually has (RenderService may keep an older mode size) */
        uint32_t div = (w > 0) ? (mWidth + w - 1) / w : RENDER_DIV;
        int up = UpscaleToScanout(buf.GetFb(), w, h, static_cast<uint32_t>(buf.GetStride()), div, mWidth,
            mHeight);
        if (up >= 0) {
            scanFd = up;
            sw = sfw = mWidth;
            sh = sfh = mHeight;
        }
    }
    c.fdIdma[0] = scanFd;
    c.fdIdma[1] = c.fdIdma[2] = -1;
    c.acqFence = c.relFence = -1;
    c.planeAlpha = 255;
    c.blending = 0;
    c.idmaType = 0;
    c.format = fmt;
    c.src = { 0, 0, sw, sh, sfw, sfh };
    c.dst = { 0, 0, mWidth, mHeight, mWidth, mHeight };
    int ret = Commit(&d, "client target");
    if (ret != 0) {
        return DISPLAY_FAILURE;
    }
    ++mFrames;
    uint64_t t1 = NowNs();
    /* where the time goes: interval between client frames (RenderService) vs. time spent presenting (composer) */
    {
        static uint64_t lastNs;
        if (mFrames % 30 == 0) {
            Klog("frame %llu: %llu ms since last, %llu ms in present", static_cast<unsigned long long>(mFrames),
                static_cast<unsigned long long>((t1 - lastNs) / 1000000),
                static_cast<unsigned long long>((t1 - t0) / 1000000));
            lastNs = t1;
        }
    }
    if (mFrames == 1 || mFrames == 3 || mFrames == 10 || mFrames == 40 || mFrames == 150 || mFrames == 600) {
        DumpFrame(buf.GetFb(), w, h, static_cast<uint32_t>(buf.GetStride()), buf.GetFormat(), mFrames);
    }
    if (mFrames == 1 || mFrames % 600 == 0) {
        Klog("frame %llu presented (retire %d rel %d)", static_cast<unsigned long long>(mFrames), d.retireFence,
            c.relFence);
    }
    /* rel_fence: the buffer is no longer scanned out; retire_fence: the frame reached the panel */
    if (c.relFence >= 0) {
        *releaseFence = c.relFence;
        if (d.retireFence >= 0) {
            close(d.retireFence);
        }
    } else {
        *releaseFence = d.retireFence;
    }
    return DISPLAY_SUCCESS;
}

int32_t DeconFb::PresentColor(uint32_t xrgb)
{
    std::lock_guard<std::mutex> lock(mMutex);
    DeconData d;
    memset(&d, 0, sizeof(d));
    d.retireFence = d.fdOdma = -1;
    d.config[0].state = WIN_COLOR;
    d.config[0].color = xrgb;
    d.config[0].dst = { 0, 0, mWidth, mHeight, mWidth, mHeight };
    int ret = Commit(&d, "color");
    if (ret == 0 && d.retireFence >= 0) {
        close(d.retireFence);
    }
    return ret == 0 ? DISPLAY_SUCCESS : DISPLAY_FAILURE;
}

int32_t DeconFb::SetBacklight(uint32_t level)
{
    int fd = OpenBl("brightness", O_WRONLY);
    DISPLAY_CHK_RETURN((fd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("no backlight node"));
    if (level > 255) {
        level = 255;
    }
    uint32_t v = mMaxBl ? static_cast<uint32_t>((static_cast<uint64_t>(level) * mMaxBl) / 255) : level;
    char b[16];
    int n = snprintf(b, sizeof(b), "%u", v);
    ssize_t r = write(fd, b, n);
    close(fd);
    static uint32_t last = 1;
    if ((level == 0) != (last == 0)) {
        Klog("backlight %u -> panel %u", level, v);
    }
    last = level;
    return r == n ? DISPLAY_SUCCESS : DISPLAY_FAILURE;
}

int32_t DeconFb::GetBacklight(uint32_t *level)
{
    int fd = OpenBl("brightness", O_RDONLY);
    DISPLAY_CHK_RETURN((fd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("no backlight node"));
    char b[16] = {};
    (void)read(fd, b, sizeof(b) - 1);
    close(fd);
    uint32_t v = static_cast<uint32_t>(atoi(b));
    *level = mMaxBl ? static_cast<uint32_t>((static_cast<uint64_t>(v) * 255) / mMaxBl) : v;
    return DISPLAY_SUCCESS;
}

int32_t HdiClientComposition::SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer)
{
    (void)clientLayer;
    for (auto &layer : layers) {
        layer->SetDeviceSelect(COMPOSITION_CLIENT);
    }
    return DISPLAY_SUCCESS;
}

int32_t HdiDeconComposition::SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer)
{
    (void)layers;
    mClient = &clientLayer;
    mCompLayers.clear();
    mCompLayers.push_back(&clientLayer);
    return DISPLAY_SUCCESS;
}

int32_t HdiDeconComposition::Apply(bool modeSet)
{
    (void)modeSet;
    std::lock_guard<std::mutex> lock(mApplyMutex);
    DISPLAY_CHK_RETURN((mClient == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("no client layer"));
    HdiLayerBuffer *buf = mClient->GetCurrentBuffer();
    if (buf == nullptr) {
        mClient->SetReleaseFence(-1);
        return DISPLAY_SUCCESS;
    }
    mClient->WaitAcquireFence();
    int32_t fence = -1;
    int32_t ret = DeconFb::Get().Present(*buf, &fence);
    mClient->SetReleaseFence(ret == DISPLAY_SUCCESS ? fence : -1);
    return ret;
}

DeconDisplay::~DeconDisplay()
{
    {
        std::lock_guard<std::mutex> lg(mVsyncMutex);
        mRunning = false;
    }
    mVsyncCv.notify_all();
    if (mThread && mThread->joinable()) {
        mThread->join();
    }
}

int32_t DeconDisplay::Init()
{
    int32_t ret = DeconFb::Get().Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("fb0 init failed"));
    ret = HdiDisplay::Init();
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("display init failed"));
    mComposer = std::make_unique<HdiComposer>(std::make_unique<HdiClientComposition>(),
        std::make_unique<HdiDeconComposition>());
    mRunning = true;
    mThread = std::make_unique<std::thread>([this]() { VsyncLoop(); });
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::GetDisplayCapability(DisplayCapability *info)
{
    DISPLAY_CHK_RETURN((info == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("info is null"));
    static const char NAME[] = "DECON";
    info->phyWidth = 74;
    info->phyHeight = 152;
    info->type = DISP_INTF_MIPI;
    memcpy_s(const_cast<char *>(info->name.c_str()), info->name.size(), NAME, sizeof(NAME) - 1);
    if (info->name.size() >= sizeof(NAME)) {
        const_cast<char *>(info->name.c_str())[sizeof(NAME) - 1] = 0;
    }
    info->supportLayers = 4;
    info->virtualDispCount = 0;
    info->supportWriteBack = false;
    info->propertyCount = 0;
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::GetDisplaySupportedModes(uint32_t *num, DisplayModeInfo *modes)
{
    DISPLAY_CHK_RETURN((num == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("num is null"));
    *num = 1;
    if (modes != nullptr) {
        modes[0].width = static_cast<int32_t>(DeconFb::Get().Width() / RENDER_DIV);
        modes[0].height = static_cast<int32_t>(DeconFb::Get().Height() / RENDER_DIV);
        modes[0].freshRate = 60;
        modes[0].id = 0;
    }
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::GetDisplayMode(uint32_t *modeId)
{
    DISPLAY_CHK_RETURN((modeId == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("modeId is null"));
    *modeId = 0;
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::SetDisplayMode(uint32_t modeId)
{
    return modeId == 0 ? DISPLAY_SUCCESS : DISPLAY_PARAM_ERR;
}

int32_t DeconDisplay::GetDisplayPowerStatus(DispPowerStatus *status)
{
    DISPLAY_CHK_RETURN((status == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("status is null"));
    *status = mPowerStatus;
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::SetDisplayPowerStatus(DispPowerStatus status)
{
    uint32_t st = static_cast<uint32_t>(status);
    Klog("SetDisplayPowerStatus %u (was %u)", st, static_cast<uint32_t>(mPowerStatus));
    switch (status) {
        case POWER_STATUS_ON:
            if (DeconFb::Get().IsOff()) {
                DeconFb::Get().SetPower(true);
            }
            break;
        case POWER_STATUS_OFF:
        case POWER_STATUS_STANDBY:
        case POWER_STATUS_SUSPEND:
            DeconFb::Get().SetBacklight(0);
            DeconFb::Get().SetPower(false);
            break;
        default:
            /* 4 OFF_FAKE, 5 PRE_ON, 6 PRE_OFF (composer 1.2): hints, keep the panel as it is */
            return DISPLAY_SUCCESS;
    }
    mPowerStatus = status;
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::GetDisplayBacklight(uint32_t *value)
{
    DISPLAY_CHK_RETURN((value == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("value is null"));
    return DeconFb::Get().GetBacklight(value);
}

int32_t DeconDisplay::SetDisplayBacklight(uint32_t value)
{
    if (DeconFb::Get().IsOff()) {
        return DISPLAY_SUCCESS; /* powermgr may animate the level while the panel is blanked */
    }
    return DeconFb::Get().SetBacklight(value);
}

int32_t DeconDisplay::RegDisplayVBlankCallback(VBlankCallback cb, void *data)
{
    std::lock_guard<std::mutex> lg(mVsyncMutex);
    mVsyncCb = std::make_shared<VsyncCallBack>(cb, data, 0);
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::SetDisplayVsyncEnabled(bool enabled)
{
    {
        std::lock_guard<std::mutex> lg(mVsyncMutex);
        mVsyncEnabled = enabled;
    }
    mVsyncCv.notify_all();
    return DISPLAY_SUCCESS;
}

int32_t DeconDisplay::WaitForVBlank(uint64_t *ns)
{
    DISPLAY_CHK_RETURN((ns == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("ns is null"));
    uint64_t now = NowNs();
    uint64_t next = (now / VSYNC_PERIOD_NS + 1) * VSYNC_PERIOD_NS;
    usleep(static_cast<useconds_t>((next - now) / 1000));
    *ns = next;
    return DISPLAY_SUCCESS;
}

/* software vsync on a 60 Hz grid (the DECON vsync uevent is not used yet) */
void DeconDisplay::VsyncLoop()
{
    unsigned int seq = 0;
    for (;;) {
        std::shared_ptr<VsyncCallBack> cb;
        {
            std::unique_lock<std::mutex> ul(mVsyncMutex);
            mVsyncCv.wait(ul, [this]() { return mVsyncEnabled || !mRunning; });
            if (!mRunning) {
                return;
            }
            cb = mVsyncCb;
        }
        if (DeconFb::Get().IsOff()) {
            usleep(16000);
            continue;
        }
        uint64_t ns = 0;
        WaitForVBlank(&ns);
        if (cb != nullptr) {
            cb->Vsync(seq++, ns);
        }
    }
}

std::shared_ptr<HdiDeviceInterface> DeconDevice::Create()
{
    return std::make_shared<DeconDevice>();
}

int32_t DeconDevice::Init()
{
    return DeconFb::Get().Init();
}

std::unordered_map<uint32_t, std::shared_ptr<HdiDisplay>> DeconDevice::DiscoveryDisplay()
{
    mDisplays.clear();
    auto display = std::make_shared<DeconDisplay>();
    if (display->Init() != DISPLAY_SUCCESS) {
        DISPLAY_LOGE("DECON display init failed");
        return mDisplays;
    }
    mDisplays.emplace(display->GetId(), display);
    return mDisplays;
}
} // namespace DISPLAY
} // namespace HDI
} // namespace OHOS
