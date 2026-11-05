#!/bin/bash
set -e
B=/workspace/llvm-project/build/bin
PLUGIN=/workspace/cfi-pass/build/lib/CFIPass.so
SRC=/workspace/bench/lua-5.4.7
OUT=/workspace/bench/out
mkdir -p $OUT/bc
cd $SRC
for f in *.c; do
  case $f in luac.c|onelua.c) continue;; esac
  $B/clang -O2 -Xclang -disable-llvm-passes -DLUA_USE_POSIX \
    -emit-llvm -c $f -o $OUT/bc/${f%.c}.bc
done
$B/llvm-link $OUT/bc/*.bc -o $OUT/lua_all.bc

# baseline: ίδιο pipeline, χωρίς το pass
$B/clang -O2 $OUT/lua_all.bc -o $OUT/lua_base -lm

# με το pass (chain)
$B/opt -load-pass-plugin=$PLUGIN -passes=cfi-pass -cfi-verbose \
  $OUT/lua_all.bc -o $OUT/lua_chain.bc 2> $OUT/cfi_verbose.txt
$B/clang -O2 $OUT/lua_chain.bc -o $OUT/lua_chain -lm
echo "done"
