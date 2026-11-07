#!/usr/bin/env python3
"""Make figures from results/results.csv and results/sizes.csv.

Needs matplotlib (run it on the host or anywhere with the CSV files):
    python3 plot.py [--resdir results]
"""
import argparse
import csv
import os
import statistics
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# colour-blind safe, one fixed colour per variant (extend when trie exists)
COLORS = {"base": "#7f7f7f", "chain": "#d55e00", "binsearch": "#0072b2",
          "trie": "#009e73"}


def quartiles(xs):
    if len(xs) < 2:
        return xs[0], xs[0]
    q = statistics.quantiles(xs, n=4)
    return q[0], q[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--resdir", default="results")
    a = ap.parse_args()

    data = defaultdict(list)  # (workload, variant) -> [seconds]
    workloads, variants = [], []
    with open(os.path.join(a.resdir, "results.csv")) as f:
        for r in csv.DictReader(f):
            data[(r["workload"], r["variant"])].append(float(r["seconds"]))
            if r["workload"] not in workloads:
                workloads.append(r["workload"])
            if r["variant"] not in variants:
                variants.append(r["variant"])

    # --- figure 1: slowdown vs baseline (median, bars = IQR) ---
    inst = [v for v in variants if v != "base"]
    fig, ax = plt.subplots(figsize=(7, 4))
    width = 0.8 / max(len(inst), 1)
    for i, v in enumerate(inst):
        xs, ys, lo, hi = [], [], [], []
        for j, w in enumerate(workloads):
            base = statistics.median(data[(w, "base")])
            med = statistics.median(data[(w, v)]) / base
            q1, q3 = quartiles(data[(w, v)])
            xs.append(j + i * width)
            ys.append(med)
            lo.append(med - q1 / base)
            hi.append(q3 / base - med)
        bars = ax.bar(xs, ys, width, label=v, color=COLORS.get(v, None),
                      yerr=[lo, hi], capsize=3)
        for b, y in zip(bars, ys):
            ax.annotate(f"{y:.2f}x", (b.get_x() + b.get_width() / 2, y),
                        ha="center", va="bottom", fontsize=8,
                        xytext=(0, 4), textcoords="offset points")
    ax.axhline(1.0, color="black", linewidth=0.8)
    ax.set_xticks([j + width * (len(inst) - 1) / 2 for j in range(len(workloads))])
    ax.set_xticklabels(workloads)
    ax.set_ylabel("run time / baseline (median, IQR)")
    ax.set_title("Run-time overhead of the CFI check variants")
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(a.resdir, "slowdown.png"), dpi=200)

    # --- figure 2: code size (.text) ---
    sizes = {}
    p = os.path.join(a.resdir, "sizes.csv")
    if os.path.exists(p):
        with open(p) as f:
            for r in csv.DictReader(f):
                if r["text"]:
                    sizes[r["variant"]] = int(r["text"])
    if sizes and "base" in sizes:
        fig, ax = plt.subplots(figsize=(5, 4))
        vs = [v for v in variants if v in sizes and v != "base"]
        delta = [(sizes[v] - sizes["base"]) / 1024 for v in vs]
        bars = ax.bar(vs, delta, color=[COLORS.get(v) for v in vs])
        for b, d, v in zip(bars, delta, vs):
            pct = 100.0 * (sizes[v] - sizes["base"]) / sizes["base"]
            ax.annotate(f"+{d:.1f} KiB ({pct:.1f}%)",
                        (b.get_x() + b.get_width() / 2, d), ha="center",
                        va="bottom", fontsize=8, xytext=(0, 4),
                        textcoords="offset points")
        ax.set_ylabel(".text size increase over baseline [KiB]")
        ax.set_title("Code size overhead")
        fig.tight_layout()
        fig.savefig(os.path.join(a.resdir, "codesize.png"), dpi=200)

    print("wrote slowdown.png" + (" and codesize.png" if sizes else ""))


if __name__ == "__main__":
    main()
