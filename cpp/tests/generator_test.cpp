#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/solver.hpp"

using namespace sudoku;

namespace {

// Independent check, written against raw cells rather than the Board
// validators: each row, column and box must contain exactly the set 1..9.
bool independently_valid(const Board& b) {
  auto is_perfect = [&](auto cell_of) {
    std::bitset<10> seen;
    for (int k = 0; k < 9; ++k) {
      const int d = b.get(cell_of(k));
      if (d < 1 || d > 9 || seen[static_cast<std::size_t>(d)]) return false;
      seen[static_cast<std::size_t>(d)] = true;
    }
    return true;
  };
  for (int u = 0; u < 9; ++u) {
    if (!is_perfect([&](int k) { return u * 9 + k; })) return false;
    if (!is_perfect([&](int k) { return k * 9 + u; })) return false;
    const int top = u / 3 * 3, left = u % 3 * 3;
    if (!is_perfect([&](int k) { return (top + k / 3) * 9 + left + k % 3; }))
      return false;
  }
  return true;
}

constexpr std::uint64_t kSeeds[] = {0, 1, 2, 3, 7, 42, 1234, 99999,
                                    0xFFFFFFFFFFFFFFFFull};

void check_generated(const GenerateResult& r) {
  CHECK(r.status == GenerateStatus::Generated);
  CHECK(r.grid.has_value());
  if (!r.grid) return;
  CHECK(r.grid->is_complete());
  CHECK(r.grid->is_consistent());
  CHECK(r.grid->is_valid_solution());
  CHECK(independently_valid(*r.grid));
  CHECK(r.stats.nodes >= 82);  // root + one node per filled cell
  CHECK(r.stats.assignments >= 81);
}

void test_valid_for_many_seeds() {
  for (std::uint64_t seed : kSeeds) check_generated(generate_complete_grid(seed));
  for (std::uint64_t seed = 100; seed < 150; ++seed) {
    check_generated(generate_complete_grid(seed));
  }
}

void test_reproducible() {
  for (std::uint64_t seed : kSeeds) {
    const GenerateResult a = generate_complete_grid(seed);
    const GenerateResult b = generate_complete_grid(seed);
    CHECK(a.grid && b.grid && *a.grid == *b.grid);
    CHECK(a.stats.nodes == b.stats.nodes);
    CHECK(a.stats.backtracks == b.stats.backtracks);
  }
  // Interleaving other requests does not affect a seed's result (no shared
  // state between calls).
  const Board first = *generate_complete_grid(5).grid;
  (void)generate_complete_grid(6);
  (void)generate_complete_grid(7);
  CHECK(*generate_complete_grid(5).grid == first);
  // A node budget only decides whether generation finishes; a large budget
  // yields the same grid as no budget.
  GenerateOptions big;
  big.node_budget = 1000000;
  CHECK(*generate_complete_grid(5, big).grid == first);
}

void test_seeds_differ() {
  std::set<std::vector<std::uint8_t>> distinct;
  for (std::uint64_t seed : kSeeds) {
    const Board b = *generate_complete_grid(seed).grid;
    distinct.insert(std::vector<std::uint8_t>(b.cells().begin(), b.cells().end()));
  }
  CHECK(distinct.size() >= 2);
  // Strong in practice with 9 seeds: expect nearly all to differ.
  CHECK(distinct.size() >= 8);
  // Not a fixed template: not equal to the ascending-order solver's grid.
  const Board ascending = *find_solution(Board{}).solution;
  bool all_equal = true;
  for (std::uint64_t seed : kSeeds) {
    if (*generate_complete_grid(seed).grid != ascending) all_equal = false;
  }
  CHECK(!all_equal);
}

// Regression pin of the current algorithm (seed 42). Update deliberately,
// together with the documented algorithm revision, if the search changes.
void test_golden_seed() {
  const Board b = *generate_complete_grid(42).grid;
  const char* expected = "396542718571386294842917356725638149139475682468291573284153967657829431913764825";
  std::string s;
  for (auto c : b.cells()) s += static_cast<char>('0' + c);
  CHECK(s == expected);
}

void test_budget_failure() {
  GenerateOptions o;
  // 82 nodes are required, so a budget of 81 can never succeed.
  for (std::uint64_t budget : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{81}}) {
    o.node_budget = budget;
    for (std::uint64_t seed : kSeeds) {
      const GenerateResult r = generate_complete_grid(seed, o);
      CHECK(r.status == GenerateStatus::BudgetExhausted);
      CHECK(!r.grid.has_value());
      CHECK(r.stats.nodes == budget);
    }
  }
  // Whatever the budget, the outcome is either failure with no grid or a
  // fully valid grid.
  for (std::uint64_t budget = 1; budget <= 300; budget += 7) {
    o.node_budget = budget;
    const GenerateResult r = generate_complete_grid(3, o);
    if (r.status == GenerateStatus::Generated) {
      check_generated(r);
    } else {
      CHECK(r.status == GenerateStatus::BudgetExhausted && !r.grid);
    }
  }
  o.node_budget = 0;
  CHECK_THROWS_AS(generate_complete_grid(1, o), std::invalid_argument);
}

void test_global_random_state_untouched() {
  std::srand(123);
  const int expected = std::rand();
  std::srand(123);
  (void)generate_complete_grid(1);
  CHECK(std::rand() == expected);
}

}  // namespace

int main() {
  test_valid_for_many_seeds();
  test_reproducible();
  test_seeds_differ();
  test_golden_seed();
  test_budget_failure();
  test_global_random_state_untouched();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
