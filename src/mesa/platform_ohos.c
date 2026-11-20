/*
 * Mesa 3-D graphics library
 *
 * OpenHarmony / HarmonyOS EGL platform for Mesa 26.
 *
 * Ported for the HarmonyOS bring-up on the Redmi Note 10 Pro (sweet) from
 * the OpenHarmony third_party_mesa3d platform_ohos.c (Copyright (c) 2021
 * Huawei Device Co., Ltd.), which in turn is based on platform_android:
 *
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xf86drm.h>
#include <drm-uapi/drm_fourcc.h>

#include "util/compiler.h"
#include "util/libsync.h"
#include "util/log.h"
#include "util/os_file.h"
#include "util/u_debug.h"

#include "main/glconfig.h"
#include "platform_ohos.h"
#include "egl_dri2.h"
#include "eglglobals.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include "dri_util.h"

/* ------------------------------------------------------------------------ */
/* native buffer -> dma-buf description                                     */
/* ------------------------------------------------------------------------ */

static int
ohos_get_fourcc(int native)
{
   switch (native) {
   case PIXEL_FMT_RGB_565:
      return DRM_FORMAT_RGB565;
   case PIXEL_FMT_BGRA_8888:
      return DRM_FORMAT_ARGB8888;
   case PIXEL_FMT_BGRX_8888:
      return DRM_FORMAT_XRGB8888;
   case PIXEL_FMT_RGBA_8888:
      return DRM_FORMAT_ABGR8888;
   case PIXEL_FMT_RGBX_8888:
      return DRM_FORMAT_XBGR8888;
   case PIXEL_FMT_YCBCR_420_SP:
   case PIXEL_FMT_YCRCB_420_SP:
   case PIXEL_FMT_YCBCR_420_P:
   case PIXEL_FMT_YCRCB_420_P:
      /* handled by ohos_yuv_to_rgba_image() */
      return DRM_FORMAT_ABGR8888;
   default:
      _eglLog(_EGL_WARNING, "ohos: unsupported native buffer format 0x%x",
              native);
      return -1;
   }
}

/* softpipe cannot sample planar YUV, so convert to RGBA on the CPU into a
 * memfd and import that instead (a snapshot taken at import time). */
