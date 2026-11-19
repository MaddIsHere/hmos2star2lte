#!/usr/bin/env python3
"""Temporary helper: print hilog lines of one boot in a time window, filtered by tag substring.

    python3 tools/_stream_window.py <stream> <from HH:MM:SS> <to HH:MM:SS> <tag-substr> [limit]

The stream lines look like: "01-04 05:13:25.750  1321  2221 W C01400/tag: message".
"""
import sys

path, t0, t1, tag = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 80
shown = 0
with open(path, errors="replace") as f:
    for line in f:
        if len(line) < 19 or line[6] == " ":
            continue
        ts = line[6:18]
        if ts < t0 or ts > t1:
            continue
        if tag.lower() in line.lower():
            print(line.rstrip()[:220])
            shown += 1
            if shown >= limit:
                break
print(f"({shown} lines)")
