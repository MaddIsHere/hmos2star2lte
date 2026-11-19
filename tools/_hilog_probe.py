#!/usr/bin/env python3
"""Temporary helper: dump one decoded hilog line with readable context, or grep raw payloads.

    tools/_hilog_probe.py <file.gz> <tag-substring> [count]
"""
import gzip
import sys


def main():
    path, want = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    buf = gzip.open(path, "rb").read()
    n = len(buf)
    pos = 0
    shown = 0
    while pos + 28 < n and shown < limit:
        plen = int.from_bytes(buf[pos:pos + 2], "little")
        packed = int.from_bytes(buf[pos + 2:pos + 4], "little")
        sec = int.from_bytes(buf[pos + 4:pos + 8], "little")
        if (packed & 0x07) != 0 or plen < 8 or pos + 28 + plen > n:
            pos += 1
            continue
        tag_len = (packed >> 10) & 0x3F
        if not (2 <= tag_len <= plen):
            pos += 1
            continue
        payload = buf[pos + 28:pos + 28 + plen]
        tag = payload[:tag_len]
        if tag[-1] != 0:
            pos += 1
            continue
        tag = tag[:-1].decode("ascii", "replace")
        if want in tag:
            print(f"{sec} tag={tag!r} level={(packed >> 6) & 7} pid={int.from_bytes(buf[pos + 16:pos + 20], 'little')}")
            print(f"  payload ({plen} B): {payload[tag_len:]!r}")
            shown += 1
        pos += 28 + plen


if __name__ == "__main__":
    main()