static inline int
ohos_clamp8(int v)
{
   return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* The converted frames go into memfds.  Making a new 13 MB memfd for every imported frame leaked all of them
 * (render_service reached 3.6 GB and was OOM-killed), so a small ring of memfds per frame size is reused instead:
 * the frame is converted again on every import anyway, and RenderService draws it right after importing it. */
#define OHOS_YUV_RING 6
#define OHOS_YUV_SIZES 8
static struct {
   size_t outsz;
   int fds[OHOS_YUV_RING];
   unsigned next;
} ohos_yuv_pool[OHOS_YUV_SIZES];
static pthread_mutex_t ohos_yuv_lock = PTHREAD_MUTEX_INITIALIZER;

/* returns a pooled memfd of outsz bytes (owned by the pool: do not close), or -1 */
static int
ohos_yuv_pool_fd(size_t outsz)
{
   int fd = -1;
   pthread_mutex_lock(&ohos_yuv_lock);
   for (int i = 0; i < OHOS_YUV_SIZES; i++) {
      if (ohos_yuv_pool[i].outsz == 0) {
         ohos_yuv_pool[i].outsz = outsz;
         for (int k = 0; k < OHOS_YUV_RING; k++)
            ohos_yuv_pool[i].fds[k] = -1;
      }
      if (ohos_yuv_pool[i].outsz != outsz)
         continue;
      int *slot = &ohos_yuv_pool[i].fds[ohos_yuv_pool[i].next++ % OHOS_YUV_RING];
      if (*slot < 0) {
         int m = memfd_create("ohos-yuv", MFD_CLOEXEC);
         if (m >= 0 && ftruncate(m, outsz) != 0) {
            close(m);
            m = -1;
         }
         *slot = m;
      }
      fd = *slot;
      break;
   }
   pthread_mutex_unlock(&ohos_yuv_lock);
   return fd;
}

static long long
ohos_now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* "resident MB / shared MB" of this process, for the log */
static void
ohos_mem_mb(long *rss, long *shr)
{
   long a = 0, b = 0, c = 0;
   FILE *f = fopen("/proc/self/statm", "r");
   *rss = *shr = -1;
   if (!f)
      return;
   if (fscanf(f, "%ld %ld %ld", &a, &b, &c) == 3) {
      *rss = b * 4 / 1024;
      *shr = c * 4 / 1024;
   }
   fclose(f);
}

/* The video-style YUV layers are drawn small (the whole UI is rendered at 1/8 of the panel), and the dma-buf they live
 * in is uncached: reading it a byte at a time cost ~1 s per 1216x2688 frame.  So the frame is converted to an image
 * OHOS_YUV_DIV times smaller (nearest sampling) reading whole source rows with wide copies; the output size is
 * returned in *out_w / *out_h and is what gets imported. */
#define OHOS_YUV_DIV 4
#define OHOS_YUV_MAXSTRIDE 8192

static int
ohos_yuv_to_rgba_memfd(const BufferHandle *h, int *out_stride, bool *pooled,
                       int *out_w, int *out_h)
{
   int w = h->width, ht = h->height, ys = h->stride;
   int fmt = h->format;
   size_t insz = (size_t)ys * ht * 3 / 2;
   if (w <= 0 || ht <= 0 || ys < w || ys > OHOS_YUV_MAXSTRIDE)
      return -1;
   int div = OHOS_YUV_DIV;
   int ow = w / div, oh = ht / div;
   if (ow < 2 || oh < 2) {
      div = 1;
      ow = w;
      oh = ht;
   }
   uint8_t *in = mmap(NULL, insz, PROT_READ, MAP_SHARED, h->fd, 0);
   if (in == MAP_FAILED) {
      _eglLog(_EGL_WARNING, "ohos: yuv mmap failed errno %d", errno);
      return -1;
   }
   int out_pitch = ow * 4;
   size_t outsz = (size_t)out_pitch * oh;
   *pooled = true;
   int mfd = ohos_yuv_pool_fd(outsz);
   if (mfd < 0) {
      *pooled = false;
      mfd = memfd_create("ohos-yuv", MFD_CLOEXEC);
      if (mfd < 0 || ftruncate(mfd, outsz) != 0) {
         if (mfd >= 0)
            close(mfd);
         munmap(in, insz);
         return -1;
      }
   }
   uint8_t *out = mmap(NULL, outsz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
   if (out == MAP_FAILED) {
      if (!*pooled)
         close(mfd);
      munmap(in, insz);
      return -1;
   }
   int semi = (fmt == PIXEL_FMT_YCBCR_420_SP || fmt == PIXEL_FMT_YCRCB_420_SP);
   int cs = semi ? ys : ys / 2;
   const uint8_t *c0 = in + (size_t)ys * ht;
   const uint8_t *c1 = c0 + (size_t)cs * ((ht + 1) / 2);
   uint8_t yrow[OHOS_YUV_MAXSTRIDE], ur[OHOS_YUV_MAXSTRIDE], vr[OHOS_YUV_MAXSTRIDE];
   for (int oy = 0; oy < oh; oy++) {
      int sy = oy * div;
      int cy = sy >> 1;
      uint8_t *o = out + (size_t)oy * out_pitch;
      memcpy(yrow, in + (size_t)sy * ys, w);         /* whole rows: wide loads from the uncached buffer */
      const uint8_t *cu, *cv;
      int cstep;
      if (semi) {
         memcpy(ur, c0 + (size_t)cy * cs, cs);
         if (fmt == PIXEL_FMT_YCBCR_420_SP) { cu = ur; cv = ur + 1; }
         else { cv = ur; cu = ur + 1; }
         cstep = 2;
      } else {
         const uint8_t *p0 = c0 + (size_t)cy * cs, *p1 = c1 + (size_t)cy * cs;
         if (fmt == PIXEL_FMT_YCBCR_420_P) {
            memcpy(ur, p0, cs); memcpy(vr, p1, cs);
         } else {
            memcpy(vr, p0, cs); memcpy(ur, p1, cs);
         }
         cu = ur; cv = vr;
         cstep = 1;
      }
      for (int ox = 0; ox < ow; ox++) {
         int sx = ox * div;
         int d = cu[(sx >> 1) * cstep] - 128, e = cv[(sx >> 1) * cstep] - 128;
         int c = (yrow[sx] - 16) * 298;
         o[0] = ohos_clamp8((c + 409 * e + 128) >> 8);
         o[1] = ohos_clamp8((c - 100 * d - 208 * e + 128) >> 8);
         o[2] = ohos_clamp8((c + 516 * d + 128) >> 8);
         o[3] = 255;
         o += 4;
      }
   }
   munmap(out, outsz);
   munmap(in, insz);
   *out_stride = out_pitch;
   *out_w = ow;
   *out_h = oh;
   return mfd;
}

static struct dri_image *
ohos_create_image_from_native_buffer(_EGLDisplay *disp,
                                     struct ANativeWindowBuffer *buf,
                                     void *priv)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   BufferHandle *handle = GetBufferHandleFromNative(buf);
   unsigned error;

   if (!handle || handle->fd < 0) {
      _eglLog(_EGL_WARNING, "ohos: native buffer without dma-buf");
      return NULL;
   }

   _eglLog(_EGL_WARNING, "ohos: import %dx%d fmt %d stride %d size %d fd %d",
           handle->width, handle->height, handle->format, handle->stride,
           handle->size, handle->fd);
   int fourcc = ohos_get_fourcc(handle->format);
   if (fourcc < 0 || handle->stride <= 0)
      return NULL;

   int yuv_fd = -1, yuv_stride = 0;
   bool yuv_pooled = false;
   int img_w = handle->width, img_h = handle->height;
   if (handle->format == PIXEL_FMT_YCBCR_420_SP ||
       handle->format == PIXEL_FMT_YCRCB_420_SP ||
       handle->format == PIXEL_FMT_YCBCR_420_P ||
       handle->format == PIXEL_FMT_YCRCB_420_P) {
      static unsigned yuv_count;
      static long long yuv_total_us;
      long long t0 = ohos_now_us();
      yuv_fd = ohos_yuv_to_rgba_memfd(handle, &yuv_stride, &yuv_pooled, &img_w, &img_h);
      yuv_total_us += ohos_now_us() - t0;
      if (++yuv_count % 30 == 0) {
         long rss, shr;
         ohos_mem_mb(&rss, &shr);
         _eglLog(_EGL_WARNING,
                 "ohos: yuv->rgba %dx%d -> %dx%d: %u frames, avg %lld us each; process memory %ld MB (shared %ld MB)",
                 handle->width, handle->height, img_w, img_h, yuv_count,
                 yuv_total_us / yuv_count, rss, shr);
      }
      if (yuv_fd < 0)
         return NULL;
   }
   int fds[4] = {yuv_fd >= 0 ? yuv_fd : handle->fd, -1, -1, -1};
   int strides[4] = {yuv_fd >= 0 ? yuv_stride : handle->stride, 0, 0, 0};
   int offsets[4] = {0, 0, 0, 0};
   int nplanes = 1;
   /* sweet gralloc: YUV planes packed one after the other in the same
    * dma-buf, luma pitch = stride, luma height = buffer height */
   int ysize = handle->stride * handle->height;
   switch (fourcc) {
   case DRM_FORMAT_NV12:
   case DRM_FORMAT_NV21:
      nplanes = 2;
      fds[1] = handle->fd;
      strides[1] = handle->stride;
      offsets[1] = ysize;
      break;
   case DRM_FORMAT_YUV420:
   case DRM_FORMAT_YVU420:
      nplanes = 3;
      fds[1] = fds[2] = handle->fd;
      strides[1] = strides[2] = handle->stride / 2;
      offsets[1] = ysize;
      offsets[2] = ysize + (handle->stride / 2) * ((handle->height + 1) / 2);
      break;
   default:
      break;
   }

   struct dri_image *img = dri2_from_dma_bufs(
      dri2_dpy->dri_screen_render_gpu, img_w, img_h, fourcc,
      DRM_FORMAT_MOD_LINEAR, fds, nplanes, strides, offsets,
      __DRI_YUV_COLOR_SPACE_ITU_REC601, __DRI_YUV_NARROW_RANGE,
      __DRI_YUV_CHROMA_SITING_0, __DRI_YUV_CHROMA_SITING_0, 0, &error, priv);
   if (!img) {
      img = dri2_from_dma_bufs(
         dri2_dpy->dri_screen_render_gpu, img_w, img_h,
         fourcc, DRM_FORMAT_MOD_INVALID, fds, nplanes, strides, offsets,
         __DRI_YUV_COLOR_SPACE_ITU_REC601, __DRI_YUV_NARROW_RANGE,
         __DRI_YUV_CHROMA_SITING_0, __DRI_YUV_CHROMA_SITING_0, 0, &error,
         priv);
   }
   if (yuv_fd >= 0 && !yuv_pooled)
      close(yuv_fd);
   if (!img)
      _eglLog(_EGL_WARNING,
              "ohos: dma-buf import failed (%dx%d fmt %d stride %d err %u)",
              handle->width, handle->height, handle->format, handle->stride,
              error);
   return img;
}

/* ------------------------------------------------------------------------ */
/* window surfaces (software rendering)                                     */
/*                                                                          */
/* Mesa renders with softpipe into CPU memory.  On eglSwapBuffers the sw    */
/* winsys hands the frame to our swrast loader (putImage2), which copies it */
/* into a dma-buf dequeued from the OpenHarmony window; the buffer is then  */
/* queued to render_service.                                                */
/* ------------------------------------------------------------------------ */

static int
ohos_bytes_per_pixel(int native_format)
{
   return native_format == PIXEL_FMT_RGB_565 ? 2 : 4;
}

static bool
ohos_window_dequeue(struct dri2_egl_surface *dri2_surf)
{
   int fence_fd = -1;

   if (dri2_surf->buffer)
      return true;

   if (!dri2_surf->window ||
       ANativeWindow_dequeueBuffer(dri2_surf->window, &dri2_surf->buffer,
                                   &fence_fd)) {
      dri2_surf->buffer = NULL;
      return false;
   }

   /* the CPU copy below must not start before the producer is done */
   if (fence_fd >= 0) {
      sync_wait(fence_fd, 1000);
      close(fence_fd);
   }
   return true;
}

static void
ohos_put_image(struct dri_drawable *draw, int op, int x, int y, int w, int h,
               int stride, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (dri2_surf->base.Type != EGL_WINDOW_BIT || !data)
      return;
   if (!ohos_window_dequeue(dri2_surf))
      return;

   BufferHandle *bh = GetBufferHandleFromNative(dri2_surf->buffer);
   if (!bh || bh->fd < 0 || bh->size == 0)
      return;

   const int bpp = ohos_bytes_per_pixel(bh->format);
   uint8_t *dst = bh->virAddr;
   bool mapped = false;
   if (!dst) {
      dst = mmap(NULL, bh->size, PROT_READ | PROT_WRITE, MAP_SHARED, bh->fd, 0);
      if (dst == MAP_FAILED) {
         _eglLog(_EGL_WARNING, "ohos: mmap of window buffer failed: %s",
                 strerror(errno));
         return;
      }
      mapped = true;
   }

   /* clip: drivers may submit out-of-bounds rectangles */
   int x0 = x < 0 ? 0 : x;
   int y0 = y < 0 ? 0 : y;
   int x1 = x + w > bh->width ? bh->width : x + w;
   int y1 = y + h > bh->height ? bh->height : y + h;
   if (x1 > x0) {
      for (int row = y0; row < y1; row++) {
         memcpy(dst + (size_t)row * bh->stride + (size_t)x0 * bpp,
                (const uint8_t *)data + (size_t)(row - y) * stride +
                   (size_t)(x0 - x) * bpp,
                (size_t)(x1 - x0) * bpp);
      }
   }

   if (mapped)
      munmap(dst, bh->size);
}

static void
ohos_put_image_simple(struct dri_drawable *draw, int op, int x, int y, int w,
                      int h, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int bpp = 4;
   if (dri2_surf->window) {
      int fmt = ANativeWindow_getFormat(dri2_surf->window);
      bpp = ohos_bytes_per_pixel(fmt);
   }
   ohos_put_image(draw, op, x, y, w, h, w * bpp, data, loaderPrivate);
}

static void
ohos_get_image2(struct dri_drawable *read, int x, int y, int w, int h,
                int stride, char *data, void *loaderPrivate)
{
   /* Nothing is kept CPU-side: window contents are write-only for GL. */
   for (int row = 0; row < h; row++)
      memset(data + (size_t)row * stride, 0, (size_t)w * 4);
}

static void
ohos_get_image(struct dri_drawable *read, int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   ohos_get_image2(read, x, y, w, h, w * 4, data, loaderPrivate);
}

static void
ohos_get_drawable_info(struct dri_drawable *draw, int *x, int *y, int *w,
                       int *h, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int32_t width = dri2_surf->base.Width, height = dri2_surf->base.Height;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
      int32_t gh = 0, gw = 0;
      if (NativeWindowHandleOpt(dri2_surf->window, GET_BUFFER_GEOMETRY, &gh,
                                &gw) == 0 && gw > 0 && gh > 0) {
         width = gw;
         height = gh;
         dri2_surf->base.Width = gw;
         dri2_surf->base.Height = gh;
      }
   }
   *x = *y = 0;
   *w = width;
   *h = height;
}

