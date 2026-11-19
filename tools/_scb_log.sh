#!/bin/bash
# Temporary helper: SceneBoard / bundle install timeline in the last boot's hilog stream.
S=${1:-/mnt/c/hmos-logs2/cache/boot-0/hilog-stream-6.txt}
echo "== /system/app/SceneBoard references"
grep -a -F "/system/app/SceneBoard" "$S" | cut -c1-180 | head -10
echo "== BootStartInstall"
grep -a -F "BootStartInstall" "$S" | cut -c1-160 | head -20
echo "== BMSInstaller errors"
grep -a "foundation/BMSInstaller" "$S" | grep -a -E " E | W " | cut -c1-170 | head -20
echo "== HandleSceneBoard"
grep -a -i "HandleSceneBoard" "$S" | cut -c1-170 | head -10
echo "== bundle install failures"
grep -a -i "install" "$S" | grep -a -i -E "fail|error" | cut -c1-170 | head -20
