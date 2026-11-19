import sys
sys.path.insert(0, '.')
from lib import bootimg
for path in ('out/boot.img', 'out/boot_test.img'):
    h = bootimg.parse_header(path)
    print('==', path, len(h['cmdline']))
    for tok in h['cmdline'].split():
        print('   ', tok)
