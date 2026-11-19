#!/bin/bash
set -e
sed -n '/^    ce[0-9a-f][0-9a-f]:/,/^    cf[cdef][0-9a-f]:/p' /tmp/ueventd.dis
