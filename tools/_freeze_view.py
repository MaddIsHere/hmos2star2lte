#!/usr/bin/env python3
"""Temporary helper: skim a HiviewDFX freeze log, skipping the event history.

    python3 tools/_freeze_view.py <file> [--skip N] [--limit N]
    python3 tools/_freeze_view.py <file> --grep <substr>
    python3 tools/_freeze_view.py <file> --threads [--frames N]
"""
import os
import re
import sys

args = sys.argv[1:]
path = args.pop(0)
skip = 0
limit = 120
grep = None
threads = False
frames = 6
i = 0
while i < len(args):
    if args[i] == "--skip":
        i += 1
        skip = int(args[i])
    elif args[i] == "--limit":
        i += 1
        limit = int(args[i])
    elif args[i] == "--grep":
        i += 1
        grep = args[i].lower()
    elif args[i] == "--threads":
        threads = True
    elif args[i] == "--frames":
        i += 1
        frames = int(args[i])
    i += 1

SKIP_RE = re.compile(r"^\s*(No\.\s+\d+ :|Event \{|History event queue)")


def short(frame):
    """Fold a ``#NN pc 0000... /path(lib(sym)+off)(hash)`` line into lib(sym)."""
    m = re.match(r"^#(\d+) (?:pc [0-9a-f]+ )?([^(]*?)\(([^)]*)\)", frame.strip())
    if m:
        return f"#{m.group(1)} {os.path.basename(m.group(2))}({m.group(3)})"
    return frame.strip()[:160]


if threads:
    with open(path, errors="replace") as f:
        cur = None
        shown = 0
        for line in f:
            if line.startswith("Tid:"):
                cur = line.strip()
                print(cur)
                shown = 0
                continue
            if cur is None or shown >= frames:
                continue
            if line.startswith("#"):
                print("    " + short(line))
                shown += 1
            elif line.lstrip().startswith("at "):
                print("    " + line.strip()[:160])
                shown += 1
    sys.exit(0)


def interesting(line):
    if SKIP_RE.match(line):
        return False
    if line.rstrip().endswith("}") and "send thread" in line:
        return False
    return True


shown = 0
with open(path, errors="replace") as f:
    for no, line in enumerate(f, 1):
        text = line.rstrip()
        if grep is not None:
            if grep in line.lower():
                print(f"{no}: {text}")
                shown += 1
                if shown >= limit:
                    break
            continue
        if not interesting(line):
            continue
        if skip:
            skip -= 1
            continue
        print(f"{no}: {text}")
        shown += 1
        if shown >= limit:
            break
