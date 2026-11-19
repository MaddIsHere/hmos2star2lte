import sys
sys.path.insert(0, '.')
from lib.device import Device
d = Device('star2lte')
text = d.fstab()
open('/tmp/fstab.samsungexynos9810', 'w', newline='\n').write(text)
print(text)
