#!/bin/bash
# Temporary helper: show decoded hilog lines of one pid in time order.
#   tools/_hilog_pid.sh <dir> <pid> [from-sec] [to-sec] [count]
cd "${1:?dir}" || exit 1
pid=${2:?pid}
from=${3:-0}
to=${4:-9999999999}
count=${5:-80}
awk -v p="$pid" -v f="$from" -v t="$to" '$2 == p && $1 >= f && $1 <= t {print}' *.log |
    sort -n | head -n "$count"
