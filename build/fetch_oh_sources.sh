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
  [ -d "$d" ] || git clone --depth 1 -b "$t" "https://gitee.com/openharmony/$repo.git" "$d"
done
echo "OpenHarmony sources in $OH_SRC"
