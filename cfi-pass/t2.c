#include <stdio.h>
#include <stdlib.h>

struct ops {
  const char *name;
  int (*apply)(int, int);
};

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int sub(int a, int b) { return a - b; }

static struct ops table[] = {
  { "add", add },
  { "mul", mul },
};

static int cmp_int(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

static void run(struct ops *o, int x, int y) {
  printf("%s(%d,%d) = %d\n", o->name, x, y, o->apply(x, y));
}

int main(void) {
  for (int i = 0; i < 2; i++)
    run(&table[i], 6, 7);

  struct ops local = { "sub", sub };   // address-taken μέσα σε συνάρτηση
  run(&local, 6, 7);

  int v[] = { 5, 2, 9, 1 };
  qsort(v, 4, sizeof v[0], cmp_int);   // callback που καλείται από libc
  for (int i = 0; i < 4; i++) printf("%d ", v[i]);
  putchar('\n');
  return 0;
}
