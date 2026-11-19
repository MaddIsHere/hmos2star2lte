#!/bin/bash
set -e
grep -n '^000000000000fb34' /tmp/ueventd.dis
sed -n '/^000000000000fb34/,/^000000000000fc/p' /tmp/ueventd.dis
