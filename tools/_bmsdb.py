#!/usr/bin/env python3
"""Temporary helper: inspect the pulled bmsdb.db (installed bundles)."""
import sqlite3
import sys

db = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/hmos-logs2/db/q.db"
c = sqlite3.connect(db)
tables = [r[0] for r in c.execute("select name from sqlite_master where type='table'")]
print("tables:", tables)

for t in tables:
    cols = [d[1] for d in c.execute(f"pragma table_info({t})")]
    print(f"\n== {t} ({c.execute(f'select count(*) from {t}').fetchone()[0]} rows)")
    print("   cols:", cols)

for t in ("installed_bundle", "preinstalled_bundle"):
    if t not in tables:
        continue
    cols = [d[1] for d in c.execute(f"pragma table_info({t})")]
    namecol = next((x for x in cols if "name" in x.lower()), cols[1])
    rows = list(c.execute(f"select {namecol} from {t}"))
    names = [r[0] for r in rows]
    print(f"\n{t}: {len(names)} names")
    for n in names:
        if "scene" in str(n).lower() or "wms" in str(n).lower() or "foundation" in str(n).lower():
            print("   ", n)
