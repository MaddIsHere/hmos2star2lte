#!/bin/bash
# Temporary helper: list a directory inside an ext4 image, filtered by a keyword.
#   tools/_img_find.sh <img> <dir-in-image> [keyword]
img=${1:?img}
dir=${2:?dir}
pat=${3:-.}
debugfs -R "ls -l $dir" "$img" 2>/dev/null | grep -i -F "$pat" | head -40
