import re
data = open('/tmp/ueventd.bin', 'rb').read()
for off, ln in ((0x26a0, 0x50), (0x29b80, 0x40)):
    print(hex(off), data[off:off+ln])
    print()
# find what is at vaddr 0x29b98 in file (same offset, rodata)
print('0x29b98 ->', data[0x29b98:0x29bb0])
