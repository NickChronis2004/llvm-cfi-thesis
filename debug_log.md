# Bug Log — CFI Pass Remote Infra

Μορφή κάθε entry: **Symptom → Root Cause → Fix → Μάθημα**

---

## Bug #1 — OOM kill κατά το compile στάδιο

**Ημερομηνία:** 11/9/2026

**Symptom:**

c++: fatal error: Killed signal terminated program cc1plus
compilation terminated.
ninja: build stopped: subcommand failed.

Εμφανίστηκε στο ~43% του build (2045/4724 targets), κατά το compile του `AArch64Subtarget.cpp`.

**Root Cause:**
Το ninja, χωρίς ρητό `-j` flag, τρέχει by default τόσα παράλληλα jobs όσα τα CPU cores (16 στον server). Ορισμένα LLVM .cpp αρχεία (ειδικά codegen backends όπως AArch64/AMDGPU) είναι πολύ "βαριά" σε compile-time RAM χρήση (2-3GB+ το καθένα λόγω heavy template instantiation). Όταν πολλά τέτοια αρχεία compile-άρονται ταυτόχρονα, η αθροιστική RAM χρήση ξεπέρασε τη διαθέσιμη RAM + swap του server (~15GB RAM + 4GB swap), και το Linux OOM killer σκότωσε τη διεργασία.

Σημείωση: το `-DLLVM_PARALLEL_COMPILE_JOBS=12` στο CMake configure **δεν αρκεί** από μόνο του να περιορίσει αυτό — χρειάζεται ρητό `-j` flag στο ίδιο το ninja invocation.

**Fix:**
```bash
ninja -j6 opt clang
```
Περιορισμός σε 6 παράλληλα jobs αντί για default 16. Το ninja συνέχισε από εκεί που είχε μείνει (δεν ξαναέχτισε ήδη-ολοκληρωμένα .o αρχεία).

**Μάθημα:**
Σε μηχανήματα με ~16GB RAM, μην εμπιστεύεσαι το default ninja parallelism για builds σαν το LLVM. Πάντα ρητό `-j` χαμηλότερο από τον αριθμό cores όταν η RAM είναι το bottleneck, όχι το CPU.

---

## Bug #2 — Linker relocation overflow (debug info)

**Ημερομηνία:** 11/9/2026

**Symptom:**

ld.lld: error: lib/libLLVMDebugInfoPDB.a(RawError.cpp.o):(.debug_loclists+0x23f):
relocation R_X86_64_32 out of range: 4407004735 is not in [0, 4294967295]

referenced by RawError.cpp; consider recompiling with -fdebug-types-section
to reduce size of debug sections

Πολλαπλά τέτοια errors σε διαφορετικά `.a` αρχεία (`libLLVMDebugInfoPDB`, `libLLVMTextAPI`, `libLLVMObject`, `libLLVMFrontendOpenMP`), όλα στο `.debug_loclists` section, στο **link** στάδιο (όχι compile — το compile πέρασε καθαρά).

**Root Cause:**
Build type ήταν `RelWithDebInfo`, το οποίο παράγει πλήρες DWARF debug info για κάθε συνάρτηση/μεταβλητή/τύπο σε όλο το LLVM codebase. Όταν όλα τα ~30+ static libraries του LLVM linkάρονται μαζί σε ένα executable (`opt`/`clang`), το συνολικό `.debug_loclists` section (μέρος DWARF format, κρατάει πληροφορίες θέσης μεταβλητών) ξεπέρασε το 4GB (4294967295 bytes), που είναι το απόλυτο μέγιστο που μπορεί να αναπαραστήσει ένα 32-bit relocation (`R_X86_64_32`).

Το LLVM είναι ιδιαίτερα επιρρεπές σε αυτό λόγω βαριάς χρήσης C++ templates (π.χ. `RegionBase<RegionTraits<Function>>`) — κάθε template instantiation παράγει ξεχωριστά debug symbols, πολλαπλασιάζοντας δραστικά το μέγεθος του debug info section.

**Fix:**
Αλλαγή από `CMAKE_BUILD_TYPE=RelWithDebInfo` σε `CMAKE_BUILD_TYPE=Release` (καθόλου debug info) και clean rebuild (νέο build directory).

**Εναλλακτική λύση που εξετάστηκε αλλά δεν εφαρμόστηκε:**
Split DWARF (`-gsplit-dwarf` compile flag) — θα κρατούσε πλήρες debug info αλλά σε ξεχωριστά per-object `.dwo` αρχεία αντί να συσσωρεύεται όλο στο τελικό linked binary. Πιο πολύπλοκο configure/workflow, απορρίφθηκε προς όφελος του απλού Release build.

**Trade-off που έγινε αποδεκτό:**
Χωρίς debug info στο ίδιο το LLVM core, δεν είναι δυνατό step-through debugging μέσα στον πηγαίο κώδικα του LLVM (π.χ. μέσα στο PassBuilder). Το debugging του ίδιου του pass (`CFIPass.cpp`) παραμένει πλήρως διαθέσιμο, μιας και είναι ξεχωριστό compilation unit εκτός LLVM core, άσχετο με αυτό το πρόβλημα.

