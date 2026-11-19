#!/usr/bin/env python3
"""Temporary helper: statistics of the plaintext hilog stream saved in the CACHE partition.

    python3 tools/_rs_stats.py /mnt/c/hmos-logs2/cache/boot-0/hilog-stream-6.txt

Prints the time span, per-minute composition attempts (RCD), render_service lines and errors per tag.
"""
import collections
import re
import sys

path = sys.argv[1]
line_re = re.compile(r"^(\d\d-\d\d \d\d:\d\d):(\d\d)\.\d+\s+(\d+)\s+(\d+) ([EWIDF]) [A-Z0-9]+/(\S+?): (.*)$")

per_min = collections.Counter()
rs_min = collections.Counter()
tags = collections.Counter()
errors = collections.Counter()
first = last = None
last_rs = None
rs_lines = 0

with open(path, "r", errors="replace") as f:
    for line in f:
        m = line_re.match(line.rstrip("\n"))
        if not m:
            continue
        minute, sec, tid, mtag = m.group(1), m.group(2), m.group(3), m.group(6)
        level, tag = m.group(5), m.group(6)
        if first is None:
            first = minute + ":" + sec
        last = minute + ":" + sec
        if tag.startswith("render_service/"):
            rs_lines += 1
            last_rs = minute + ":" + sec
            rs_min[minute] += 1
            tags[tag.split("/", 1)[1]] += 1
        if "RcdProcessInfo" in line:
            per_min[minute] += 1
        if level in "EWF":
            errors[tag] += 1

print("span", first, "->", last)
print("render_service lines", rs_lines, "last", last_rs)
print("compositions (RCD) per minute:")
for k in sorted(per_min):
    print("  ", k, per_min[k])
print("render_service lines per minute (last 15):")
for k in sorted(rs_min)[-15:]:
    print("  ", k, rs_min[k])
print("error/warn tags in stream (top 15):")
for tag, n in errors.most_common(15):
    print("  ", n, tag)
