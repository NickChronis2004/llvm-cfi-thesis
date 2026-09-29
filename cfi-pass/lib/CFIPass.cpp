#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

// Walks a constant recursively (initializers, ConstantExprs, aggregates)
// and collects every Function it references. Needed because a function
// whose address is stored in a global table never shows up as a direct
// instruction operand.
void collectFuncs(Constant *C, SmallPtrSetImpl<Constant *> &Seen,
                  SmallPtrSetImpl<Function *> &Out) {
  if (!Seen.insert(C).second)
    return;
  if (auto *F = dyn_cast<Function>(C)) {
    Out.insert(F);
    return;
  }
  for (Use &U : C->operands())
    if (auto *Op = dyn_cast<Constant>(U.get()))
      collectFuncs(Op, Seen, Out);
}

struct CFIPass : RequiredPassInfoMixin<CFIPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    SmallPtrSet<Function *, 16> AddressTakenFuncs;
    SmallPtrSet<CallBase *, 16> IndirectCallSites; // sets
    SmallPtrSet<Constant *, 32> Seen;

    // scan global initializers (function pointer tables, structs, etc.)
    for (GlobalVariable &GV : M.globals())
      if (GV.hasInitializer())
        collectFuncs(GV.getInitializer(), Seen, AddressTakenFuncs);

    for (Function &F : M) {       // for every function in the module
      for (BasicBlock &BB : F) {  // for every block in each function
        for (Instruction &I : BB) { // for each instruction inside basic block

          // check if this instruction is an indirect call site
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (CB->isIndirectCall()) {
              IndirectCallSites.insert(CB);
            }
          }

          for (Use &U : I.operands()) { // check the operands (arguments)
            // the callee slot of a call is not an address-taken use
            if (auto *CB = dyn_cast<CallBase>(&I))
              if (CB->isCallee(&U))
                continue;

            // any other operand: look for functions inside it
            if (auto *C = dyn_cast<Constant>(U.get()))
              collectFuncs(C, Seen, AddressTakenFuncs);
          }
        }
      }
    }


	// group legitimate targets by function type
    DenseMap<FunctionType *, SmallVector<Function *, 8>> Targets;
    for (Function *F : AddressTakenFuncs)
      if (!F->isIntrinsic())
        Targets[F->getFunctionType()].push_back(F);

    errs() << "=== CFI Pass: Address-Taken Functions ===\n";
    for (Function *F : AddressTakenFuncs) {
      errs() << "  " << F->getName() << "\n";
    }
    errs() << "Total: " << AddressTakenFuncs.size() << "\n";

    // print indirect call sites
    errs() << "=== CFI Pass: Indirect Call Sites ===\n";
    for (CallBase *CB : IndirectCallSites) {
      errs() << "  in function " << CB->getFunction()->getName() << "\n";
    }
    errs() << "Total: " << IndirectCallSites.size() << "\n";

    errs() << "=== CFI Pass: Targets per Type ===\n";
    for (auto &[FT, Funcs] : Targets) {
      errs() << "  type: ";
      FT->print(errs());
      errs() << "\n";
      for (Function *F : Funcs)
        errs() << "    " << F->getName() << "\n";
    }

    errs() << "=== CFI Pass: Allowed Targets per Call Site ===\n";
    for (CallBase *CB : IndirectCallSites) {
      auto It = Targets.find(CB->getFunctionType());
      errs() << "  in " << CB->getFunction()->getName() << ": "
             << (It == Targets.end() ? 0 : It->second.size())
             << " allowed target(s)\n";
    }

        // === Instrumentation ===
    bool Changed = false;
    for (CallBase *CB : IndirectCallSites) {
      IRBuilder<> B(CB);
      Value *Callee = CB->getCalledOperand();

      // ok = (callee == t1) || (callee == t2) || ...
      Value *Ok = B.getFalse();
      auto It = Targets.find(CB->getFunctionType());
      if (It != Targets.end())
        for (Function *T : It->second)
          Ok = B.CreateOr(Ok, B.CreateICmpEQ(Callee, T));

      Value *Bad = B.CreateNot(Ok);
      Instruction *Term = SplitBlockAndInsertIfThen(
          Bad, CB->getIterator(), /*Unreachable=*/true);
      IRBuilder<> TB(Term);
      TB.CreateIntrinsic(Intrinsic::trap, ArrayRef<Value *>{});
      Changed = true;
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // namespace

// Plugin registration boilerplate: tells LLVM/opt that this shared
// library provides a pass named "cfi-pass". When someone runs
// `opt -passes=cfi-pass`, the callback below recognizes the name
// and adds an instance of CFIPass to the module pass pipeline.
llvm::PassPluginLibraryInfo getCFIPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CFIPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // για `opt -passes=cfi-pass`
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "cfi-pass") {
                    MPM.addPass(CFIPass());
                    return true;
                  }
                  return false;
                });

            // για `clang -fpass-plugin=...`: τρέχει στην αρχή του pipeline
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(CFIPass());
                });
          }};
}
// Entry point that `opt` looks for when dynamically loading this
// shared library via -load-pass-plugin. extern "C" disables C++ name
// mangling so the loader can find this exact symbol name.
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getCFIPassPluginInfo();
}