static const __DRIswrastLoaderExtension ohos_swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 4},
   .getDrawableInfo = ohos_get_drawable_info,
   .putImage = ohos_put_image_simple,
   .getImage = ohos_get_image,
   .putImage2 = ohos_put_image,
   .getImage2 = ohos_get_image2,
};

static _EGLSurface *
ohos_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                    void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   struct ANativeWindow *window = native_window;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "ohos_create_surface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list, true,
                          native_window))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      if (ANativeWindow_getFormat(window) < 0) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "ohos_create_surface");
         goto cleanup_surface;
      }
      int32_t height = 0, width = 0;
      NativeWindowHandleOpt(window, GET_BUFFER_GEOMETRY, &height, &width);
      dri2_surf->base.Width = width;
      dri2_surf->base.Height = height;
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   if (window) {
      ANativeWindow_acquire(window);
      dri2_surf->window = window;
   }

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
ohos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                           void *native_window, const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                              attrib_list);
}

static _EGLSurface *
ohos_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
ohos_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      if (dri2_surf->buffer)
         ANativeWindow_cancelBuffer(dri2_surf->window, dri2_surf->buffer, -1);
      if (dri2_surf->window)
         ANativeWindow_release(dri2_surf->window);
   }

   driDestroyDrawable(dri2_surf->dri_drawable);
   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
