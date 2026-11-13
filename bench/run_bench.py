#!/usr/bin/env python3
"""Run the CFI measurements for one program and write raw results to CSV.

Only needs the Python standard library (so it runs inside the llvm-dev
container). Plots are made separately by plot.py from the CSV files.

The program under test is described by a small JSON spec (see specs/):

    {
      "name":      "lua",                  # label used in the output
      "bindir":    "out",                  # relative to this script, or absolute
      "binprefix": "lua",                  # binary = <bindir>/<binprefix>_<variant>
      "normalize": "regex",                # optional: removed from every line of
                                           #   stdout before comparing variants
                                           #   (use it for timings in the output)
      "workloads": ["fib", "cfunc"],       # list of names -> "args_template"
      "args_template": ["{HERE}/workloads/{name}.lua"],
      #   ... or a dict  {"name": ["arg", "arg", ...]}  with explicit arguments
    }

{HERE} is the directory of this script. Examples:

    python3 run_bench.py --spec specs/lua.json --reps 15 --cpu 2 --outdir out_dir
    python3 run_bench.py --spec specs/sqlite.json --workloads main,cte
"""
import argparse
import csv
import glob
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def run_once(cmd, cpu, cwd):
    """Run one process; return (seconds, stdout). Raises on failure."""
    if cpu is not None and shutil.which("taskset"):
        cmd = ["taskset", "-c", str(cpu)] + cmd
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       cwd=cwd)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} exited with {p.returncode}: "
                           f"{p.stderr.decode(errors='replace')[:200]}")
    return dt, p.stdout


def binary_sizes(binary):
    """Return (text, data, bss) from `size` (Berkeley format), or None."""
    if not shutil.which("size"):
        return None
    out = subprocess.run(["size", binary], stdout=subprocess.PIPE,
                         universal_newlines=True).stdout.splitlines()
    if len(out) < 2:
        return None
    f = out[1].split()
    return int(f[0]), int(f[1]), int(f[2])


def iqr(xs):
    if len(xs) < 2:
        return 0.0
    q = statistics.quantiles(xs, n=4)
    return q[2] - q[0]


