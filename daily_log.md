# Daily Log — CFI Pass Remote Infra Setup

## 11 Σεπτεμβρίου 2026

**Στόχος της μέρας:** Μεταφορά του development environment για το LLVM CFI pass από το τοπικό Kali PC σε remote server (JARVIS home server), ώστε να υπάρχει πρόσβαση απομακρυσμένα και δυνατότητα για attacker-outside/victim-inside ROP/JOP testing.

**Τι αποφασίστηκε αρχιτεκτονικά:**
- Δύο ξεχωριστά Docker containers αντί για ένα:
  - `llvm-dev` — βαρύ container, όλο το LLVM build toolchain, εκεί γίνεται το development του pass
  - `victim-lab` — ελαφρύ, disposable container, μόνο runtime deps, τρέχει το compiled victim binary σαν network service
- Λόγος διαχωρισμού: πιο ρεαλιστικό exploitation setup (attack έρχεται απ' έξω μέσω δικτύου, όχι local-only demo) + το victim-lab μπορεί να κρασάρει/ξαναφτιάχνεται ελεύθερα χωρίς να πειράζει το βαρύ dev environment
- Δικτύωση: Tailscale-only port binding (όχι δημόσια έκθεση), ίδιο security model με το υπόλοιπο JARVIS setup

**Hardware baseline του server:**
- 16 CPU cores, ~15GB RAM, 132GB free disk (πριν το build)

**Βήματα που έγιναν:**
1. `git clone --depth 1 https://github.com/llvm/llvm-project.git` απευθείας στο host, στο `~/llvm-cfi/llvm-project`
2. Γράφτηκαν `Dockerfile.llvm-dev`, `Dockerfile.victim-lab`, `docker-compose.yml` — bind mount του `~/llvm-cfi` στο `/workspace` μέσα στο `llvm-dev` container (αντί για named Docker volume, μιας και το clone υπήρχε ήδη στο host)
3. `docker compose build` — build και των δύο images (~164s)
4. `docker compose up -d llvm-dev` — εκκίνηση του dev container
5. Setup script (`setup-llvm-build.sh`) έφτιαξε skeleton για το pass (`cfi-pass/lib/CFIPass.cpp`, hello-world ModulePass) + `git init` για το pass repo
6. CMake configure (πρώτη προσπάθεια, `RelWithDebInfo`) — πέτυχε καθαρά
7. `ninja opt clang` — **απέτυχε** στο compile στάδιο (OOM, βλ. bug log)
8. Retry με `ninja -j6 opt clang` — προχώρησε πολύ παραπέρα, **απέτυχε** στο link στάδιο (relocation overflow, βλ. bug log)
9. Clean rebuild με `CMAKE_BUILD_TYPE=Release` αντί για `RelWithDebInfo` — σε εξέλιξη στο τέλος της μέρας

**Κατάσταση στο τέλος της μέρας:** Το Release build ολοκληρώθηκε επιτυχώς (4724/4724 targets). Επιβεβαιώθηκε με `./bin/opt --version`:
```
LLVM version 24.0.0git
Optimized build with assertions.
Default target: x86_64-unknown-linux-gnu
```

**Επόμενα βήματα:**
- ✅ Χτίσιμο του `cfi-pass` πάνω στο LLVM build — πέτυχε μετά από 2 API-related fixes (βλ. bug log #3, #4)
- ✅ Επιβεβαιώθηκε ότι το hello-world pass φορτώνει και τρέχει σωστά μέσα στο `opt` πάνω σε test IR
- ✅ Ενοποιημένο git repo φτιάχτηκε στο `/workspace` (= `~/llvm-cfi` στο host): Dockerfiles, compose, setup script, cfi-pass — `llvm-project/` ρητά αγνοημένο μέσω `.gitignore`
- ✅ Repo ανέβηκε σε private GitHub repo (`NickChronis2004/llvm-cfi-thesis`) — νέο SSH key δημιουργήθηκε μέσα στο `llvm-dev` container ειδικά για αυτό (ξεχωριστό από το προσωπικό key)
- Στήσιμο του victim-lab container με πρώτο test binary
- Αρχή πραγματικής CFI λογικής μέσα στο pass (αυτό ήταν μέχρι τώρα μόνο hello-world skeleton)

**Reference — ακριβές LLVM commit που χρησιμοποιείται (Επιλογή Γ: παραμονή σε HEAD, με καταγεγραμμένο commit αντί για μετάβαση σε stable release tag):**
```
commit: c9e8899e31dba827a9c738252de1762f3b6fd3df
date:   2026-09-11 09:07:25 +0800
```
Αν χρειαστεί ποτέ αναπαραγωγή του ίδιου setup ή σύγκριση με μελλοντικό LLVM update, αυτό είναι το ακριβές σημείο εκκίνησης.
