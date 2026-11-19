#!/usr/bin/env python3
"""Temporary helper: one-screen summary of a pulled log folder (CACHE + faultlog).

    python3 tools/_boot_report.py /mnt/c/hmos-logs5 [--frames N]

Reads cache/hmos/kmsg-*.log, cache/hmos/boot-*/cmdline.txt, cache/hmos/boot-*/snap*-stacks.txt
and faultlog/faultlogger. Decodes sec_debug.reset_reason/rst_stat of every kept boot, which is the
only record of how the boot before it ended.
"""
import glob
import os
import re
import sys

RR = {1: "S(SW reset)", 2: "W", 3: "D", 4: "K(KERNEL PANIC)", 5: "M", 6: "P", 7: "R(reboot)",
      8: "B", 9: "N(normal power on)", 10: "T(thermal)", 11: "C"}
# S2MPS18 RST_STAT bits, drivers/staging/samsung/sec_debug_reset_reason.c
RST = ["CLUSTER1_DBGRSTREQ0", "CLUSTER1_DBGRSTREQ1", "CLUSTER1_DBGRSTREQ2", "CLUSTER1_DBGRSTREQ3",
       "CLUSTER0_DBGRSTREQ0", "CLUSTER0_DBGRSTREQ1", "CLUSTER0_DBGRSTREQ2", "CLUSTER0_DBGRSTREQ3",
       "CLUSTER1_WARMRSTREQ0", "CLUSTER1_WARMRSTREQ1", "CLUSTER1_WARMRSTREQ2", "CLUSTER1_WARMRSTREQ3",
       "CLUSTER0_WARMRSTREQ0", "CLUSTER0_WARMRSTREQ1", "CLUSTER0_WARMRSTREQ2", "CLUSTER0_WARMRSTREQ3",
       "PINRESET", "-", "-", "CHUB_CPU_WDT", "CORTEXM23_APM_WDT", "CORTEXM23_APM_SYSRST", "VTS_CPU_WDT",
       "CLUSTER1_WDT", "CLUSTER0_WDT", "AUD_CA7_WDT", "SSS_WDT", "-", "WRESET", "SWRESET", "-", "-"]
PWR_OFF = ["RSVD0", "TSD", "TIMEOUT", "LDO3OK", "PWRHOLD", "RSVD5", "RSVD6", "UVLOB"]


def bits(value, names):
    return " ".join(n for i, n in enumerate(names) if value & (1 << i) and n != "-") or "-"


def cmdline_reason(path):
    try:
        text = open(path, errors="replace").read()
    except OSError:
        return None
    opts = dict(t.split("=", 1) for t in text.split() if "=" in t)
    reason = int(opts.get("sec_debug.reset_reason", "0") or 0)
    rst = int(opts.get("sec_debug.rst_stat", "0") or 0, 16)
    off = int(opts.get("sec_debug.pwroffsrc", "0") or 0, 16)
    return "%s  rst_stat=%#x [%s]  pwroffsrc=%#x [%s]" % (
        RR.get(reason, str(reason)), rst, bits(rst, RST), off, bits(off, PWR_OFF))


def main():
    root = sys.argv[1]
    frames = 8
    if "--frames" in sys.argv:
        frames = int(sys.argv[sys.argv.index("--frames") + 1])
    cache = os.path.join(root, "cache", "hmos")
    if not os.path.isdir(cache):
        cache = os.path.join(root, "cache")  # /cache/hmos pulled directly into cache/

    print("== boots kept")
    for kmsg in sorted(glob.glob(os.path.join(cache, "kmsg-*.log")),
                       key=lambda p: int(re.search(r"kmsg-(\d+)", p).group(1))):
        idx = re.search(r"kmsg-(\d+)", kmsg).group(1)
        first = last = ""
        lines = 0
        with open(kmsg, errors="replace") as f:
            for line in f:
                lines += 1
                if lines == 1:
                    first = line.strip()[-60:]
                last = line.strip()[-90:]
        cmd = os.path.join(cache, "boot-%s" % idx, "cmdline.txt")
        print("  kmsg-%s.log %8d lines" % (idx, lines))
        print("    first: %s" % first)
        print("    last : %s" % last)
        reason = cmdline_reason(cmd)
        print("    reset before this boot: %s" % (reason if reason else "(no cmdline.txt)"))

    print("== frame timing (composer, every 30th frame)")
    pat = re.compile(r"frame (\d+): (\d+) ms since last, (\d+) ms in present")
    for kmsg in sorted(glob.glob(os.path.join(cache, "kmsg-*.log"))):
        seen = []
        with open(kmsg, errors="replace") as f:
            for line in f:
                m = pat.search(line)
                if m:
                    seen.append("%s: frame %s, %s ms between, %s ms presenting" % (
                        os.path.basename(kmsg), m.group(1), m.group(2), m.group(3)))
        for s in seen[-frames:]:
            print("  " + s)
        if not seen:
            print("  %s: none" % os.path.basename(kmsg))

    print("== freezes")
    for p in sorted(glob.glob(os.path.join(root, "faultlog", "faultlogger", "*"))):
        try:
            head = open(p, errors="replace").read(1500)
        except OSError:
            continue
        reason = re.search(r"^Reason:(.+)$", head, re.M)
        pid = re.search(r"^Pid:(\d+)", head, re.M)
        print("  %-58s %-18s pid %s" % (os.path.basename(p)[:58],
                                        reason.group(1).strip() if reason else "?",
                                        pid.group(1) if pid else "?"))

    print("== render_service / composer_host thread states (last snapshot)")
    for d in sorted(glob.glob(os.path.join(cache, "boot-*")), key=lambda p: int(p.rsplit("-", 1)[1])):
        snaps = sorted(glob.glob(os.path.join(d, "snap*-stacks.txt")))
        if not snaps:
            continue
        text = open(snaps[-1], errors="replace").read()
        cur = None
        for line in text.splitlines():
            if line.startswith("== pid"):
                cur = line
                print("  [%s] %s" % (os.path.basename(snaps[-1]), cur))
            elif line.startswith("comm:"):
                print("      %s" % line.strip())


if __name__ == "__main__":
    main()
