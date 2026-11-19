#!/usr/bin/env python3
"""Decode the binary hilog persister chunks (hilog.NNN.*.gz) into text lines.

Record layout (little endian):

    off  size  field
      0     2  payload length (bytes following this header)
      2     2  bitfield: version:3, logType:3, level:3, flag:1, tagLen:6
      4     4  tv_sec
      8     4  tv_nsec
     12     4  mono_sec
     16     4  pid
     20     4  tid
     24     4  domain
     28     n  tag (tagLen bytes, NUL terminated)
            .  8 byte content hash, then the message text and optional
               binary privacy parameters

The message text is the longest printable run of the payload, which skips the
binary hash even when one of its bytes is printable.

    tools/_hilog_decode.py <pull-dir> [out-dir]
"""
import gzip
import os
import sys

MIN_SEC = 1767200000
MAX_SEC = 1767600000
LEVELS = "DIWEFA"


def best_text(payload):
    """Longest printable run in a payload (message text)."""
    best = start = 0
    best_len = 0
    run = 0
    for i, c in enumerate(payload):
        printable = 0x20 <= c <= 0x7E or c == 0x09
        if printable:
            if run == 0:
                start = i
            run += 1
            if run > best_len:
                best_len = run
                best = start
        else:
            run = 0
    if best_len < 4:
        return ""
    return payload[best:best + best_len].decode("ascii", "replace")


def records(buf):
    n = len(buf)
    pos = 0
    while pos + 28 < n:
        plen = int.from_bytes(buf[pos:pos + 2], "little")
        packed = int.from_bytes(buf[pos + 2:pos + 4], "little")
        sec = int.from_bytes(buf[pos + 4:pos + 8], "little")
        nsec = int.from_bytes(buf[pos + 8:pos + 12], "little")
        if (packed & 0x07) != 0 or not (MIN_SEC <= sec <= MAX_SEC) or nsec >= 1000000000:
            pos += 1
            continue
        if pos + 28 + plen > n or plen < 8:
            pos += 1
            continue
        tag_len = (packed >> 10) & 0x3F
        if tag_len < 2 or tag_len > plen:
            pos += 1
            continue
        payload = buf[pos + 28:pos + 28 + plen]
        tag = payload[:tag_len]
        if tag[-1] != 0:
            pos += 1
            continue
        tag = tag[:-1]
        if any(c < 0x20 or c > 0x7E for c in tag):
            pos += 1
            continue
        yield (sec, nsec,
               (packed >> 3) & 0x07,               # log type
               (packed >> 6) & 0x07,               # level
               int.from_bytes(buf[pos + 16:pos + 20], "little"),
               int.from_bytes(buf[pos + 20:pos + 24], "little"),
               int.from_bytes(buf[pos + 24:pos + 28], "little"),
               tag.decode("ascii", "replace"),
               best_text(payload[tag_len:]))
        pos += 28 + plen


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/hmos-logs2/hilog"
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/hilog-dec"
    os.makedirs(out, exist_ok=True)
    total = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".gz"):
            continue
        with gzip.open(os.path.join(src, name), "rb") as f:
            buf = f.read()
        lines = sorted(records(buf), key=lambda r: (r[0], r[1]))
        dst = os.path.join(out, name[:-3] + ".log")
        with open(dst, "w", encoding="utf-8", errors="replace") as f:
            for sec, nsec, ltype, level, pid, tid, domain, tag, text in lines:
                f.write(f"{sec}.{nsec // 1000:06d} {pid:5d} {tid:5d} "
                        f"{LEVELS[level] if level < len(LEVELS) else '?'} "
                        f"T{ltype} D{domain:08x} {tag}: {text}\n")
        print(f"{name}: {len(lines)} records -> {dst}")
        total += len(lines)
    print(f"{total} records")


if __name__ == "__main__":
    main()
