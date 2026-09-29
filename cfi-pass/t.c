#include <stdio.h>
#include <stdlib.h>

void good(void)  { puts("good"); }
int  evil(int x) { puts("EVIL REACHED"); return x; }

void (*table[])(void) = { good };

int main(int argc, char **argv) {
  void (*fp)(void) = table[0];
  if (argc > 1)
    fp = (void (*)(void))evil;   // simulated corrupted pointer
  fp();
  return 0;
}
