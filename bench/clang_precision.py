#!/usr/bin/env python3
"""How many targets does Clang's CFI allow at each indirect call site?

    python3 clang_precision.py <out dir> <clangcfi binary> [--ours cfi_verbose_trie.txt]

Needs the files written by build_clangcfi.sh: <out dir>/ll_clangcfi/*.ll
(unoptimised IR, one llvm.type.test per indirect call) and the linked
binary (the jump-table sizes are read from its __typeid_* symbols).

 * allowed(type id) = size_m1+1 of its jump table; a table without a
   size_m1 symbol is a single-entry table (table spacing 16 B = 1 entry of
   8 B, checked for SQLite and Lua).
 * a site is matched with a site of OUR pass by (function name, k-th site in
   that function). Functions whose site counts differ are reported, not
   matched.
Writes <out dir>/clang_sites.csv (function,idx,typeid,clang_allowed[,our_allowed]).
"""
import argparse, collections, glob, os, re, statistics, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("outdir"); ap.add_argument("binary")
ap.add_argument("--ours", help="cfi_verbose_*.txt of our pass (SITE,<fn>,<n> lines)")
a = ap.parse_args()

# jump-table size per type id, from the binary
size = {}; tables = set()
for l in subprocess.run(["nm", a.binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) != 3: continue
    m = re.match(r"__typeid_(.*)_(global_addr|size_m1)$", p[2])
    if not m: continue
    if m.group(2) == "global_addr": tables.add(m.group(1))
    else: size[m.group(1)] = int(p[0], 16) + 1
for t in tables: size.setdefault(t, 1)

# call sites from the IR
DEF = re.compile(r'^define\b.*@("([^"]+)"|[\w.$-]+)\(')
TT = re.compile(r'@llvm\.type\.test\([^,]+,\s*metadata !"([^"]+)"\)')
sites = []                       # (function, idx, typeid)
cnt = collections.Counter(); fn = None
for path in sorted(glob.glob(os.path.join(a.outdir, "ll_clangcfi", "*.ll"))):
    for line in open(path, errors="replace"):
        m = DEF.match(line)
        if m: fn = m.group(2) or m.group(1); continue
        m = TT.search(line)
        if m and fn:
            sites.append((fn, cnt[fn], m.group(1))); cnt[fn] += 1
if not sites: sys.exit("no llvm.type.test found - run build_clangcfi.sh first")

# a site whose type id has no table has no legal target at all (always traps)
missing = {t for _, _, t in sites if t not in size}
rows = [(f, i, t, size.get(t, 0)) for f, i, t in sites]
known = [r[3] for r in rows]
print(f"clang: {len(tables)} jump tables, {len(sites)} sites, "
      f"{sum(1 for k in known if k == 0)} of them with 0 allowed targets "
      f"({len(missing)} type ids without a table)")
print(f"clang allowed/site: mean {statistics.mean(known):.1f}  median "
      f"{statistics.median(known):.0f}  max {max(known)}  "
      f"sites with 0 / 1 target: {sum(1 for k in known if k == 0)} / {sum(1 for k in known if k == 1)}")

ours = None
if a.ours:
    ours = collections.defaultdict(list)
    for l in open(a.ours):
        if l.startswith("SITE,"):
            _, f, n = l.strip().split(",")
            ours[f].append(int(n))
    print(f"ours:  {sum(map(len, ours.values()))} sites, mean allowed "
          f"{statistics.mean(x for v in ours.values() for x in v):.1f}")
    pairs = []; skipped = 0
    byfn = collections.defaultdict(list)
    for f, i, t, n in rows: byfn[f].append(n)
    for f, cl in byfn.items():
        o = ours.get(f)
        if o is None or len(o) != len(cl): skipped += len(cl); continue
        pairs += list(zip(cl, o))
    if pairs:
        c = [p[0] for p in pairs]; o = [p[1] for p in pairs]
        print(f"matched {len(pairs)} sites ({skipped} clang sites not matched):")
        print(f"  mean allowed  clang {statistics.mean(c):.1f}   ours {statistics.mean(o):.1f}")
        print(f"  sites where ours allows more: {sum(1 for x, y in pairs if y > x)}, "
              f"equal: {sum(1 for x, y in pairs if y == x)}, fewer: {sum(1 for x, y in pairs if y < x)}")
        print(f"  largest gaps (ours-clang): "
              f"{sorted(((y - x) for x, y in pairs), reverse=True)[:5]}")

with open(os.path.join(a.outdir, "clang_sites.csv"), "w") as f:
    f.write("function,idx,typeid,clang_allowed\n")
    for r in rows: f.write(",".join(str(x) for x in r) + "\n")
print("wrote", os.path.join(a.outdir, "clang_sites.csv"))
