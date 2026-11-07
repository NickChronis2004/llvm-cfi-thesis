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
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

// ---- command-line options (file scope, registered once when the plugin loads) ----
static cl::opt<bool> Verbose("cfi-verbose",
                             cl::desc("Print analysis results"),
                             cl::init(false));

// Which runtime check to emit: 'chain' (icmp chain, no runtime support),
// 'binsearch' (sorted array, cfi_rt.c) or 'trie' (3-level radix trie,
// cfi_rt_trie.c). The measurement scripts pass -cfi-check=<variant>.
enum class CheckKind { Chain, BinSearch, Trie };
static cl::opt<CheckKind> CheckMode(
    "cfi-check", cl::desc("Runtime check structure"),
    cl::init(CheckKind::Chain),
    cl::values(clEnumValN(CheckKind::Chain, "chain", "icmp chain"),
               clEnumValN(CheckKind::BinSearch, "binsearch", "sorted array"),
               clEnumValN(CheckKind::Trie, "trie", "trie")));

// ---- statistics (printed with `opt -stats`) ----
#define DEBUG_TYPE "cfi-pass"
STATISTIC(NumAddressTaken, "Address-taken functions");
STATISTIC(NumIndirectSites, "Indirect call sites instrumented");
STATISTIC(MaxTargets, "Max targets at one call site");

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
                  SetVector<Function *> &Out) {
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
    SetVector<Function *> AddressTakenFuncs; // set of functions that are address-taken
    SmallVector<CallBase *, 16> IndirectCallSites; // set of indirect call sites (call instructions that call through a function pointer)
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
              IndirectCallSites.push_back(CB); // add the indirect call site to the set
            }
          }

          for (Use &U : I.operands()) { // for every operand of the instruction
            // if the operand is type of call instruction save the it to the CB*,
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



    NumAddressTaken += static_cast<unsigned>(AddressTakenFuncs.size());

     // Group legitimate targets by function type.
    // Sort by name first so the order is canonical: the output no longer
    // depends on pointer values or on the order functions appear in the module.
    SmallVector<Function *, 16> Sorted(AddressTakenFuncs.begin(),
                                       AddressTakenFuncs.end());
    llvm::sort(Sorted, [](Function *A, Function *B) {
      return A->getName() < B->getName();
    });

    // MapVector (instead of DenseMap) iterates in insertion order, which keeps
    // the "Targets per Type" output deterministic.
    // key: function type, value: address-taken functions with that type
    MapVector<FunctionType *, SmallVector<Function *, 8>> Targets;
    for (Function *F : Sorted)
      if (!F->isIntrinsic()) // intrinsics are not user-defined indirect targets
        Targets[F->getFunctionType()].push_back(F);

    // ---- analysis output (only with -cfi-verbose) ----
    if (Verbose) {
      // print address-taken functions (sorted by name)
      errs() << "=== CFI Pass: Address-Taken Functions ===\n";
      for (Function *F : Sorted)
        errs() << "  " << F->getName() << "\n";
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
      // (the SITE,<function>,<count> lines are meant to be parsed by scripts)
      errs() << "=== CFI Pass: Allowed Targets per Call Site ===\n";
      for (CallBase *CB : IndirectCallSites) {
        auto It = Targets.find(CB->getFunctionType());
        size_t N = (It == Targets.end()) ? 0 : It->second.size();
        errs() << "  in " << CB->getFunction()->getName() << ": " << N
               << " allowed target(s)\n";
        errs() << "SITE," << CB->getFunction()->getName() << "," << N << "\n";
      }
    }

    // ---- binsearch / trie support ----
    // Function addresses are not known at compile time (PIE/ASLR), so for
    // 'binsearch' and 'trie' we emit one writable table of targets per function
    // type and build the lookup structure at program start (binsearch: sort the
    // table, see cfi_rt.c; trie: build a trie from it, see cfi_rt_trie.c). The
    // runtime functions are only declared here; the runtime bitcode is linked
    // in afterwards.
    LLVMContext &Ctx = M.getContext();
    Type *PtrTy = PointerType::getUnqual(Ctx);
    Type *I32Ty = Type::getInt32Ty(Ctx);
    Type *IntPtrTy = M.getDataLayout().getIntPtrType(Ctx);
    MapVector<FunctionType *, GlobalVariable *> Tables; // key: function type
    // trie only: one global per function type that holds the pointer to the
    // trie built by the constructor (the pass treats the trie as opaque)
    MapVector<FunctionType *, GlobalVariable *> Handles;
    FunctionCallee CheckFn, SortFn, BuildFn;
    if (CheckMode == CheckKind::BinSearch) {
      CheckFn = M.getOrInsertFunction("__cfi_check_bs", I32Ty, PtrTy, I32Ty,
                                      IntPtrTy);
      SortFn = M.getOrInsertFunction("__cfi_sort", Type::getVoidTy(Ctx), PtrTy,
                                     I32Ty);
    } else if (CheckMode == CheckKind::Trie) {
      CheckFn = M.getOrInsertFunction("__cfi_check_trie", I32Ty, PtrTy,
                                      IntPtrTy);
      BuildFn = M.getOrInsertFunction("__cfi_trie_build", PtrTy, PtrTy, I32Ty);
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

      FunctionType *FT = CB->getFunctionType();
      auto It = Targets.find(FT); // find the legitimate targets for this call site's function type
      unsigned NumT = 0;
      if (It != Targets.end())
        NumT = static_cast<unsigned>(It->second.size());

      // statistics
      ++NumIndirectSites;
      MaxTargets.updateMax(NumT);

      // Bad = true if the callee is NOT one of the legitimate targets
      Value *Bad;
      if (CheckMode != CheckKind::Chain) {
        if (NumT == 0) {
          Bad = B.getTrue(); // no legitimate target at all: always fail
        } else {
          // create the table for this function type the first time it is needed
          GlobalVariable *&Tab = Tables[FT];
          if (!Tab) {
            SmallVector<Constant *, 8> Elts(It->second.begin(), It->second.end());
            ArrayType *AT = ArrayType::get(PtrTy, NumT);
            Tab = new GlobalVariable(M, AT, /*isConstant=*/false,
                                     GlobalValue::InternalLinkage,
                                     ConstantArray::get(AT, Elts),
                                     "__cfi_tab_" + Twine(Tables.size() - 1));
            if (CheckMode == CheckKind::Trie)
              Handles[FT] = new GlobalVariable(
                  M, PtrTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
                  ConstantPointerNull::get(cast<PointerType>(PtrTy)),
                  "__cfi_trie_" + Twine(Tables.size() - 1));
          }
          Value *R;
          if (CheckMode == CheckKind::BinSearch) {
            // r = __cfi_check_bs(table, n, (uintptr_t)callee); fail if r == 0
            R = B.CreateCall(
                CheckFn,
                {Tab, B.getInt32(NumT), B.CreatePtrToInt(Callee, IntPtrTy)});
          } else {
            // r = __cfi_check_trie(trie, (uintptr_t)callee); fail if r == 0
            Value *Trie = B.CreateLoad(PtrTy, Handles[FT]);
            R = B.CreateCall(CheckFn,
                             {Trie, B.CreatePtrToInt(Callee, IntPtrTy)});
          }
          Bad = B.CreateICmpEQ(R, B.getInt32(0));
        }
      } else {
        // ok = (callee == t1) || (callee == t2) || ...
        Value *Ok = B.getFalse(); // initialize Ok to false
        if (It != Targets.end()) // if there are legitimate targets for this function type
          for (Function *T : It->second) // for each legitimate target function
          // update Ok to be true if the callee matches any of the legitimate targets
            Ok = B.CreateOr(Ok, B.CreateICmpEQ(Callee, T));
        Bad = B.CreateNot(Ok);
      }

      // if the check failed, split the basic block and insert a trap instruction
      Instruction *Term = SplitBlockAndInsertIfThen(
          Bad, CB->getIterator(), /*Unreachable=*/true);
      IRBuilder<> TB(Term);
      // insert a trap instruction to terminate the program if the check fails
      TB.CreateIntrinsic(Intrinsic::trap, ArrayRef<Value *>{});
      Changed = true; // mark that we have modified the module
    }

    // binsearch/trie: generate a constructor that prepares every table (binsearch:
    // sort by address; trie: build the trie and store it in its handle).
    // Priority 0 = runs before the other constructors of the program.
    if (!Tables.empty()) {
      Function *Init = Function::Create(
          FunctionType::get(Type::getVoidTy(Ctx), false),
          GlobalValue::InternalLinkage, "__cfi_init", &M);
      IRBuilder<> IB(BasicBlock::Create(Ctx, "entry", Init));
      for (auto &KV : Tables) {
        GlobalVariable *Tab = KV.second;
        unsigned N = cast<ArrayType>(Tab->getValueType())->getNumElements();
        if (CheckMode == CheckKind::BinSearch) {
          IB.CreateCall(SortFn, {Tab, IB.getInt32(N)});
        } else {
          Value *T = IB.CreateCall(BuildFn, {Tab, IB.getInt32(N)});
          IB.CreateStore(T, Handles[KV.first]);
        }
      }
      IB.CreateRetVoid();
      appendToGlobalCtors(M, Init, /*Priority=*/0);
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

/*
Χαρακτηριστικό  Διαδρομή 1: opt Διαδρομή 2: clang
Είσοδος (Input) LLVM IR (.ll / .bc) | Πηγαίος κώδικας C/C++ (.c)
Έξοδος (Output) Τροποποιημένο LLVM IR (.ll) | Εκτελέσιμο binary (test_protected)
Μηχανισμός RegistrationregisterPipelineParsingCallback  | registerPipelineStartEPCallback
Πώς καλείται το Pass; Ρητά μέσω ονόματος (-passes=cfi-pass) |  Αυτόματα μέσω Hook στην αρχή του pipeline
Κύριος ΣκοπόςDebugging, inspection του IR, απομόνωση | Παραγωγή τελικού binary, automated builds
*/
