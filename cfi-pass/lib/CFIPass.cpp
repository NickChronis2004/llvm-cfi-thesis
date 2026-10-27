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

/*
helper function to recursively collect all functions that are reachable from a given
 constant. This is used to find all address-taken functions in the module, including
  those that are referenced indirectly through global variables or other constants.

  arguements:
    C: pointer to the constant to start from (for example, an array of function pointers)
    Seen: a set of constants that have already been visited (to avoid infinite recursion)
    Out: a set of functions that are found to be address-taken
*/
void collectFuncs(Constant *C, SmallPtrSetImpl<Constant *> &Seen,
                  SmallPtrSetImpl<Function *> &Out) {
  // case: if we've already seen this constant, return early to 
  //avoid infinite recursion
  if (!Seen.insert(C).second)
    return;
  // case: if the constant is a function, add it to the output set and return
  if (auto *F = dyn_cast<Function>(C)) { // checks if C is a Function object
    Out.insert(F); // add the function to the output set
    return;
  }
  // case: otherwise, recurse on all constant operands (arrays, structs, constant expressions, global initializers)
  for (Use &U : C->operands()) // iterate over the operands of the constant
    if (auto *Op = dyn_cast<Constant>(U.get())) // check if the operand is a constant
      collectFuncs(Op, Seen, Out); // recurse on the operand
}

// the main pass class that implements the CFI pass. It inherits from 
//RequiredPassInfoMixin to provide the necessary interface for the pass manager
struct CFIPass : RequiredPassInfoMixin<CFIPass> { // RequiredPassInfoMixin is a CRTP
  // base class that provides default implementations of the pass interface methods

  // the main entry point of the pass. It takes a module and a module analysis 
  // manager as input
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    SmallPtrSet<Function *, 16> AddressTakenFuncs; // set of functions that are address-taken
    SmallPtrSet<CallBase *, 16> IndirectCallSites; // set of indirect call sites (call instructions that call through a function pointer)
    SmallPtrSet<Constant *, 32> Seen; // set of constants that have already been visited (to avoid infinite recursion)

    // collect all address-taken functions in the module, including those that are 
    //referenced indirectly through global variables or other constants
    for (GlobalVariable &GV : M.globals())
      if (GV.hasInitializer()) // check if the global variable has an initializer
        collectFuncs(GV.getInitializer(), Seen, AddressTakenFuncs);

    for (Function &F : M) {       // for every function in the module
      for (BasicBlock &BB : F) {  // for every basic block in the function
        for (Instruction &I : BB) { // for every instruction in the basic block

          // check if this instruction is an indirect call site
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (CB->isIndirectCall()) {
              IndirectCallSites.insert(CB); // add the indirect call site to the set
            }
          }

          for (Use &U : I.operands()) { // for every operand of the instruction
            // if the operand is the callee of a call instruction,
            // skip it (we only want to look at other operands)
            if (auto *CB = dyn_cast<CallBase>(&I))
              if (CB->isCallee(&U))
                continue;

            // any other operand: look for functions inside it
            // example: if the operand is a constant array of function pointers, 
            // we want to find all the functions in that array and skip the callee slot
            if (auto *C = dyn_cast<Constant>(U.get()))
              collectFuncs(C, Seen, AddressTakenFuncs);

          }
        }
      }
    }


	// group legitimate targets by function type
    DenseMap<FunctionType *, SmallVector<Function *, 8>> Targets; // map for grouping legitimate targets by function type 
    // (key: function type, value: vector of functions with that type)
    for (Function *F : AddressTakenFuncs) 
      if (!F->isIntrinsic()) // skip intrinsic functions, 
      // which are not user-defined and cannot be called indirectly
        Targets[F->getFunctionType()].push_back(F); // add the function to the vector of functions with the same type

        // print address-taken functions
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

    // print legitimate targets grouped by function type
    errs() << "=== CFI Pass: Targets per Type ===\n";
    for (auto &[FT, Funcs] : Targets) {
      errs() << "  type: ";
      FT->print(errs());
      errs() << "\n";
      for (Function *F : Funcs)
        errs() << "    " << F->getName() << "\n";
    }

    // print allowed targets per indirect call site
    errs() << "=== CFI Pass: Allowed Targets per Call Site ===\n";
    for (CallBase *CB : IndirectCallSites) {
      auto It = Targets.find(CB->getFunctionType());
      errs() << "  in " << CB->getFunction()->getName() << ": "
             << (It == Targets.end() ? 0 : It->second.size())
             << " allowed target(s)\n";
    }

        /* for each indirect call site, insert a runtime check that 
         verifies that the callee is one of the legitimate targets for 
        that call site. If the check fails, trap the program.
        */
    bool Changed = false;
    for (CallBase *CB : IndirectCallSites) {
      IRBuilder<> B(CB); // create an IRBuilder at the location of the indirect call site
      // get the callee operand of the call instruction (the function pointer being called)
      Value *Callee = CB->getCalledOperand(); 

      // ok = (callee == t1) || (callee == t2) || ...
      Value *Ok = B.getFalse(); // initialize Ok to false
      auto It = Targets.find(CB->getFunctionType()); // find the legitimate targets for this call site's function type
      if (It != Targets.end()) // if there are legitimate targets for this function type
        for (Function *T : It->second) // for each legitimate target function
        // update Ok to be true if the callee matches any of the legitimate targets
          Ok = B.CreateOr(Ok, B.CreateICmpEQ(Callee, T));

      // if Ok is false, split the basic block and insert a trap instruction
      Value *Bad = B.CreateNot(Ok); 
      Instruction *Term = SplitBlockAndInsertIfThen(
          Bad, CB->getIterator(), /*Unreachable=*/true);
      IRBuilder<> TB(Term);
      // insert a trap instruction to terminate the program if the check fails
      TB.CreateIntrinsic(Intrinsic::trap, ArrayRef<Value *>{});
      Changed = true; // mark that we have modified the module
    }
    // return the appropriate PreservedAnalyses value based on whether we modified the module
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
            // for `opt -passes=cfi-pass`
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "cfi-pass") {
                    MPM.addPass(CFIPass());
                    return true;
                  }
                  return false;
                });

            // for `clang -fpass-plugin=...`: runs at the start of the pipeline
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