ohos_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
   /* render_service paces itself with its own vsync */
   surf->SwapInterval = interval;
   return EGL_TRUE;
}

static EGLBoolean
ohos_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return EGL_TRUE;

   /* flushes the frame; the sw winsys calls ohos_put_image() */
   driSwapBuffers(dri2_surf->dri_drawable);

   if (dri2_surf->buffer) {
      ANativeWindow_queueBuffer(dri2_surf->window, dri2_surf->buffer, -1);
      dri2_surf->buffer = NULL;
   }
   return EGL_TRUE;
}

static EGLint
ohos_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   return 0;
}

static EGLBoolean
ohos_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute,
                   EGLint *value)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int32_t width = 0, height = 0;

   switch (attribute) {
   case EGL_WIDTH:
   case EGL_HEIGHT:
      if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
         NativeWindowHandleOpt(dri2_surf->window, GET_BUFFER_GEOMETRY, &height,
                               &width);
         *value = attribute == EGL_WIDTH ? width : height;
         return EGL_TRUE;
      }
      break;
   default:
      break;
   }
   return _eglQuerySurface(disp, surf, attribute, value);
}

/* ------------------------------------------------------------------------ */
/* EGLImage from a native buffer                                            */
/* ------------------------------------------------------------------------ */

static _EGLImage *
dri2_create_image_ohos_native_buffer(_EGLDisplay *disp, _EGLContext *ctx,
                                     struct ANativeWindowBuffer *buf)
{
   if (ctx != NULL) {
      _eglError(EGL_BAD_CONTEXT, "eglCreateEGLImageKHR: for "
                                 "EGL_NATIVE_BUFFER_OHOS, the context must be "
                                 "EGL_NO_CONTEXT");
      return NULL;
   }

