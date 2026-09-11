#!/bin/bash
# Run this INSIDE the llvm-dev container, from /workspace.
# Recommended: run inside a tmux session so the build survives SSH disconnects:
#   tmux new -s llvmbuild
#   ./setup-llvm-build.sh
#   (Ctrl+B, D to detach; `tmux attach -t llvmbuild` to reattach later)

set -e

cd /workspace

# 1. Shallow clone of upstream LLVM — already done in your case, so this just skips.
if [ ! -d "llvm-project" ]; then
    echo ">>> Cloning llvm-project (shallow)..."
    git clone --depth 1 https://github.com/llvm/llvm-project.git
else
    echo ">>> llvm-project already present, skipping clone."
fi

# 2. Skeleton for your out-of-tree pass: ModulePass, RequiredPassInfoMixin design.
mkdir -p cfi-pass/lib cfi-pass/include
cd cfi-pass

if [ ! -f "CMakeLists.txt" ]; then
cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.20)
project(CFIPass)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

add_subdirectory(lib)
EOF
fi

if [ ! -f "lib/CMakeLists.txt" ]; then
cat > lib/CMakeLists.txt << 'EOF'
add_llvm_library(CFIPass MODULE
  CFIPass.cpp
  PLUGIN_TOOL
  opt
)

target_link_libraries(CFIPass PRIVATE LLVMSupport)
EOF
fi

if [ ! -f "lib/CFIPass.cpp" ]; then
cat > lib/CFIPass.cpp << 'EOF'
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
struct CFIPass : PassInfoMixin<CFIPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    errs() << "Hello World from CFIPass — module: " << M.getName() << "\n";
    return PreservedAnalyses::all();
  }
};
} // namespace

llvm::PassPluginLibraryInfo getCFIPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CFIPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "cfi-pass") {
                    MPM.addPass(CFIPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCFIPassPluginInfo();
}
EOF
fi

# git init for the pass, ready to push to GitHub whenever you want
if [ ! -d ".git" ]; then
    git init
    cat > .gitignore << 'EOF'
build/
*.o
*.so
EOF
    git add .
    git commit -m "hello-world CFI pass skeleton"
fi

cd /workspace

echo ""
echo ">>> Skeleton ready. Next: configure + build LLVM itself."
echo ">>> This is the long step — run it inside tmux."
echo ""
echo "Suggested configure command (RelWithDebInfo: keeps debug info, much lighter than full Debug):"
echo ""
cat << 'EOF'
mkdir -p llvm-project/build && cd llvm-project/build
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_ABI_BREAKING_CHECKS=FORCE_ON \
  -DLLVM_PARALLEL_LINK_JOBS=2 \
  -DLLVM_PARALLEL_COMPILE_JOBS=12 \
  -DLLVM_USE_LINKER=lld \
  -DLLVM_CCACHE_BUILD=ON

ninja opt clang
EOF
echo ""
echo ">>> This will take a while (expect 1-3+ hours depending on load). Run inside tmux."
