#!/bin/bash
set -e
sed -n '/^    10420:/,/^    10580:/p' /tmp/ueventd.dis
