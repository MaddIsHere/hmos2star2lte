#!/usr/bin/env python3
"""Temporary helper: summarize a preinit snap*-stacks.txt (pid/comm/wchan per thread).

    python3 tools/_stacks_view.py <snap*-stacks.txt> [--pid N] [--comm substr] [--full]
"""
import re
import sys

path = sys.argv[1:]
if not path:
    sys.exit("usage: _stacks_view.py <file> [--pid N] [--comm substr] [--full]")
f = path.pop(0)
want_pid = None
want_comm = None
full = False
i = 0
while i < len(path):
    if path[i] == "--pid":
        i += 1
        want_pid = path[i]
    elif path[i] == "--comm":
        i += 1
        want_comm = path[i].lower()
    elif path[i] == "--full":
        full = True
    i += 1

cur = None
lines = open(f, errors="replace").read().splitlines()
n = 0
for k, line in enumerate(lines):
    m = re.match(r"== pid (\d+) tid (\d+)", line)
    if m:
        cur = {"pid": m.group(1), "tid": m.group(2), "comm": "", "wchan": "", "stack": []}
        continue
    if cur is None:
        continue
    if line.startswith("comm:"):
        cur["comm"] = line[5:].strip()
    elif line.startswith("wchan:"):
        cur["wchan"] = line[6:].strip()
    elif line.startswith("stack:"):
        cur["stack"] = line[6:].strip()
        # collect continuation frames
        j = k + 1
        while j < len(lines) and lines[j].startswith("["):
            cur["stack"] += " " + lines[j].strip()
            j += 1
        if want_pid and cur["pid"] != want_pid:
            cur = None
            continue
        if want_comm and want_comm not in cur["comm"].lower():
            cur = None
            continue
        desc = cur["stack"] if full else " | ".join(re.findall(r"\] ([A-Za-z0-9_.]+)\+", cur["stack"])[:4])
        print("pid %-5s tid %-5s %-28s %-24s %s" % (cur["pid"], cur["tid"], cur["comm"][:28],
                                                     cur["wchan"][:24], desc))
        n += 1
        cur = None
print("-- %d thread(s)" % n)
