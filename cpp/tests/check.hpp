// Minimal assertion helpers for plain-executable tests. This is NOT a test
// framework: no discovery, fixtures or reporting beyond the first-line
// message per failure. Each test binary returns nonzero if any check failed.
#pragma once

#include <cstdio>
#include <string>

namespace testing_detail {
inline int& failures() {
  static int count = 0;
  return count;
}
}  // namespace testing_detail

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,         \
                   __LINE__, #cond);                                      \
      ++testing_detail::failures();                                       \
    }                                                                     \
  } while (false)

// Like CHECK, but prints `ctx` (e.g. seed and trial number) on failure so a
// randomized failure can be reproduced. `ctx` is any std::string expression.
#define CHECK_CTX(cond, ctx)                                              \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s [%s]\n", __FILE__,    \
                   __LINE__, #cond, std::string(ctx).c_str());            \
      ++testing_detail::failures();                                       \
    }                                                                     \
  } while (false)

// Passes only if `expr` throws exactly an exception of type `ex` (or derived).
#define CHECK_THROWS_AS(expr, ex)                                         \
  do {                                                                    \
    bool threw_expected = false;                                          \
    try {                                                                 \
      (void)(expr);                                                       \
    } catch (const ex&) {                                                 \
      threw_expected = true;                                              \
    } catch (...) {                                                       \
    }                                                                     \
    CHECK(threw_expected);                                                \
  } while (false)

#define TEST_MAIN_RESULT() (testing_detail::failures() == 0 ? 0 : 1)
