#!/usr/bin/env python3
"""Temporary helper: grep a kmsg file with uptime filtering and context.

    python3 tools/_kmsg_grep.py <kmsg> <needle> [<needle> ...] [--from S] [--to S] [--ctx N] [--limit N]

Match is case-insensitive substring. Uptime comes from the ``pri,seq,usec,flags;`` prefix.
"""
import re
import sys

args = sys.argv[1:]
path = args.pop(0)
needles = []
frm = 0.0
to = 1e18
ctx = 0
limit = 200
i = 0
while i < len(args):
    if args[i] == "--from":
        i += 1
        frm = float(args[i])
    elif args[i] == "--to":
        i += 1
        to = float(args[i])
    elif args[i] == "--ctx":
        i += 1
        ctx = int(args[i])
    elif args[i] == "--limit":
        i += 1
        limit = int(args[i])
    else:
        needles.append(args[i].lower())
    i += 1

pat = re.compile(r"^\d+,\d+,(\d+),")
lines = open(path, errors="replace").read().splitlines()
shown = 0
for n, line in enumerate(lines):
    m = pat.match(line)
    if not m:
        continue
    sec = int(m.group(1)) / 1e6
    if not (frm <= sec <= to):
        continue
    if needles and not any(nd in line.lower() for nd in needles):
        continue
    lo = max(0, n - ctx)
    hi = min(len(lines), n + ctx + 1)
    for k in range(lo, hi):
        tag = ">" if k == n else " "
        print("%s%8.1fs %s" % (tag, sec, lines[k][:220]))
    shown += 1
    if shown >= limit:
        break
print("-- %d match(es)" % shown)
