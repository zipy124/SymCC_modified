
// RUN: %symcc -O2  %s -o %t
// RUN: env SYMCC_ENABLE_ARGS=ON %t 2 2>&1 | %filecheck %s
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>

int foo(int a, int b) {
  if (a == 1)
    return a;
  else if (a == 2)
    return b;
  else
    return 0;
}

// isNumericChar and myAtoi are sourced from
// https://www.geeksforgeeks.org/write-your-own-atoi/
// and are not my own work

int isNumericChar(char x)
{
  return (x >= '0' && x <= '9') ? 1 : 0;
}

int myAtoi(char* str)
{
  if (*str == '\0')
    return 0;
  // Initialize result
  int res = 0;

  // Initialize sign as positive
  int sign = 1;
  // Initialize index of first digit
  int i = 0;

  // If number is negative,
  // then update sign
  if (str[0] == '-')
  {
    sign = -1;

    // Also update index of first digit
    i++;
  }

  // Iterate through all digits of
  // input string and update result
  for (; str[i] != '\0'; ++i)
  {

    // You may add some lines
    // to write error message
    if (isNumericChar(str[i]) == 0)
      return 0;

    // to error stream
    res = res * 10 + str[i] - '0';
  }

  // Return result with sign
  return sign * res;
}


int main(int argc, char* argv[]) {
  if (argc <= 1) {
    printf("Error no arguments supplied");
    exit(1);
  }

  int x = myAtoi(argv[1]);
  printf("%d\n", foo(x, 7));
  // SIMPLE: Current path_condition
  // SIMPLE: Trying to solve
  // ANY: 7
  return 0;
}