def load_spec(path):
    with open(path) as f:
        spec = json.load(f)
    for key in ("name", "bindir", "binprefix", "workloads"):
        if key not in spec:
            sys.exit(f"{path}: missing key '{key}'")
    if isinstance(spec["workloads"], list) and "args_template" not in spec:
        sys.exit(f"{path}: 'workloads' is a list but 'args_template' is missing")
    return spec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", default=os.path.join(HERE, "specs", "lua.json"))
    ap.add_argument("--bindir", default=None,
                    help="override the bindir of the spec")
    ap.add_argument("--outdir", default=None,
                    help="default: results/<name>/run_<time> next to this script")
    ap.add_argument("--variants", default="base,chain,binsearch,trie",
                    help="comma list; the FIRST one is the baseline")
    ap.add_argument("--workloads", default=None,
                    help="comma list; default: all workloads of the spec")
    ap.add_argument("--reps", type=int, default=15)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--cpu", type=int, default=None,
                    help="pin to this CPU with taskset (same CPU for all runs)")
    a = ap.parse_args()

    spec = load_spec(a.spec)
    bindir = a.bindir or spec["bindir"]
    if not os.path.isabs(bindir):
        bindir = os.path.join(HERE, bindir)
    outdir = a.outdir or os.path.join(
        HERE, "results", spec["name"], "run_" + time.strftime("%Y%m%d_%H%M"))
    norm = re.compile(spec["normalize"], re.M) if spec.get("normalize") else None

    # workload name -> argument list
    requested = a.workloads.split(",") if a.workloads else None
    if isinstance(spec["workloads"], dict):
        all_wl = {n: [s.replace("{HERE}", HERE) for s in args]
                  for n, args in spec["workloads"].items()}
        names = requested or list(all_wl)
        for n in names:
            if n not in all_wl:
                sys.exit(f"unknown workload '{n}' "
                         f"(spec has: {', '.join(all_wl)})")
    else:
        # list + args_template: any name works with --workloads, so a new
        # script (e.g. workloads/foo.lua) can be run without editing the spec
        names = requested or list(spec["workloads"])
        all_wl = {n: [s.replace("{HERE}", HERE).replace("{name}", n)
                      for s in spec["args_template"]]
                  for n in names}

    variants = a.variants.split(",")
    os.makedirs(outdir, exist_ok=True)

    def binary(v):
        return os.path.join(bindir, f"{spec['binprefix']}_{v}")

    for v in variants:
        if not os.path.exists(binary(v)):
            sys.exit(f"missing binary: {binary(v)}")
    if a.cpu is not None and not shutil.which("taskset"):
        print("WARNING: taskset not found, runs are NOT pinned", flush=True)

    def cmd(v, w):
        return [binary(v)] + all_wl[w]

    def normalized(out):
        text = out.decode(errors="replace")
        return norm.sub("", text) if norm else text

    # every run happens in a scratch directory, so programs that create
    # files (e.g. a database) cannot touch the repo or each other
    scratch = tempfile.mkdtemp(prefix="cfi_bench_")
    try:
        # correctness: every variant must print the same as the baseline
        ref = variants[0]
        for w in names:
            _, expected = run_once(cmd(ref, w), a.cpu, scratch)
            expected = normalized(expected)
            for v in variants[1:]:
                _, got = run_once(cmd(v, w), a.cpu, scratch)
                if normalized(got) != expected:
                    sys.exit(f"OUTPUT MISMATCH: {v} vs {ref} on {w}")
        print(f"[{spec['name']}] correctness: all variants produce the same "
              f"output as {ref}", flush=True)

        # warm-up (not recorded)
        for w in names:
            for v in variants:
                for _ in range(a.warmup):
                    run_once(cmd(v, w), a.cpu, scratch)

        # measurement: variants are interleaved and the order is rotated every
        # repetition, so slow drift (thermal, other load) hits all variants alike
        rows = []
        for rep in range(a.reps):
            for w in names:
                k = rep % len(variants)
                order = variants[k:] + variants[:k]
                for v in order:
                    dt, _ = run_once(cmd(v, w), a.cpu, scratch)
                    rows.append((w, v, rep, dt))
            print(f"rep {rep + 1}/{a.reps}", flush=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    with open(os.path.join(outdir, "results.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["workload", "variant", "rep", "seconds"])
        wr.writerows(rows)

    with open(os.path.join(outdir, "sizes.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["variant", "text", "data", "bss", "file_bytes"])
        for v in variants:
            s = binary_sizes(binary(v)) or ("", "", "")
            wr.writerow([v, *s, os.path.getsize(binary(v))])

    # keep the pass' verbose output (N per call site) next to the numbers
    for p in glob.glob(os.path.join(bindir, "cfi_verbose_*.txt")):
        shutil.copy(p, outdir)

    # summary table
    print(f"\n[{spec['name']}]")
    ww = max(10, max(len(w) for w in names) + 2)
    print(f"{'workload':<{ww}}{'variant':<12}{'median[s]':>10}{'IQR[s]':>9}"
          f"{'vs base':>9}")
    for w in names:
        base_med = None
        for v in variants:
            xs = [r[3] for r in rows if r[0] == w and r[1] == v]
            med = statistics.median(xs)
            if v == ref:
                base_med = med
            ratio = med / base_med if base_med else float("nan")
            print(f"{w:<{ww}}{v:<12}{med:>10.3f}{iqr(xs):>9.3f}{ratio:>8.2f}x")
    print(f"\nwrote {outdir}/results.csv and sizes.csv "
          f"(reps={a.reps}, cpu={a.cpu})")


if __name__ == "__main__":
    main()
