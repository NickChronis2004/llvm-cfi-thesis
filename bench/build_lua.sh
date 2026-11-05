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

# runtime support for binsearch
$B/clang -O2 -Xclang -disable-llvm-passes -emit-llvm \
  -c /workspace/bench/cfi_rt.c -o $OUT/cfi_rt.bc

# baseline: ίδιο pipeline, χωρίς το pass
$B/clang -O2 $OUT/lua_all.bc -o $OUT/lua_base -lm

# variants with the pass
for V in chain binsearch; do
  $B/opt -load-pass-plugin=$PLUGIN -passes=cfi-pass -cfi-verbose -cfi-check=$V \
    $OUT/lua_all.bc -o $OUT/lua_$V.pre.bc 2> $OUT/cfi_verbose_$V.txt
  if [ "$V" = binsearch ]; then
    # the runtime is linked after the pass, so the pass never sees it
    $B/llvm-link $OUT/lua_$V.pre.bc $OUT/cfi_rt.bc -o $OUT/lua_$V.bc
  else
    cp $OUT/lua_$V.pre.bc $OUT/lua_$V.bc
  fi
  $B/clang -O2 $OUT/lua_$V.bc -o $OUT/lua_$V -lm
done
echo "done"
