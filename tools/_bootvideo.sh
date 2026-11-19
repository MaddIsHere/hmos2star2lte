#!/bin/bash
# Temporary helper: dump the boot video + config from sys_prod.img and report the video geometry.
set -u
cd ~/hmos2star2lte || exit 1
IMG=work/ota/sys_prod.img
rm -rf /tmp/ba && mkdir -p /tmp/ba
debugfs -R "dump /etc/bootanimation/bootanimation_custom_config.json /tmp/ba/config.json" "$IMG" 2>/dev/null
debugfs -R "dump /etc/bootanimation/cust_bootvideo.mp4 /tmp/ba/bootvideo.mp4" "$IMG" 2>/dev/null
ls -la /tmp/ba
echo "== config"
cat /tmp/ba/config.json
echo
echo "== mp4 boxes"
python3 - <<'EOF'
import struct
p = "/tmp/ba/bootvideo.mp4"
data = open(p, "rb").read()

def walk(buf, base, depth):
    i = 0
    while i + 8 <= len(buf):
        size, typ = struct.unpack(">I4s", buf[i:i+8])
        typ = typ.decode("latin1")
        if size == 1:
            size = struct.unpack(">Q", buf[i+8:i+16])[0]
        if size < 8 or i + size > len(buf):
            break
        print("  " * depth + f"{typ} size={size}")
        if typ in ("moov", "trak", "mdia", "minf", "stbl", "stsd"):
            off = 8
            if typ == "stsd":
                off = 16
            walk(buf[i+off:i+size], base + i + off, depth + 1)
        if typ in ("tkhd",):
            v = buf[i+8:i+size]
            if v[0] == 1:
                w, h = struct.unpack(">II", v[-8:])
            else:
                w, h = struct.unpack(">II", v[-8:])
            print("  " * (depth + 1) + f"track size {w/65536:.1f} x {h/65536:.1f}")
        i += size

walk(data, 0, 0)
EOF
