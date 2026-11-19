#!/bin/bash
# one-off: extract readable strings out of the binary hilog persister files
set -e
cd /tmp
strings -a -n 6 h0.bin > h0.txt
strings -a -n 6 h1.bin > h1.txt
wc -l h0.txt h1.txt
echo '=== display/composer/wms hits ==='
grep -n -iE 'composer|hdf_devhost|/dev/dri|drm|fb0|vsync|wms' h0.txt h1.txt | head -60
