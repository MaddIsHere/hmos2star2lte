import re
data = open('/tmp/ueventd.bin', 'rb').read()
for m in re.finditer(rb'[ -~]{3,}', data[0x3300:0x3460]):
    print(hex(0x3300 + m.start()), m.group().decode())
