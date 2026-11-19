#!/bin/bash
# Run ONCE locally, after build/make_sysroot.sh. Produces build/sysroot-stubs.tar.xz: for every HarmonyOS
# library in the sysroot, an empty stub .so that has the same SONAME and exports the same symbol NAMES
# (no code, no data), which is all the linker needs. This is what lets GitHub Actions cross-build Mesa/LLVM
# without the Huawei OTA: the stub tarball contains no Huawei code and is safe to commit to a public repo.
# usage: build/make_stub_sysroot.sh
set -e
. "$(dirname "$0")/env.sh"
L="$SYSROOT/usr/lib"; [ -d "$L" ] || { echo "run build/make_sysroot.sh first"; exit 1; }
T="$WORK/stubs"; rm -rf "$T"; mkdir -p "$T/usr/lib"
n=0
for so in "$L"/*.so; do
  b=$(basename "$so")
  if [ -L "$so" ] && [ "$(readlink "$so")" = libc.so ]; then ln -s libc.so "$T/usr/lib/$b"; continue; fi
  [ -s "$so" ] || continue
  s="$T/$b.S"; : > "$s"
  # dynamic symbols that are defined (Ndx != UND): name, type
  llvm-readelf --dyn-syms --wide "$so" 2>/dev/null | awk 'NR>3 && $7!="UND" && $8!="" {print $4, $5, $8}' | sed 's/@.*//' | sort -u | \
  while read -r type bind name; do
    [ -n "$name" ] || continue
    case "$bind" in WEAK) printf '.weak %s\n' "$name" >> "$s";; *) printf '.globl %s\n' "$name" >> "$s";; esac
    case "$type" in
      OBJECT) printf '.type %s,%%object\n.size %s,8\n%s: .quad 0\n' "$name" "$name" "$name" >> "$s";;
      TLS)    ;;                                  # TLS symbols are not needed for linking here
      *)      printf '.type %s,%%function\n%s: ret\n' "$name" "$name" >> "$s";;
    esac
  done
  clang --target=aarch64-linux-ohos -nostdlib -shared -fuse-ld=lld -Wl,-soname,"$b" -Wl,--undefined-version \
        -x assembler "$s" -o "$T/usr/lib/$b" 2>/dev/null || { echo "stub failed: $b"; continue; }
  rm -f "$s"; n=$((n+1))
done
tar -cJf "$REPO/build/sysroot-stubs.tar.xz" -C "$T" usr
echo "$n stub libraries -> build/sysroot-stubs.tar.xz ($(du -h "$REPO/build/sysroot-stubs.tar.xz" | cut -f1))"
