#!/bin/bash
# LLVM for llvmpipe, cross-built for aarch64-linux-ohos with ohos-clang, static libraries only.
#   native step : llvm-tblgen + llvm-config for the build machine (llvm-config is only used by meson)
#   target step : LLVM libraries (AArch64 backend + MCJIT/ORC) installed to $LLVM_PREFIX
# Produces $LLVM_PREFIX and $LLVM_PREFIX/bin/llvm-config-host (a wrapper meson can run on the build machine).
# Needs: cmake, ninja, clang/lld, python3, a sysroot ($SYSROOT: real, or the stubs from the repo).
set -e
. "$(dirname "$0")/env.sh"
LLVM_VER=${LLVM_VER:-19.1.7}
: "${LLVM_PREFIX:=$WORK/llvm-ohos}"
S=$WORK/llvm-project-$LLVM_VER
if [ ! -d "$S/llvm" ]; then
  mkdir -p "$WORK"; cd "$WORK"
  curl -fL --retry 3 -o llvm.tar.xz "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"
  tar -xf llvm.tar.xz; mv "llvm-project-$LLVM_VER.src" "$S"; rm llvm.tar.xz
fi
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_ASSERTIONS=OFF
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
  -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF)
# --- native tools -------------------------------------------------------------
NB=$WORK/llvm-native
if [ ! -x "$NB/bin/llvm-config" ]; then
  cmake -S "$S/llvm" -B "$NB" "${COMMON[@]}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
  ninja -C "$NB" llvm-tblgen llvm-config
fi
# --- target libraries ---------------------------------------------------------
TB=$WORK/llvm-target
cmake -S "$S/llvm" -B "$TB" "${COMMON[@]}" \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="$REPO/build/ohos-clang" -DCMAKE_CXX_COMPILER="$REPO/build/ohos-clang++" \
  -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_FLAGS="-O2 -g0" -DCMAKE_CXX_FLAGS="-O2 -g0 -fno-emulated-tls" \
  -DCMAKE_EXE_LINKER_FLAGS="$SYSROOT/usr/lib/libc++.so $SYSROOT/usr/lib/libc.so" \
  -DCMAKE_SHARED_LINKER_FLAGS="$SYSROOT/usr/lib/libc++.so $SYSROOT/usr/lib/libc.so" \
  -DCMAKE_MODULE_LINKER_FLAGS="$SYSROOT/usr/lib/libc++.so $SYSROOT/usr/lib/libc.so" \
  -DLLVM_HOST_TRIPLE=aarch64-linux-ohos -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-ohos \
  -DLLVM_TABLEGEN="$NB/bin/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$NB/bin" \
  -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_BACKTRACES=OFF -DLLVM_ENABLE_PIC=ON \
  -DLLVM_ENABLE_THREADS=ON -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_ENABLE_BINDINGS=OFF \
  -DCMAKE_INSTALL_PREFIX="$LLVM_PREFIX"
# the helper executables link first (fast): a libc++/stub problem shows up now, not after the long compile
ninja -C "$TB" llvm-tblgen llvm-min-tblgen 2>&1 | tail -30; [ -x "$TB/bin/llvm-tblgen" ] || { echo "target llvm-tblgen did not link"; exit 1; }
ninja -C "$TB"
ninja -C "$TB" install
# --- llvm-config meson can run: the native one, with paths redirected to the target install -------------------
mkdir -p "$LLVM_PREFIX/bin"
cat > "$LLVM_PREFIX/bin/llvm-config-host" <<EOW
#!/bin/bash
# native llvm-config (same version, same targets) answering with the ohos install
"$NB/bin/llvm-config" "\$@" | sed -e "s#$NB/include#$LLVM_PREFIX/include#g" -e "s#$NB/lib#$LLVM_PREFIX/lib#g" -e "s#$NB/bin#$LLVM_PREFIX/bin#g" -e "s#$NB#$LLVM_PREFIX#g" -e "s#$S/llvm/include#$LLVM_PREFIX/include#g"
EOW
chmod +x "$LLVM_PREFIX/bin/llvm-config-host"
echo "LLVM $LLVM_VER for ohos -> $LLVM_PREFIX ($(du -sh "$LLVM_PREFIX/lib" | cut -f1) of libraries)"
