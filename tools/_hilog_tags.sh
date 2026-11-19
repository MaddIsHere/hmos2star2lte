#!/bin/bash
# Temporary helper: tag histogram of decoded hilog logs (tools/_hilog_decode.py output).
# Line format: sec.nsec  pid  tid  L  Tn  Dhex  tag: text
cd "${1:-/tmp/hdec3}" || exit 1
shift
files=${*:-*.log}
awk '{print $7}' $files | cut -d: -f1 | sort | uniq -c | sort -rn | head -40
