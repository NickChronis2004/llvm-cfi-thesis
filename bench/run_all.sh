#!/bin/bash
# Run the CFI benchmarks of one program and keep everything of the run in its
# own folder:  results/<program>/run_<date>/
#   results.csv sizes.csv slowdown.png codesize.png cfi_verbose_*.txt run.log meta.txt
#
# Usage (inside the llvm-dev container, from /workspace/bench):
#   ./run_all.sh [program] [cpu] [reps]
#   ./run_all.sh lua 2 15        # program = name of specs/<program>.json
#   ./run_all.sh sqlite 2 15
#   ./run_all.sh 2 15            # old form, same as "lua 2 15"
#   VARIANTS=base,ltobase,clangcfi,chain,binsearch,trie ./run_all.sh sqlite 2 15
#                                # with Clang's own CFI (build_clangcfi.sh first)
set -eo pipefail
cd "$(dirname "$0")"

PROG=lua
if [[ -n "$1" && ! "$1" =~ ^[0-9]+$ ]]; then PROG=$1; shift; fi
CPU=${1:-2}
REPS=${2:-15}
VARIANTS=${VARIANTS:-base,chain,binsearch,trie}

SPEC=specs/$PROG.json
[ -f "$SPEC" ] || { echo "no spec $SPEC (available: $(ls specs | tr '\n' ' '))"; exit 1; }

OUT=results/$PROG/run_$(date +%Y%m%d_%H%M)
mkdir -p "$OUT"

python3 run_bench.py --spec "$SPEC" --reps "$REPS" --cpu "$CPU" --variants "$VARIANTS" --outdir "$OUT" 2>&1 \
  | tee "$OUT/run.log"
python3 plot.py --resdir "$OUT" --title "$PROG"
python3 compare.py "$OUT" | tee "$OUT/compare.txt"

{
  echo "program:    $PROG"
  echo "date:       $(date -Is)"
  echo "cpu pinned: $CPU"
  echo "reps:       $REPS"
  echo "git commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
  uname -a
  lscpu | grep -E 'Model name|^CPU\(s\)' || true
} > "$OUT/meta.txt"

echo "results in $OUT"
