#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void good(void)           { puts("good"); }
void bad_same_type(void)  { puts("SAME TYPE REACHED"); }
int  evil(int x)          { puts("EVIL REACHED"); return x; }

void (*table[])(void)  = { good };
void (*table2[])(void) = { bad_same_type };   // address-taken, ίδιο type

int main(int argc, char **argv) {
  void (*fp)(void) = table[0];
  const char *mode = argc > 1 ? argv[1] : "";

  if (!strcmp(mode, "type"))         // λάθος type
    fp = (void (*)(void))evil;
  else if (!strcmp(mode, "mid"))     // μέση συνάρτησης (gadget-like)
    fp = (void (*)(void))((char *)good + 4);
  else if (!strcmp(mode, "same"))    // ίδιο type, address-taken
    fp = table2[0];

  fp();
  return 0;
}
