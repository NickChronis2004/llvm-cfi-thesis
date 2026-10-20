#include <stdio.h>
#include <stdlib.h>

void cb_ptr(int *p)      { printf("cb_ptr: %d\n", *p); }
void cb_int(int x)       { printf("cb_int: %d\n", x); }
int  inc_s(int x)        { return x + 1; }
unsigned inc_u(unsigned x) { return x + 1; }

int main(int argc, char **argv) {
    int which = argc > 1 ? atoi(argv[1]) : 0;
    int v = 7;

    switch (which) {
    case 0: { void (*f)(int) = cb_int; f(v); break; }
    case 1: { void (*f)(void *) = (void (*)(void *))cb_ptr; f(&v); break; }
    case 2: { int (*f)(int) = (int (*)(int))inc_u; printf("%d\n", f(v)); break; }
    case 3: { void (*f)(void *) = (void (*)(void *))cb_int; f(&v); break; }
    case 4: { void (*f)(int) = (void (*)(int))inc_s; f(v); break; }
    }
    return 0;
}
