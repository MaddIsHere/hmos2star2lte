#!/bin/bash
# Mesa 26.1 software GL (softpipe/swrast, OHOS EGL platform) -> patches/11-gpu-mesa/files.
# MESA_SRC: a Mesa 26.1-devel tree (tested: mesa-for-android-container-adreno, 26.1.0-devel).
# Needs: meson >= 1.4, ninja, python3-mako, python3-yaml, glslang (host), bison, flex.
set -e
. "$(dirname "$0")/env.sh"
O=$OH_SRC; M=$WORK/mesa-build; mkdir -p "$M/pc"
cd "$MESA_SRC"
PATCH=$REPO/src/mesa
if ! grep -q with_platform_ohos meson.build; then
  patch -p1 -N < "$PATCH/0001-mesa-ohos-platform-for-sweet.patch" || true
  # only the EGL->hilog logging hunk (egllog.c) may fail on other Mesa snapshots: it is not needed for rendering
  bad=$(find . -name '*.rej' ! -name 'egllog.c.rej' | head -5)
  [ -z "$bad" ] || { echo "ERROR: patch hunks rejected: $bad"; exit 1; }
  rm -f src/egl/main/egllog.c.rej src/egl/main/egllog.c.orig
  grep -q with_platform_ohos meson.build || { echo "ERROR: meson.build was not patched"; exit 1; }
fi
# --- fixes on top of the base patch (all idempotent) -------------------------
python3 - <<'PYEOF'
import re
def sub(path, fn):
    s = open(path).read(); n = fn(s)
    if n != s: open(path, 'w').write(n)
# 'ohos' as an allowed value of -Dplatforms
sub('meson.options', lambda s: s if "'ohos'" in s else re.sub(r"('macos',?)(\s*\])", r"'macos', 'ohos'\2", s, count=1) if re.search(r"'macos',?\s*\]", s) else s.replace("'macos',", "'macos', 'ohos',", 1))
# 'ohos' as an allowed value of -Degl-native-platform
def eglplat(s):
    i = s.find("'egl-native-platform'")
    if i < 0: return s
    j = s.find(")", i)
    blk = s[i:j]
    if "'ohos'" in blk: return s
    blk2 = blk.replace("'surfaceless'", "'surfaceless', 'ohos'", 1) if "'surfaceless'" in blk else blk.replace("'android'", "'android', 'ohos'", 1)
    return s[:i] + blk2 + s[j:]
sub('meson.options', eglplat)
# EGL_PLATFORM_OHOS_KHR fallback
sub('src/egl/main/eglapi.c', lambda s: s if 'define EGL_PLATFORM_OHOS_KHR' in s else
    s.replace('#include', '#ifndef EGL_PLATFORM_OHOS_KHR\n#define EGL_PLATFORM_OHOS_KHR 0x34E0\n#endif\n#include', 1))
# EGL_NATIVE_BUFFER_OHOS fallback
sub('src/egl/drivers/dri2/platform_ohos.h', lambda s: s if 'define EGL_NATIVE_BUFFER_OHOS' in s else
    s.replace('#endif /* EGL_OHOS_INCLUDED */', '#ifndef EGL_NATIVE_BUFFER_OHOS\n#define EGL_NATIVE_BUFFER_OHOS 0x34E1\n#endif\n\n#endif /* EGL_OHOS_INCLUDED */'))
# dri2 display/surface members used by the OHOS platform + prototype
def hdr(s):
    s = s.replace('#ifdef HAVE_ANDROID_PLATFORM\n   bool has_native_fence_fd;', '#if defined(HAVE_ANDROID_PLATFORM) || defined(HAVE_OHOS_PLATFORM)\n   bool has_native_fence_fd;')
    lines = s.split('\n')
    for i, l in enumerate(lines):
        if l.strip() == '#ifdef HAVE_ANDROID_PLATFORM' and i in (293, 365):
            lines[i] = '#if defined(HAVE_ANDROID_PLATFORM) || defined(HAVE_OHOS_PLATFORM)'
    s = '\n'.join(lines)
    if 'dri2_initialize_ohos' not in s:
        s = s.replace('#ifdef HAVE_ANDROID_PLATFORM\nEGLBoolean\ndri2_initialize_android', '#ifdef HAVE_OHOS_PLATFORM\nEGLBoolean\ndri2_initialize_ohos(_EGLDisplay *disp);\n#endif\n\n#ifdef HAVE_ANDROID_PLATFORM\nEGLBoolean\ndri2_initialize_android', 1)
    return s
