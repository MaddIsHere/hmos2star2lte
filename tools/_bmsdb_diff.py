#!/usr/bin/env python3
"""Temporary helper: diff preinstalled vs installed bundles from bmsdb.db.

    python3 tools/_bmsdb_diff.py <db>
"""
import sqlite3
import sys

c = sqlite3.connect(sys.argv[1])
pre = [str(r[0]) for r in c.execute("select KEY from preinstalled_bundle")]
ins = set(str(r[0]) for r in c.execute("select KEY from installed_bundle"))
missing = sorted(b for b in pre if b not in ins)
if len(sys.argv) > 2 and sys.argv[2] == "names":
    for b in missing:
        print(b)
    sys.exit(0)
print(f"preinstalled {len(pre)}, installed {len(ins)}, missing {len(missing)}")
for b in missing:
    print("missing:", b[:100])
