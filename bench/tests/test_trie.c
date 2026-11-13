// Unit test for the trie runtime (cfi_rt_trie.c): compares __cfi_check_trie with a
// binary search over the same targets, on synthetic address sets (one cluster,
// program + libc-like clusters, boundary spans, duplicates, empty table).
// Build and run:
//   gcc -O1 -g -fsanitize=address,undefined -o /tmp/test_trie tests/test_trie.c
//   ASAN_OPTIONS=detect_leaks=0 /tmp/test_trie     (the tries are never freed, by design)
#include "../cfi_rt_trie.c"
#include <string.h>
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void){ rs ^= rs<<13; rs ^= rs>>7; rs ^= rs<<17; return rs; }
static int member(const uintptr_t *s, unsigned n, uintptr_t p){ // sorted s
  unsigned lo=0, hi=n; while(lo<hi){unsigned m=lo+(hi-lo)/2; if(s[m]<p) lo=m+1; else hi=m;} return lo<n && s[lo]==p; }
static long bad = 0, checks = 0;
static void test(const char *name, uintptr_t *tab, unsigned n, int expect_nreg) {
  cfi_trie *t = __cfi_trie_build(tab, n);
  uintptr_t *s = malloc((n?n:1)*sizeof *s); memcpy(s, tab, n*sizeof *s); qsort(s,n,sizeof *s,cmp_addr);
  if (expect_nreg >= 0 && (int)t->nreg != expect_nreg) { printf("%s: nreg=%u expected %d\n", name, t->nreg, expect_nreg); bad++; }
  for (unsigned i=0;i<n;i++){ // every member must pass; neighbours must match brute force
    uintptr_t cand[5] = { s[i], s[i]+1, s[i]-1, s[i]+256, s[i]-256 };
    for (int k=0;k<5;k++){ checks++; if (__cfi_check_trie(t,cand[k]) != member(s,n,cand[k])) { bad++; if (bad<5) printf("%s: mismatch at %lx\n", name,(unsigned long)cand[k]); } }
  }
  for (int q=0;q<200000;q++){ // random addresses, some near the tables
    uintptr_t p = rnd();
    if (n && (q&1)) p = s[rnd()%n] + (rnd()%65536) - 32768;
    if (q%7==0) p = 0; if (q%11==0) p = ~(uintptr_t)0;
    checks++; if (__cfi_check_trie(t,p) != member(s,n,p)) { bad++; if (bad<5) printf("%s: random mismatch %lx\n", name,(unsigned long)p); } }
  printf("%-22s n=%-6u regions=%u ok\n", name, n, t->nreg);
}
int main(void){
  uintptr_t tab[40000]; unsigned n;
  // 1. one cluster (like the Lua case)
  n=170; for(unsigned i=0;i<n;i++) tab[i]=0x55a000000000ULL + (rnd()%400000)*4; test("one cluster", tab, n, 1);
  // 2. program text + libc cluster (the SQLite failure)
  n=100; for(unsigned i=0;i<n;i++) tab[i]=0x55a000000000ULL + (rnd()%1200000);
  for(unsigned i=0;i<20;i++) tab[n+i]=0x7f1234500000ULL + (rnd()%1500000); n+=20; test("program + libc", tab, n, 2);
  // 3. three clusters, libc bigger than the program
  n=0; for(unsigned i=0;i<10;i++) tab[n++]=0x55a000000000ULL + (rnd()%900000);
  for(unsigned i=0;i<60;i++) tab[n++]=0x7f1234500000ULL + (rnd()%1500000);
  for(unsigned i=0;i<5;i++) tab[n++]=0x7f9999900000ULL + (rnd()%100000); test("three clusters", tab, n, 3);
  // 4. exact boundary: span 2^24-1 stays one region, 2^24 splits
  tab[0]=0x1000000; tab[1]=0x1000000+CFI_MAX_RANGE-1; test("span 2^24-1", tab, 2, 1);
  tab[1]=0x1000000+CFI_MAX_RANGE; test("span 2^24", tab, 2, 2);
  // 5. tiny / duplicates
  tab[0]=0x4000; test("single target", tab, 1, 1);
  tab[0]=0x4000; tab[1]=0x4000; tab[2]=0x4000; test("duplicates", tab, 3, 1);
  // 6. empty table (the pass never emits one, but the runtime must not crash)
  test("empty", tab, 0, 1);
  // 7. many targets, dense and across clusters
  n=0; for(unsigned i=0;i<30000;i++) tab[n++]=0x55a000000000ULL + (rnd()%3000000);
  for(unsigned i=0;i<9000;i++) tab[n++]=0x7f0000000000ULL + (rnd()%9000000); test("39k targets", tab, n, 2);
  printf("checks=%ld mismatches=%ld\n", checks, bad); return bad!=0;
}