sub('src/egl/drivers/dri2/egl_dri2.h', hdr)
PYEOF
# the real platform implementation: software rendering, no kgsl/DRM
cp "$PATCH/platform_ohos.c" src/egl/drivers/dri2/platform_ohos.c
# dma-buf import in the sw winsys: no leak, honour the exporter pitch
patch -p1 -N --dry-run < "$PATCH/0002-sw-winsys-dmabuf-import.patch" >/dev/null 2>&1 && patch -p1 -N < "$PATCH/0002-sw-winsys-dmabuf-import.patch"
pc() { printf 'Name: %s\nDescription: %s\nVersion: %s\nLibs: -L%s -l%s\nCflags: %s\n' "$1" "$1" "$2" "$SYSROOT/usr/lib" "$3" "$4" > "$M/pc/$1.pc"; }
pc libdrm 2.4.120 drm "-I$O/third_party_libdrm -I$O/third_party_libdrm/include/drm"
pc libhilog 1.0 hilog "-I$O/hiviewdfx_hilog/interfaces/native/innerkits/include"
pc libsurface 1.0 surface.z "-I$O/graphic_graphic_surface/interfaces/inner_api/surface -I$O/graphic_graphic_surface/interfaces/inner_api/utils -I$O/drivers_peripheral/display/interfaces/include -I$O/graphic_graphic_surface/interfaces/inner_api/common"
cat > "$M/cross.ini" <<EOC
[binaries]
c = '$REPO/build/ohos-clang'
cpp = '$REPO/build/ohos-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-O2', '-fno-emulated-tls', '-Wno-error', '-Wno-int-conversion', '-Wno-incompatible-function-pointer-types']
cpp_args = ['-O2', '-fno-emulated-tls', '-Wno-error']
c_link_args = ['-Wl,--no-undefined', '-Wl,--undefined-version', '$SYSROOT/usr/lib/libc.so']
cpp_link_args = ['-Wl,--no-undefined', '-Wl,--undefined-version', '$SYSROOT/usr/lib/libc++.so', '$SYSROOT/usr/lib/libc.so']
[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$M/pc'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOC
# LLVM_PREFIX (build/build_llvm.sh) set -> llvmpipe (JIT, multithreaded); otherwise softpipe only
GALLIUM=softpipe; LLVMOPT=disabled; EXTRA=()
if [ -n "${LLVM_PREFIX:-}" ] && [ -x "$LLVM_PREFIX/bin/llvm-config-host" ]; then
  GALLIUM=llvmpipe,softpipe; LLVMOPT=enabled
  sed -i "s#^pkg-config = .*#&\nllvm-config = '$LLVM_PREFIX/bin/llvm-config-host'#" "$M/cross.ini"
  EXTRA=(-Dshared-llvm=disabled -Dcpp_rtti=false)
  # llvm-config's 'native' component is empty here (native = x86 build machine, only AArch64 is built), so the AArch64
  # backend never reaches the link line: pass the libraries explicitly (cpp_link_args sit inside the
  # --start-group of the link, so the order does not matter)
  LLVMLIBS=$("$LLVM_PREFIX/bin/llvm-config-host" --link-static --libfiles bitwriter core engine executionengine instcombine \
      mcdisassembler mcjit native scalaropts transformutils coroutines lto irreader ipo vectorize \
      aarch64codegen aarch64asmparser aarch64disassembler aarch64desc aarch64info aarch64utils asmprinter selectiondag globalisel | tr ' ' '\n' | grep . | sed "s/.*/'&'/" | paste -sd, -)
  [ -n "$LLVMLIBS" ] || { echo "ERROR: llvm-config returned no libraries"; exit 1; }
  sed -i "s#^cpp_link_args = \[#cpp_link_args = [$LLVMLIBS, #" "$M/cross.ini"
  echo "LLVM archives added to cpp_link_args: $(echo "$LLVMLIBS" | tr ',' '\n' | wc -l)"
fi
rm -rf "$M/b"
meson setup "$M/b" --force-fallback-for=zlib -Dzlib:default_library=static --cross-file "$M/cross.ini" --buildtype=release \
  -Dplatforms=ohos -Degl-native-platform=ohos -Dandroid-stub=false -Dgallium-drivers=$GALLIUM -Dvulkan-drivers= \
  -Dgles1=enabled -Dgles2=enabled -Dopengl=false -Dgbm=disabled -Dglx=disabled -Degl=enabled -Dllvm=$LLVMOPT "${EXTRA[@]}"
ninja -C "$M/b"
F=$REPO/patches/11-gpu-mesa/files/lib64/chipsetsdk; mkdir -p "$F"
# real libraries only: meson also leaves *.symbols helper files with similar names
pick() { find "$M/b" -name "$1" -type f ! -name '*.symbols' ! -name '*.p' ! -name '*.a' | head -1; }
llvm-strip -o "$F/libEGL_impl.so" "$(pick 'libEGL.so*')"
llvm-strip -o "$F/libGLESv1_impl.so" "$(pick 'libGLESv1_CM.so*')"
llvm-strip -o "$F/libGLESv2_impl.so" "$(pick 'libGLESv2.so*')"
cp "$F/libGLESv2_impl.so" "$F/libGLESv3_impl.so"
llvm-strip -o "$F/libgallium-26.1.0-devel.so" "$(pick 'libgallium-*.so')"
cp "$(pick 'libexpat.so.1.*')" "$F/libexpat.so.1"
llvm-strip "$F/libexpat.so.1"
echo "Mesa -> $F"
