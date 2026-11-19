import re
data = open('/tmp/ueventd.bin', 'rb').read()
for off in (0x25b0, 0x2620, 0x25f0, 0x25d0):
    print(hex(off), repr(data[off:off+48]))
