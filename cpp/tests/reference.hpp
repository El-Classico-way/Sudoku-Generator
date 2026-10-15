// Deliberately naive, independent reference implementations used to
// cross-check the production code. They share no code with src/: no bit
// masks, no MRV, no Board validators. Clarity over speed.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ref {

using Grid = std::array<int, 81>;  // row-major, 0 = empty

// Small deterministic generator (SplitMix64) so test inputs never depend on
// the library under test or on <random> implementation details.
struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

inline bool sees(int a, int b) {  // distinct cells sharing a unit
  if (a == b) return false;
  const int ra = a / 9, ca = a % 9, rb = b / 9, cb = b % 9;
  return ra == rb || ca == cb || (ra / 3 == rb / 3 && ca / 3 == cb / 3);
}

// Pairwise definition of "no repeated nonzero digit in any unit".
inline bool consistent(const Grid& g) {
  for (int a = 0; a < 81; ++a)
    for (int b = a + 1; b < 81; ++b)
      if (g[a] != 0 && g[a] == g[b] && sees(a, b)) return false;
  return true;
}

inline bool unit_has_pair(const Grid& g, int kind, int u) {  // 0 row 1 col 2 box
  auto in_unit = [&](int i) {
    if (kind == 0) return i / 9 == u;
    if (kind == 1) return i % 9 == u;
    return (i / 9 / 3) * 3 + (i % 9) / 3 == u;
  };
  for (int a = 0; a < 81; ++a)
    for (int b = a + 1; b < 81; ++b)
      if (in_unit(a) && in_unit(b) && g[a] != 0 && g[a] == g[b]) return true;
  return false;
}

inline bool placeable(const Grid& g, int cell, int d) {
  for (int o = 0; o < 81; ++o)
    if (g[o] == d && sees(cell, o)) return false;
  return true;
}

// Counts completions, stopping at `cap`. Fills the first empty cell in
// row-major order with 1..9. Returns nullopt if more than `node_cap`
// recursive calls were needed (too slow for the naive method; callers skip).
struct Counter {
  std::uint64_t cap, node_cap, nodes = 0, count = 0;
  bool aborted = false;
  void go(Grid& g) {
    if (count >= cap || aborted) return;
    if (++nodes > node_cap) { aborted = true; return; }
    int cell = 0;
    while (cell < 81 && g[cell] != 0) ++cell;
    if (cell == 81) { ++count; return; }
    for (int d = 1; d <= 9; ++d) {
      if (!placeable(g, cell, d)) continue;
      g[cell] = d;
      go(g);
      g[cell] = 0;
    }
  }
};

inline std::optional<std::uint64_t> count(Grid g, std::uint64_t cap,
                                          std::uint64_t node_cap = 20000) {
  Counter c{cap, node_cap};
  c.go(g);
  if (c.aborted) return std::nullopt;
  return c.count;
}

inline std::string to_string(const Grid& g) {
  std::string s;
  for (int v : g) s += static_cast<char>('0' + v);
  return s;
}

}  // namespace ref