   if (!buf) {
      _eglError(EGL_BAD_PARAMETER, "eglCreateEGLImageKHR");
      return NULL;
   }

   struct dri_image *dri_image =
      ohos_create_image_from_native_buffer(disp, buf, buf);
   if (dri_image) {
      /* keep the native buffer alive as long as the EGLImage */
      NativeObjectReference(buf);
      return dri2_create_image_from_dri(disp, dri_image);
   }

   return NULL;
}

static _EGLImage *
ohos_create_image_khr(_EGLDisplay *disp, _EGLContext *ctx, EGLenum target,
                      EGLClientBuffer buffer, const EGLint *attr_list)
{
   switch (target) {
   case EGL_NATIVE_BUFFER_OHOS:
      return dri2_create_image_ohos_native_buffer(
         disp, ctx, (struct ANativeWindowBuffer *)buffer);
   default:
      return dri2_create_image_khr(disp, ctx, target, buffer, attr_list);
   }
}

static int
ohos_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format,
                       uint32_t *stamp, void *loaderPrivate,
                       uint32_t buffer_mask, struct __DRIimageList *images)
{
   /* software surfaces never use image buffers */
   images->image_mask = 0;
   images->front = NULL;
   images->back = NULL;
   return 0;
}

static void
ohos_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
ohos_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static void
ohos_destroy_loader_image_state(void *loaderPrivate)
{
   if (loaderPrivate)
      NativeObjectUnreference(loaderPrivate);
}

