// RUN: echo "(assert (and (bvsgt #x0000000a (concat var3 var2 var1 var0)) (bvsle #x00000000 (concat var3 var2 var1 var0))))" > %T/con.txt
// RUN: %symcc -O2  %s -o %t
// RUN: env SYMCC_CONSTRAINT_FILE=%T/con.txt %t 2>&1 | %filecheck %s
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>

int foo(int a, int b) {
  if (2 * a < b){
    printf("Path1\n");
    return a;
  }
  else if (a % b){
    printf("Path2\n");
    return b;
  }
  else {
    printf("Path3\n");
    return a + b;
  }
}

void __attribute__((optnone)) symbolize_this_please(void* x, size_t size){
  return;
}

int main(int argc, char* argv[]) {

  int x = 1;
  symbolize_this_please(&x, sizeof(x));
  printf("%d\n", foo(x, 7));
  // SIMPLE: Extra constraint received
  // SIMPLE: Current path_condition
  // SIMPLE: Trying to solve
  // ANY: 1
  return 0;
}