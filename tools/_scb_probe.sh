#!/bin/bash
# Temporary helper: list SceneBoard copies in the built images and the boot animation resources.
set -u
cd ~/hmos2star2lte || exit 1
for img in work/ota/system.img out/system.img; do
    echo "== $img /system/app"
    tools/_img_find.sh "$img" /system/app sceneboard
done
echo "== system/app (all *Scene*)"
tools/_img_find.sh work/ota/system.img /system/app Scene
echo "== system/etc bootanim"
tools/_img_find.sh work/ota/system.img /system/etc bootanim
echo "== system/media"
tools/_img_find.sh work/ota/system.img /system/media boot
echo "== system/etc/init bootanim"
tools/_img_find.sh work/ota/system.img /system/etc/init bootanim
