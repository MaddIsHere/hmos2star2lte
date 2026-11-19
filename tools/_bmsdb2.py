#!/usr/bin/env python3
"""Temporary helper: compact list of installed/preinstalled bundles from bmsdb.db (KEY/VALUE tables)."""
import sqlite3
import sys

db = sys.argv[1]
c = sqlite3.connect(db)

for table in ("installed_bundle", "preinstalled_bundle"):
    rows = list(c.execute(f"select KEY from {table}"))
    keys = [r[0] for r in rows]
    print(f"== {table}: {len(keys)} rows")
    for k in keys:
        s = str(k)
        if "scene" in s.lower() or "wms" in s.lower():
            print("   match:", s[:120])
    print("   sample:", keys[:6])
