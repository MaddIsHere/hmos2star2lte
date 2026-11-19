import sys
sys.path.insert(0, '.')
from lib import bootimg
for path in ('out/boot_test.img', 'out/boot.img'):
    h = bootimg.parse_header(path)
    c = h['cmdline']
    print(path, len(c), 'default_boot_device=' in c or 'default_boot_device' in c)
    print('  tail:', c[-100:])
