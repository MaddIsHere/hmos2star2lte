#!/usr/bin/env python3
"""Make statx(2) report STATX_ATTR_VERITY (bit 20) as supported, and set, for regular files.

HarmonyOS libcode_sign_utils decides "fs-verity supported" with
syscall(__NR_statx, AT_FDCWD, path, 0, 0xfff, &stx) and tests STATX_ATTR_VERITY in stx_attributes_mask.
Samsung's 4.9 tree has statx but no filesystem reports verity, so installd answers
CS_ERR_FSVREITY_NOT_SUPPORTED (-0x301) and every HAP with native code fails to install (SceneBoard included).
Together with CONFIG_OHOS_FSVERITY_STUB (the ioctls) this makes code signing a no-op.

Idempotent.  usage: add_statx.py <kernel tree>
"""
import sys

stat_c = sys.argv[1] + "/fs/stat.c"
src = open(stat_c).read()
if "OHOS: fake fs-verity" in src:
    print("add_statx: fs/stat.c already patched")
    sys.exit(0)

anchor = "\ttmp.stx_attributes_mask = stat->attributes_mask;\n"
if src.count(anchor) != 1:
    sys.exit("add_statx: cp_statx() in fs/stat.c does not look as expected (anchor found %d times)" % src.count(anchor))

add = anchor + (
    "\t/* OHOS: fake fs-verity (CONFIG_OHOS_FSVERITY_STUB): claim support (STATX_ATTR_VERITY = 1<<20) and report\n"
    "\t * it enabled on regular files, so libcode_sign_utils takes the fs-verity path */\n"
    "\ttmp.stx_attributes_mask |= 0x00100000ULL;\n"
    "\tif (S_ISREG(stat->mode))\n"
    "\t\ttmp.stx_attributes |= 0x00100000ULL;\n"
)
open(stat_c, "w").write(src.replace(anchor, add))
print("add_statx: cp_statx now reports STATX_ATTR_VERITY")