**Μάθημα:**
`RelWithDebInfo` σε project του μεγέθους/πολυπλοκότητας του LLVM (πολλά static libs + βαριά templates) μπορεί να χτυπήσει 32-bit relocation limits κατά το link. Για out-of-tree pass development που δεν απαιτεί debugging μέσα στον ίδιο τον LLVM core, καθαρό `Release` build είναι το πρακτικό default.

---

## Bug #3 — `PassPlugin.h` μετακινήθηκε (`llvm/Passes/` → `llvm/Plugins/`)

**Ημερομηνία:** 11/9/2026

**Symptom:**

fatal error: llvm/Passes/PassPlugin.h: No such file or directory

Κατά το compile του `CFIPass.cpp`, αμέσως μετά την επιτυχή ολοκλήρωση του LLVM build.

**Root Cause:**
Το `git clone --depth 1` τράβηξε το τρέχον HEAD του upstream `llvm/llvm-project` (μηχανή main/master, όχι κάποιο σταθερό release tag). Πολύ πρόσφατο upstream commit ("[LLVM][NFC] Move PassPlugin from Passes to separate library") μετακίνησε το header από `llvm/Passes/PassPlugin.h` σε `llvm/Plugins/PassPlugin.h`, ώστε να μην τραβάει μέσα ολόκληρη τη Passes βιβλιοθήκη σαν dependency όταν χρειάζεται μόνο το PassPlugin. Τα περισσότερα online tutorials/παραδείγματα (και ο αρχικός σκελετός που γράφτηκε) βασίζονται στο παλιό, πιο σταθερό path.

**Fix:**
```cpp
// πριν
#include "llvm/Passes/PassPlugin.h"
// μετά
#include "llvm/Plugins/PassPlugin.h"
```
Επιβεβαιώθηκε το σωστό path με:
```bash
find /workspace/llvm-project/llvm/include -iname "PassPlugin.h"
```

**Μάθημα:**
Το `git clone --depth 1` χωρίς συγκεκριμένο tag/branch τραβάει το πιο πρόσφατο, ασταθές HEAD — εκτεθειμένο σε πολύ πρόσφατα, ίσως μη τεκμηριωμένα ακόμα, breaking API changes. Για out-of-tree pass development πάνω σε γνωστό, σταθερό API, προτιμότερο να γίνει clone συγκεκριμένου release tag (π.χ. `--branch llvmorg-19.1.0`) αντί για `main`.

---

## Bug #4 — `PassInfoMixin` μετακινήθηκε σε `detail` namespace

**Ημερομηνία:** 11/9/2026

**Symptom:**

error: expected template-name before '<' token
struct CFIPass : PassInfoMixin<CFIPass> {

Παρόλο που το `#include "llvm/IR/PassManager.h"` προστέθηκε σωστά (επιβεβαιώθηκε με grep ότι το header περιλαμβανόταν).

**Root Cause:**
Σε αυτή τη rolling-HEAD έκδοση του LLVM, το `PassInfoMixin` δεν είναι πλέον top-level `llvm::PassInfoMixin` — είναι `llvm::detail::PassInfoMixin`, εσωτερικό implementation detail. Το δημόσιο API προς χρήση από out-of-tree passes είναι πλέον `RequiredPassInfoMixin` (για passes που πρέπει πάντα να τρέχουν όταν ζητηθούν ρητά) ή `OptionalPassInfoMixin` (για passes που το pass manager μπορεί να παραλείψει σιωπηλά) — και τα δύο κληρονομούν εσωτερικά από `detail::PassInfoMixin`. Το ίδιο το header file το ανέφερε ρητά σε σχόλιο: "Actual passes should inherit from RequiredPassInfoMixin or OptionalPassInfoMixin."

**Fix:**
```cpp
// πριν
struct CFIPass : PassInfoMixin<CFIPass> {
// μετά
struct CFIPass : RequiredPassInfoMixin<CFIPass> {
```
Επιβεβαιώθηκε η ύπαρξη/σημασία των δύο mixins με:
```bash
grep -n "RequiredPassInfoMixin\|OptionalPassInfoMixin" llvm/include/llvm/IR/PassManager.h
```

**Μάθημα:**
Ίδιο μοτίβο με το Bug #3 — pass-writing API refactoring στο upstream HEAD. Και τα δύο bugs (#3, #4) υποδεικνύουν ότι το "κλασικό" LLVM out-of-tree pass tutorial API (`PassInfoMixin` top-level, `llvm/Passes/PassPlugin.h`) έχει αλλάξει πρόσφατα προς πιο compartmentalized δομή. Αξίζει να ελέγχεται πάντα το include path/namespace απευθείας μέσα στο πηγαίο LLVM αντί να εμπιστεύεσαι παλιά tutorials, όταν δουλεύεις πάνω σε πρόσφατο HEAD clone.

---

**Επαλήθευση end-to-end (μετά τα fixes):**
```bash
cd /workspace/cfi-pass/build && ninja
# [2/2] Linking CXX shared module lib/CFIPass.so

/workspace/llvm-project/build/bin/opt -load-pass-plugin=./build/lib/CFIPass.so \
  -passes=cfi-pass -disable-output test.ll
# Hello World from CFIPass — module: test.ll
```

<!-- Νέα bugs προστίθενται εδώ, ίδια μορφή -->
