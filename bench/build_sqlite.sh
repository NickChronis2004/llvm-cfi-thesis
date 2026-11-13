#!/bin/bash
# Build SQLite (amalgamation) + its speedtest1 driver in every CFI variant.
# Same pipeline as build_lua.sh: unoptimised bitcode -> llvm-link (whole
# program) -> opt with the pass -> runtime linked after the pass -> clang -O2.
#
# Output: sqlite/out/speedtest1_{base,chain,binsearch,trie}
#         sqlite/out/cfi_verbose_{chain,binsearch,trie}.txt
#
# Overridable from the environment:
#   B=...        directory with clang, opt, llvm-link
#   PLUGIN=...   CFIPass.so
#   SQLITE_VERSION=3.54.1
#   VARIANTS="chain binsearch trie"   (base is always built)
set -e
B=${B:-/workspace/llvm-project/build/bin}
PLUGIN=${PLUGIN:-/workspace/cfi-pass/build/lib/CFIPass.so}
VER=${SQLITE_VERSION:-3.54.1}
VARIANTS=${VARIANTS:-"chain binsearch trie"}

HERE=$(cd "$(dirname "$0")" && pwd)      # bench/
SRC=$HERE/sqlite/sqlite-src
OUT=$HERE/sqlite/out
mkdir -p "$HERE/sqlite" "$OUT/bc"

# 1. source: GitHub tag (sqlite.org downloads can fail from the container).
#    `make sqlite3.c` builds the amalgamation with SQLite's own jimsh, so
#    no tclsh is needed; it only needs a C compiler (cc) and make.
if [ ! -d "$SRC" ]; then
  GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch "version-$VER" \
    https://github.com/sqlite/sqlite "$SRC"
fi
if [ ! -f "$SRC/sqlite3.c" ]; then
  (cd "$SRC" && ./configure >/dev/null && make sqlite3.c >/dev/null)
fi

# 2. unoptimised bitcode (the pass runs before the optimiser, as for Lua)
CF="-O2 -Xclang -disable-llvm-passes -DNDEBUG -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION"
$B/clang $CF -emit-llvm -c "$SRC/sqlite3.c" -o "$OUT/bc/sqlite3.bc"
$B/clang $CF -I"$SRC" -emit-llvm -c "$SRC/test/speedtest1.c" -o "$OUT/bc/speedtest1.bc"
$B/llvm-link "$OUT/bc/sqlite3.bc" "$OUT/bc/speedtest1.bc" -o "$OUT/sqlite_all.bc"

# 3. runtime support for binsearch and trie
$B/clang -O2 -Xclang -disable-llvm-passes -emit-llvm \
  -c "$HERE/cfi_rt.c" -o "$OUT/cfi_rt.bc"
$B/clang -O2 -Xclang -disable-llvm-passes -emit-llvm \
  -c "$HERE/cfi_rt_trie.c" -o "$OUT/cfi_rt_trie.bc"

# 4. baseline: same pipeline, without the pass
$B/clang -O2 "$OUT/sqlite_all.bc" -o "$OUT/speedtest1_base" -lm -lpthread

# 5. variants with the pass
for V in $VARIANTS; do
  $B/opt -load-pass-plugin="$PLUGIN" -passes=cfi-pass -cfi-verbose -cfi-check=$V \
    "$OUT/sqlite_all.bc" -o "$OUT/sqlite_$V.pre.bc" 2> "$OUT/cfi_verbose_$V.txt"
  case $V in
    binsearch) RT=$OUT/cfi_rt.bc ;;
    trie)      RT=$OUT/cfi_rt_trie.bc ;;
    *)         RT= ;;
  esac
  if [ -n "$RT" ]; then
    # the runtime is linked after the pass, so the pass never sees it
    $B/llvm-link "$OUT/sqlite_$V.pre.bc" "$RT" -o "$OUT/sqlite_$V.bc"
  else
    cp "$OUT/sqlite_$V.pre.bc" "$OUT/sqlite_$V.bc"
  fi
  $B/clang -O2 "$OUT/sqlite_$V.bc" -o "$OUT/speedtest1_$V" -lm -lpthread
done
echo "done: $OUT"
