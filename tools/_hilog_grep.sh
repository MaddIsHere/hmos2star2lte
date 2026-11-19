#!/bin/bash
# Temporary helper: grep decoded hilog logs by keyword, keeping timestamp/pid/tag.
#   tools/_hilog_grep.sh <dir> <regex>
cd "${1:?dir}" || exit 1
shift
grep -h -i -e "$@" *.log | sort -n | head -60
