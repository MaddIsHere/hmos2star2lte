#!/bin/bash
# OpenHarmony 5.1 sources needed by the userspace components (headers + HUKS core).
set -e
. "$(dirname "$0")/env.sh"
TAG=OpenHarmony-v5.1.0-Release
# The composer/buffer VDI sources were written against the 5.0 display interfaces (5.1 adds FastPresent,
# UpdateHardwareCursor and PresentParam to the VDI, which the vendored hdi-gen headers do not have).
TAG_DRV=${TAG_DRV:-OpenHarmony-v5.0.3-Release}
mkdir -p "$OH_SRC"; cd "$OH_SRC"
# "dir:repo" where the local folder name differs from the gitee repository (the build scripts expect hdf_core/)
for r in drivers_interface drivers_peripheral hdf_core:drivers_hdf_core graphic_graphic_surface commonlibrary_c_utils \
         communication_ipc hiviewdfx_hilog hiviewdfx_hitrace third_party_libdrm third_party_bounds_checking_function \
         security_huks third_party_openssl; do
  d=${r%%:*}; repo=${r##*:}
  t=$TAG; case "$d" in drivers_interface|drivers_peripheral) t=$TAG_DRV ;; esac
  # gitee drops connections from some networks (CI): retry, and never keep a half-cloned folder
  if [ ! -d "$d/.git" ]; then
    ok=0
    for try in 1 2 3 4 5 6; do
      rm -rf "$d"
      if git -c http.version=HTTP/1.1 -c http.postBuffer=524288000 clone --depth 1 -b "$t" "https://gitee.com/openharmony/$repo.git" "$d"; then ok=1; break; fi
      echo "clone of $repo failed (try $try), retrying in $((try*10)) s"; sleep $((try*10))
    done
    [ $ok = 1 ] || { echo "ERROR: could not clone $repo"; exit 1; }
  fi
done
echo "OpenHarmony sources in $OH_SRC"
