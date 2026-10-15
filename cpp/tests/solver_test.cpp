#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "check.hpp"
#include "sudoku/solver.hpp"

using namespace sudoku;

namespace {

// 81 characters, row-major; '.' or '0' = empty. Whitespace is ignored so
// fixtures can be written one row per line.
Board parse(const std::string& text) {
  std::array<int, kCellCount> v{};
  int n = 0;
  for (char ch : text) {
    if (ch == ' ' || ch == '\n') continue;
    CHECK(n < kCellCount);
    v[static_cast<std::size_t>(n++)] = (ch == '.') ? 0 : ch - '0';
  }
  CHECK(n == kCellCount);
  return Board(v);
}

// All fixtures below were solved/verified with an independent Python
// brute-force solver (not this code), as noted per fixture.

// Wikipedia example puzzle; independently confirmed to have exactly 1 solution.
const char* kWikiPuzzle =
    "53..7...."
    "6..195..."
    ".98....6."
    "8...6...3"
    "4..8.3..1"
    "7...2...6"
    ".6....28."
    "...419..5"
    "....8..79";
const char* kWikiSolution =
    "534678912"
    "672195348"
    "198342567"
    "859761423"
    "426853791"
    "713924856"
    "961537284"
    "287419635"
    "345286179";

// A hard published puzzle (Arto Inkala, 2012); unique solution
// confirmed independently.
const char* kHardPuzzle =
    "8........"
    "..36....."
    ".7..9.2.."
    ".5...7..."
    "....457.."
    "...1...3."
    "..1....68"
    "..85...1."
    ".9....4..";
const char* kHardSolution =
    "812753649"
    "943682175"
    "675491283"
    "154237896"
    "369845721"
    "287169534"
    "521974368"
    "438526917"
    "796318452";

// kWikiSolution with the "deadly rectangle" (0,3),(0,4),(3,3),(3,4) cleared.
// Those cells hold 6,7 / 7,6 in the solution; swapping to 7,6 / 6,7 also
// yields a valid grid, so there are exactly two completions (confirmed by
// brute force).
const char* kTwoSolutions =
    "534008912"
    "672195348"
    "198342567"
    "859001423"
    "426853791"
    "713924856"
    "961537284"
    "287419635"
    "345286179";
const char* kTwoSolutionsAlt =
    "534768912"
    "672195348"
    "198342567"
    "859671423"
    "426853791"
    "713924856"
    "961537284"
    "287419635"
    "345286179";

// Consistent but unsolvable: row 0 holds 1..7 in columns 0..6, so (0,7) and
// (0,8) must be 8 and 9; but (1,7)=9 forbids 9 in both cells (same column 7,
// same box 2 for (0,8)). Each cell is left with only {8}, so the search must
// place one 8 and then dead-end: an unsolvable case needing real search.
const char* kUnsolvable =
    "123456700"
    ".......9."
    "........."
    "........."
    "........."
    "........."
    "........."
    "........."
    ".........";
// Immediate dead end: (0,8) has no candidate at all (row has 1..8, column has 9).
const char* kNoCandidate =
    "12345678."
    "........9"
    "........."
    "........."
    "........."
    "........."
    "........."
    "........."
    ".........";

bool same_as(const std::optional<Board>& b, const std::string& expected) {
  return b && *b == parse(expected);
}

void test_unique_puzzles() {
  for (auto [puzzle, solution] : {std::pair{kWikiPuzzle, kWikiSolution},
                                  std::pair{kHardPuzzle, kHardSolution}}) {
    const Board p = parse(puzzle);
    const SolveResult r = find_solution(p);
    CHECK(r.status == SolveStatus::Solved);
    CHECK(same_as(r.solution, solution));
    CHECK(r.solution && r.solution->is_complete());
    CHECK(r.solution && r.solution->is_valid_solution());
    for (int i = 0; i < kCellCount; ++i) {  // givens preserved
      if (p.get(i) != 0) CHECK(r.solution && r.solution->get(i) == p.get(i));
    }
    const CountResult c = count_solutions(p, 2);
    CHECK(c.status == SearchStatus::Exhausted && c.is_exact());
    CHECK(c.count == 1);
    CHECK(classify(c) == SolutionClass::Unique);
    CHECK(classify_uniqueness(p) == SolutionClass::Unique);
  }
}

void test_unsolvable_and_invalid() {
  for (const char* s : {kUnsolvable, kNoCandidate}) {
    const Board p = parse(s);
    CHECK(p.is_consistent());
    const CountResult c = count_solutions(p, 5);
    CHECK(c.status == SearchStatus::Exhausted && c.is_exact() && c.count == 0);
    CHECK(!c.first_solution);
    CHECK(classify(c) == SolutionClass::NoSolution);
    CHECK(find_solution(p).status == SolveStatus::Unsolvable);
    CHECK(!find_solution(p).solution);
    CHECK(classify_uniqueness(p) == SolutionClass::NoSolution);
  }

  Board row, col, box;
  row.set(0, 0, 5); row.set(0, 8, 5);   // same row only
  col.set(0, 0, 5); col.set(8, 0, 5);   // same column only
  box.set(0, 0, 5); box.set(2, 2, 5);   // same box only
  for (const Board& p : {row, col, box}) {
    CHECK(!p.is_consistent());
    const CountResult c = count_solutions(p, 2);
    CHECK(c.status == SearchStatus::InvalidInput);
    CHECK(!c.is_exact() && c.count == 0 && !c.first_solution);
    CHECK(c.stats.nodes == 0);
    CHECK(classify(c) == SolutionClass::InvalidInput);
    CHECK(find_solution(p).status == SolveStatus::InvalidInput);
    CHECK(!find_solution(p).solution);
    CHECK(classify_uniqueness(p) == SolutionClass::InvalidInput);
  }
}

void test_multiple_solutions() {
  const Board p = parse(kTwoSolutions);
  // Limit 2: stops at the limit; a lower bound, not an exact total.
  const CountResult c2 = count_solutions(p, 2);
  CHECK(c2.status == SearchStatus::SolutionLimitReached);
  CHECK(!c2.is_exact());
  CHECK(c2.count == 2);
  CHECK(classify(c2) == SolutionClass::Multiple);
  CHECK(classify_uniqueness(p) == SolutionClass::Multiple);
  // A larger limit exhausts the space and gives the exact total.
  const CountResult c10 = count_solutions(p, 10);
  CHECK(c10.status == SearchStatus::Exhausted && c10.is_exact());
  CHECK(c10.count == 2);
  CHECK(classify(c10) == SolutionClass::Multiple);
  // Search order is ascending digits: (0,3) tries 6 before 7.
  CHECK(same_as(c10.first_solution, kWikiSolution));
  // The alternative completion is a distinct valid grid for the same givens.
  const Board alt = parse(kTwoSolutionsAlt);
  CHECK(alt.is_valid_solution() && alt != *c10.first_solution);
  CHECK(count_solutions(alt, 2).count == 1);
}

void test_empty_and_complete() {
  const SolveResult r = find_solution(Board{});
  CHECK(r.status == SolveStatus::Solved);
  CHECK(r.solution && r.solution->is_valid_solution());
  const CountResult few = count_solutions(Board{}, 3);  // bounded, not exhaustive
  CHECK(few.status == SearchStatus::SolutionLimitReached && few.count == 3);
  CHECK(!few.is_exact());

  const Board solved = parse(kWikiSolution);
  const CountResult c = count_solutions(solved, 2);
  CHECK(c.status == SearchStatus::Exhausted && c.is_exact() && c.count == 1);
  CHECK(c.first_solution && *c.first_solution == solved);
  CHECK(classify(c) == SolutionClass::Unique);
  CHECK(find_solution(solved).status == SolveStatus::Solved);
}

void test_limits() {
  const Board p = parse(kWikiPuzzle);
  // Limit 1 on a unique puzzle: one solution found but uniqueness NOT proven.
  const CountResult c1 = count_solutions(p, 1);
  CHECK(c1.status == SearchStatus::SolutionLimitReached);
  CHECK(c1.count == 1 && !c1.is_exact());
  CHECK(classify(c1) == SolutionClass::Unknown);
  // Same for a multi-solution puzzle with limit 1.
  CHECK(classify(count_solutions(parse(kTwoSolutions), 1)) ==
        SolutionClass::Unknown);

  CHECK_THROWS_AS(count_solutions(p, 0), std::invalid_argument);
  CHECK_THROWS_AS(find_solution(p, SearchOptions{0}), std::invalid_argument);
  CHECK_THROWS_AS(count_solutions(p, 2, SearchOptions{0}), std::invalid_argument);
  // Even invalid boards get argument validation.
  Board bad;
  bad.set(0, 0, 1); bad.set(0, 1, 1);
  CHECK_THROWS_AS(count_solutions(bad, 0), std::invalid_argument);
}

void test_budget() {
  const Board unique = parse(kWikiPuzzle);
  const Board multi = parse(kTwoSolutions);
  const Board unsolvable = parse(kUnsolvable);

  // Budget 1: only the root node is visited. Nothing may be concluded.
  for (const Board* b : {&unique, &multi, &unsolvable}) {
    const CountResult c = count_solutions(*b, 10, SearchOptions{1});
    CHECK(c.status == SearchStatus::BudgetExhausted);
    CHECK(!c.is_exact());
    CHECK(c.count == 0);
    CHECK(c.stats.nodes == 1);
    CHECK(classify(c) == SolutionClass::Unknown);  // not NoSolution
    CHECK(find_solution(*b, SearchOptions{1}).status ==
          SolveStatus::BudgetExhausted);
  }

  // Sweep budgets: whatever the budget, a stopped search never claims
  // NoSolution or Unique, counts never exceed the truth, and a large enough
  // budget finishes exhaustively with the right answer.
  struct Case { const Board* b; std::uint64_t truth; };
  for (Case cs : {Case{&unique, 1}, Case{&multi, 2}, Case{&unsolvable, 0}}) {
    bool saw_exhausted = false;
    for (std::uint64_t budget = 1; budget <= 200; ++budget) {
      const CountResult c = count_solutions(*cs.b, 10, SearchOptions{budget});
      CHECK(c.count <= cs.truth);
      CHECK(c.stats.nodes <= budget);
      if (c.status == SearchStatus::BudgetExhausted) {
        CHECK(!c.is_exact());
        const SolutionClass k = classify(c);
        CHECK(k != SolutionClass::NoSolution && k != SolutionClass::Unique);
        CHECK(!saw_exhausted);  // monotone: once done, larger budgets are done
      } else {
        saw_exhausted = true;
        CHECK(c.status == SearchStatus::Exhausted);
        CHECK(c.count == cs.truth);
      }
    }
    CHECK(saw_exhausted);
  }

  // A solution found before the budget ran out is still reported, and one
  // solution plus a stopped search is Unknown, not Unique.
  bool partial_seen = false;
  for (std::uint64_t budget = 1; budget <= 50; ++budget) {
    const CountResult c = count_solutions(multi, 10, SearchOptions{budget});
    if (c.status == SearchStatus::BudgetExhausted && c.count == 1) {
      partial_seen = true;
      CHECK(c.first_solution && c.first_solution->is_valid_solution());
      CHECK(classify(c) == SolutionClass::Unknown);
    }
  }
  CHECK(partial_seen);
}

void test_input_preserved_and_deterministic() {
  for (const char* s : {kWikiPuzzle, kTwoSolutions, kUnsolvable}) {
    const Board p = parse(s);
    const Board before = p;
    (void)find_solution(p);
    (void)count_solutions(p, 2);
    (void)count_solutions(p, 3, SearchOptions{2});
    CHECK(p == before);
  }
  Board bad;
  bad.set(0, 0, 3); bad.set(1, 1, 3);
  const Board bad_before = bad;
  (void)find_solution(bad);
  CHECK(bad == bad_before);
  const Board empty;
  (void)find_solution(empty);
  CHECK(empty == Board{});

  const Board p = parse(kHardPuzzle);
  const CountResult a = count_solutions(p, 2);
  const CountResult b = count_solutions(p, 2);
  CHECK(a.first_solution == b.first_solution && a.count == b.count);
  CHECK(a.stats.nodes == b.stats.nodes);
  CHECK(find_solution(Board{}).solution == find_solution(Board{}).solution);
}

}  // namespace

int main() {
  test_unique_puzzles();
  test_unsolvable_and_invalid();
  test_multiple_solutions();
  test_empty_and_complete();
  test_limits();
  test_budget();
  test_input_preserved_and_deterministic();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
