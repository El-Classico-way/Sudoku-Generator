// Verifies the toolchain is actually compiling in standard C++20 mode.
#include <compare>
#include <cstdio>

int main() {
#if __cplusplus < 202002L
  std::puts("FAIL: __cplusplus < 202002L");
  return 1;
#else
  // <=> is C++20-only; this fails to compile if the standard is wrong.
  const auto order = (1 <=> 2);
  if (!(order < 0)) {
    std::puts("FAIL: three-way comparison gave unexpected result");
    return 1;
  }
#ifdef __GNUC__
#ifndef __STRICT_ANSI__
  std::puts("FAIL: compiler extensions appear enabled");
  return 1;
#endif
#endif
  return 0;
#endif
}
