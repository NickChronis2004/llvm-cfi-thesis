#!/usr/bin/env python3
"""Median overhead of every variant vs 'base' and (if present) vs 'ltobase'.

    python3 compare.py results/<prog>/run_<date>

overhead = median(variant)/median(reference) - 1, in percent.
'ltobase' is the same -flto -fvisibility=hidden build as 'clangcfi' without
the check, so clangcfi-vs-ltobase is the cost of Clang's check alone.
"""
import csv, os, statistics, sys
from collections import defaultdict

d = sys.argv[1] if len(sys.argv) > 1 else "results"
t = defaultdict(list); wl = []; vs = []
for r in csv.DictReader(open(os.path.join(d, "results.csv"))):
    t[(r["workload"], r["variant"])].append(float(r["seconds"]))
    if r["workload"] not in wl: wl.append(r["workload"])
    if r["variant"] not in vs: vs.append(r["variant"])
med = {k: statistics.median(v) for k, v in t.items()}
refs = [v for v in ("base", "ltobase") if v in vs]
print(f"overhead [%] of median time (positive = slower), reps={len(next(iter(t.values())))}")
print(f"{'workload':<10}{'variant':<11}" + "".join(f"{'vs '+r:>12}" for r in refs))
for w in wl:
    for v in vs:
        row = "".join(f"{(med[(w,v)]/med[(w,r)]-1)*100:>+11.1f}%" for r in refs)
        print(f"{w:<10}{v:<11}{row}")
    print()
sz = os.path.join(d, "sizes.csv")
if os.path.exists(sz):
    rows = {r["variant"]: r for r in csv.DictReader(open(sz))}
    print(f"{'variant':<11}{'.text':>10}{'file':>12}{'d.text vs base':>16}")
    for v in vs:
        if v in rows and rows[v]["text"]:
            b = int(rows["base"]["text"]) if "base" in rows else None
            dd = f"{(int(rows[v]['text'])-b)/1024:>+13.1f} KiB" if b else ""
            print(f"{v:<11}{int(rows[v]['text']):>10}{int(rows[v]['file_bytes']):>12}{dd}")
