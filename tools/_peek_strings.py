import re
data = open('/tmp/ueventd.bin', 'rb').read()
chunk = data[0x2d00:0x3300]
for m in re.finditer(rb'[ -~]{4,}', chunk):
    print(hex(0x2d00 + m.start()), m.group().decode())
