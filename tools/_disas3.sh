#!/bin/bash
set -e
sed -n '/^    cb[cdef][0-9a-f]:/,/^    cf[0-9a-f][0-9a-f]:/p' /tmp/ueventd.dis
