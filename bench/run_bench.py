#!/usr/bin/env python3
"""Run the CFI measurements and write raw results to CSV.

Only needs the Python standard library (so it runs inside the llvm-dev
container). Plots are made separately by plot.py from the CSV files.

Example:
    python3 run_bench.py --reps 15 --cpu 2
"""
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def run_once(cmd, cpu):
    """Run one process; return (seconds, stdout). Raises on failure."""
    if cpu is not None and shutil.which("taskset"):
        cmd = ["taskset", "-c", str(cpu)] + cmd
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bindir", default=os.path.join(HERE, "out"))
    ap.add_argument("--workdir", default=os.path.join(HERE, "workloads"))
    ap.add_argument("--outdir", default=os.path.join(HERE, "results"))
    ap.add_argument("--variants", default="base,chain,binsearch",
                    help="comma list; binary is <bindir>/lua_<variant>")
    ap.add_argument("--workloads", default="fib,cfunc,cmix",
                    help="comma list; script is <workdir>/<name>.lua")
    ap.add_argument("--reps", type=int, default=15)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--cpu", type=int, default=None,
                    help="pin to this CPU with taskset (same CPU for all runs)")
    a = ap.parse_args()

    variants = a.variants.split(",")
    workloads = a.workloads.split(",")
    os.makedirs(a.outdir, exist_ok=True)

    def binary(v):
        return os.path.join(a.bindir, f"lua_{v}")

    def script(w):
        return os.path.join(a.workdir, f"{w}.lua")

    for v in variants:
        if not os.path.exists(binary(v)):
            sys.exit(f"missing binary: {binary(v)}")
    for w in workloads:
        if not os.path.exists(script(w)):
            sys.exit(f"missing workload: {script(w)}")

    # correctness: every variant must print the same as the baseline
    ref = variants[0]
    for w in workloads:
        _, expected = run_once([binary(ref), script(w)], a.cpu)
        for v in variants[1:]:
            _, got = run_once([binary(v), script(w)], a.cpu)
            if got != expected:
                sys.exit(f"OUTPUT MISMATCH: {v} vs {ref} on {w}")
    print("correctness: all variants produce identical output", flush=True)

    # warm-up (not recorded)
    for w in workloads:
        for v in variants:
            for _ in range(a.warmup):
                run_once([binary(v), script(w)], a.cpu)

    # measurement: variants are interleaved and the order is rotated every
    # repetition, so slow drift (thermal, other load) hits all variants alike
    rows = []
    for rep in range(a.reps):
        for w in workloads:
            k = rep % len(variants)
            order = variants[k:] + variants[:k]
            for v in order:
                dt, _ = run_once([binary(v), script(w)], a.cpu)
                rows.append((w, v, rep, dt))
        print(f"rep {rep + 1}/{a.reps}", flush=True)

    with open(os.path.join(a.outdir, "results.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["workload", "variant", "rep", "seconds"])
        wr.writerows(rows)

    with open(os.path.join(a.outdir, "sizes.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["variant", "text", "data", "bss", "file_bytes"])
        for v in variants:
            s = binary_sizes(binary(v)) or ("", "", "")
            wr.writerow([v, *s, os.path.getsize(binary(v))])

    # summary table
    print(f"\n{'workload':<10}{'variant':<12}{'median[s]':>10}{'IQR[s]':>9}"
          f"{'vs base':>9}")
    for w in workloads:
        base_med = None
        for v in variants:
            xs = [r[3] for r in rows if r[0] == w and r[1] == v]
            med = statistics.median(xs)
            if v == ref:
                base_med = med
            ratio = med / base_med if base_med else float("nan")
            print(f"{w:<10}{v:<12}{med:>10.3f}{iqr(xs):>9.3f}{ratio:>8.2f}x")
    print(f"\nwrote {a.outdir}/results.csv and sizes.csv "
          f"(reps={a.reps}, cpu={a.cpu})")


if __name__ == "__main__":
    main()
