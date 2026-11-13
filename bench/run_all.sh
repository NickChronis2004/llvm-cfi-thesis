#!/bin/bash
# Run all CFI benchmarks and keep everything of one run in its own folder:
#   results/run_<date>/{results.csv,sizes.csv,slowdown.png,codesize.png,
#                       cfi_verbose_*.txt,run.log,meta.txt}
# Usage (inside the llvm-dev container, from /workspace/bench):
#   ./run_all.sh [cpu] [reps]        e.g.  ./run_all.sh 2 15
set -eo pipefail
cd "$(dirname "$0")"

CPU=${1:-2}
REPS=${2:-15}
OUT=results/run_$(date +%Y%m%d_%H%M)
mkdir -p "$OUT"

command -v taskset >/dev/null || echo "WARNING: taskset not found, runs are NOT pinned"

python3 run_bench.py --variants base,chain,binsearch,trie \
  --reps "$REPS" --cpu "$CPU" --outdir "$OUT" 2>&1 | tee "$OUT/run.log"
python3 plot.py --resdir "$OUT"

cp out/cfi_verbose_*.txt "$OUT"/ 2>/dev/null || true
{
  echo "date:       $(date -Is)"
  echo "cpu pinned: $CPU"
  echo "reps:       $REPS"
  echo "git commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  uname -a
  lscpu | grep -E 'Model name|^CPU\(s\)' || true
} > "$OUT/meta.txt"

echo "results in $OUT"
