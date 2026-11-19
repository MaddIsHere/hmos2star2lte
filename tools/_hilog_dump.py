#!/usr/bin/env python3
"""Temporary helper: dump a few raw hilog records that carry a given tag."""
import gzip
import sys

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else "composer_host"
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 5

with gzip.open(path, "rb") as f:
    buf = f.read()

pos = 0
shown = 0
while shown < limit:
    i = buf.find(want.encode(), pos)
    if i < 0:
        break
    start = max(0, i - 40)
    rec = buf[start:i + 160]
    print(f"--- at {i} (tag offset {i}), record start guess {start}")
    print(" ".join(f"{b:02x}" for b in rec))
    print("".join(chr(b) if 32 <= b < 127 else "." for b in rec))
    pos = i + 1
    shown += 1
