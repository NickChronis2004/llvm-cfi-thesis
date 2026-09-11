#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
struct CFIPass : RequiredPassInfoMixin<CFIPass> {
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