static void
ohos_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   static const struct {
      int format;
      enum pipe_format pipe_format;
   } visuals[] = {
      {PIXEL_FMT_RGBA_8888, PIPE_FORMAT_RGBA8888_UNORM},
      {PIXEL_FMT_RGBX_8888, PIPE_FORMAT_RGBX8888_UNORM},
      {PIXEL_FMT_RGB_565, PIPE_FORMAT_B5G6R5_UNORM},
      {PIXEL_FMT_BGRA_8888, PIPE_FORMAT_BGRA8888_UNORM},
   };

   unsigned int format_count[ARRAY_SIZE(visuals)] = {0};
   bool has_rgba = false;

   for (int i = 0; i < ARRAY_SIZE(visuals); i++) {
      if (visuals[i].format == PIXEL_FMT_BGRA_8888 && has_rgba)
         continue;
      for (int j = 0; dri2_dpy->driver_configs[j]; j++) {
         const struct gl_config *gl_config =
            (struct gl_config *)dri2_dpy->driver_configs[j];
         enum pipe_format linear_format =
            util_format_linear(gl_config->color_format);
         if (linear_format != visuals[i].pipe_format)
            continue;

         const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
         const EGLint config_attrs[] = {
            EGL_NATIVE_VISUAL_ID,   visuals[i].format,
            EGL_NATIVE_VISUAL_TYPE, visuals[i].format,
            EGL_NONE,
         };

         struct dri2_egl_config *dri2_conf = dri2_add_config(
            disp, dri2_dpy->driver_configs[j], surface_type, config_attrs);
         if (dri2_conf)
            format_count[i]++;
      }

      if (visuals[i].format == PIXEL_FMT_RGBA_8888 && format_count[i])
         has_rgba = true;
   }

   for (int i = 0; i < ARRAY_SIZE(format_count); i++) {
      _eglLog(_EGL_DEBUG, "ohos: native format 0x%x -> %u DRI configs",
              visuals[i].format, format_count[i]);
   }
}

static const struct dri2_egl_display_vtbl ohos_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = ohos_create_window_surface,
   .create_pbuffer_surface = ohos_create_pbuffer_surface,
   .destroy_surface = ohos_destroy_surface,
   .create_image = ohos_create_image_khr,
   .swap_buffers = ohos_swap_buffers,
   .swap_interval = ohos_swap_interval,
   .query_buffer_age = ohos_query_buffer_age,
   .query_surface = ohos_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static const __DRIimageLoaderExtension ohos_image_loader_extension = {
   .base = {__DRI_IMAGE_LOADER, 4},

   .getBuffers = ohos_image_get_buffers,
   .flushFrontBuffer = ohos_flush_front_buffer,
   .getCapability = ohos_get_capability,
   .flushSwapBuffers = NULL,
   .destroyLoaderImageState = ohos_destroy_loader_image_state,
};

static const __DRIextension *ohos_swrast_loader_extensions[] = {
   &ohos_swrast_loader_extension.base,
   &ohos_image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

/* ------------------------------------------------------------------------ */
/* display initialisation: pure software, no DRM / KGSL node                */
/* ------------------------------------------------------------------------ */

EGLBoolean
dri2_initialize_ohos(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   const char *err;

   dri2_dpy->fd_render_gpu = -1;
   dri2_dpy->fd_display_gpu = -1;

   struct _egl_device *device = _eglFindDevice(-1, true);
   if (_eglHasAttrib(disp, EGL_DEVICE_EXT) && disp->Device != device) {
      err = "DRI2: wrong EGLDevice";
      goto cleanup;
   }
   disp->Device = device;

   dri2_dpy->driver_name = strdup("swrast");
   if (!dri2_dpy->driver_name) {
      err = "DRI2: out of memory";
      goto cleanup;
   }

   dri2_detect_swrast_kopper(disp);
   dri2_dpy->kopper = false;
   dri2_dpy->loader_extensions = ohos_swrast_loader_extensions;

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create swrast screen";
      goto cleanup;
   }

   dri2_setup_screen(disp);
   dri2_setup_swap_interval(disp, 1);

   disp->Extensions.KHR_image = EGL_TRUE;

   ohos_add_configs_for_visuals(disp);

   dri2_dpy->vtbl = &ohos_display_vtbl;

   return EGL_TRUE;

cleanup:
   return _eglError(EGL_NOT_INITIALIZED, err);
}
