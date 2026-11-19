#!/bin/bash
# one-off: rewrite /etc/fstab.<hw> inside an already built vendor.img (ext4),
# keeping owner/group/mode and the SELinux label of the original file.
# usage: _patch_vendor_fstab.sh <vendor.img> <hw> <new-fstab-file>
set -e
img=$1; hw=$2; new=$3
oldlabel=$(debugfs -R "ea_list /etc/fstab.$hw" "$img" 2>/dev/null | sed -n 's/.*security\.selinux ([0-9]*) = "\(.*\)"/\1/p')
echo "old label: ${oldlabel:-<none>}"
inode=$(debugfs -R "stat /etc/fstab.$hw" "$img" 2>/dev/null | sed -n 's/^Inode: \([0-9]*\).*/\1/p')
echo "inode: $inode"
debugfs -w -R "rm /etc/fstab.$hw" "$img" >/dev/null 2>&1
debugfs -w -R "write $new /etc/fstab.$hw" "$img" >/dev/null
debugfs -w -R "sif /etc/fstab.$hw mode 0100644" "$img" >/dev/null
debugfs -w -R "sif /etc/fstab.$hw uid 0" "$img" >/dev/null
debugfs -w -R "sif /etc/fstab.$hw gid 2000" "$img" >/dev/null
if [ -n "$oldlabel" ]; then
  printf '%s\0' "$oldlabel" > /tmp/_selabel.bin
  debugfs -w -R "ea_set -f /tmp/_selabel.bin /etc/fstab.$hw security.selinux" "$img" >/dev/null
fi
echo "--- new file ---"
debugfs -R "cat /etc/fstab.$hw" "$img" 2>/dev/null
debugfs -R "stat /etc/fstab.$hw" "$img" 2>/dev/null | grep -E 'Mode|User|Group|security'
