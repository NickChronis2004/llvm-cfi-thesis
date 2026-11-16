#!/bin/bash
# Build the reference points that use Clang's OWN CFI (-fsanitize=cfi-icall):
#
#   ltobase   same flags as clangcfi, but WITHOUT the CFI check
#             (isolates the cost of -flto -fvisibility=hidden)
#   clangcfi  -flto -fvisibility=hidden -fsanitize=cfi-icall
#             (jump tables + range check, see LowerTypeTests)
#
# Usage:  ./build_clangcfi.sh lua|sqlite [--ll-only]
# Needs the sources that build_lua.sh / build_sqlite.sh already fetched.
# Output: out/lua_{ltobase,clangcfi}   or   sqlite/out/speedtest1_{ltobase,clangcfi}
#
# Overridable from the environment:
#   B=...         directory with clang (and ideally ld.lld)
#   CFI_FLAGS=... default: trap mode (no UBSan runtime needed, like our pass)
#                 e.g. add -fsanitize-cfi-icall-generalize-pointers
#   LUA_SRC=... SQLITE_SRC=...   source directories
#
# Notes:
#  * CFI needs LTO, so we need ld.lld. The linker must be at least as new as
#    the clang that wrote the bitcode; the one from the LLVM build tree is
#    the safe choice (cmake -DLLVM_ENABLE_PROJECTS="clang;lld").
#  * -fno-sanitize-ignorelist: the default cfi_ignorelist.txt only exists if
#    compiler-rt was built/installed; we do not need it for plain C.
set -e
PROG=${1:?usage: build_clangcfi.sh lua|sqlite}
B=${B:-/workspace/llvm-project/build/bin}
HERE=$(cd "$(dirname "$0")" && pwd)
CFI_FLAGS=${CFI_FLAGS:-"-fsanitize=cfi-icall -fsanitize-trap=cfi-icall -fno-sanitize-ignorelist"}

LLD=()
if [ -x "$B/ld.lld" ]; then LLD=(--ld-path="$B/ld.lld")
elif command -v ld.lld >/dev/null; then
  echo "WARNING: using ld.lld from PATH ($(ld.lld --version | head -1))," \
       "it may be older than $B/clang -> LTO 'unknown attribute' errors" >&2
else
  echo "ERROR: no ld.lld. Build LLVM with lld, or: apt install lld" >&2; exit 1
fi
COMMON="-O2 -flto -fvisibility=hidden"

case $PROG in
  lua)
    SRC=${LUA_SRC:-$HERE/lua-5.4.7}
    OUT=$HERE/out
    PREFIX=lua
    DEFS="-DLUA_USE_POSIX"
    LIBS="-lm"
    FILES=$(cd "$SRC" && ls *.c | grep -v -e '^luac\.c$' -e '^onelua\.c$')
    INC="-I$SRC"
    ;;
  sqlite)
    SRC=${SQLITE_SRC:-$HERE/sqlite/sqlite-src}
    OUT=$HERE/sqlite/out
    PREFIX=speedtest1
    DEFS="-DNDEBUG -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION"
    LIBS="-lm -lpthread"
    FILES="sqlite3.c test/speedtest1.c"
    INC="-I$SRC"
    ;;
  *) echo "unknown program $PROG (lua|sqlite)"; exit 1 ;;
esac
[ -d "$SRC" ] || { echo "no sources in $SRC - run build_$PROG.sh first"; exit 1; }
mkdir -p "$OUT"

build() {   # build <variant> <extra flags>
  local V=$1; shift
  local D=$OUT/obj_$V
  rm -rf "$D"; mkdir -p "$D"
  local objs=()
  for f in $FILES; do
    local o=$D/$(basename "${f%.c}").o
    "$B/clang" $COMMON $DEFS $INC "$@" -c "$SRC/$f" -o "$o"
    objs+=("$o")
  done
  "$B/clang" $COMMON "$@" "${LLD[@]}" -fuse-ld=lld "${objs[@]}" \
    -o "$OUT/${PREFIX}_$V" $LIBS
  echo "built $OUT/${PREFIX}_$V"
}

# Unoptimised IR of the CFI build: one llvm.type.test per indirect call, so
# clang_precision.py can tell how many targets Clang allows at every site.
emit_ll() {
  local D=$OUT/ll_clangcfi
  rm -rf "$D"; mkdir -p "$D"
  for f in $FILES; do
    "$B/clang" -O2 -Xclang -disable-llvm-passes -flto -fvisibility=hidden \
      $DEFS $INC $CFI_FLAGS -S -emit-llvm "$SRC/$f" \
      -o "$D/$(basename "${f%.c}").ll"
  done
  echo "wrote $D"
}
if [ "$2" = "--ll-only" ]; then emit_ll; exit 0; fi   # reuse the existing binaries

build ltobase
build clangcfi $CFI_FLAGS
emit_ll

# statistics: how many jump tables (= type equivalence classes) did LLVM make?
# (symbols are kept in the unstripped binary; informational only)
{
  echo "clangcfi flags: $CFI_FLAGS"
  echo "jump-table type ids (nm | __typeid_*_global_addr): " \
       "$(nm "$OUT/${PREFIX}_clangcfi" | grep -c '__typeid_.*_global_addr' || true)"
} | tee "$OUT/cfi_verbose_clangcfi.txt"
