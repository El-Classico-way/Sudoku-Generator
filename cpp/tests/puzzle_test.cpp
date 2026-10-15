#include <array>
#include <initializer_list>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/puzzle.hpp"
#include "sudoku/solver.hpp"

using namespace sudoku;

namespace {

// Wikipedia example solution; validity checked independently of the Board
// code (see solver_test.cpp).
constexpr std::array<int, kCellCount> kSolved = {
    5, 3, 4, 6, 7, 8, 9, 1, 2,  6, 7, 2, 1, 9, 5, 3, 4, 8,  1, 9, 8, 3, 4, 2, 5, 6, 7,
    8, 5, 9, 7, 6, 1, 4, 2, 3,  4, 2, 6, 8, 5, 3, 7, 9, 1,  7, 1, 3, 9, 2, 4, 8, 5, 6,
    9, 6, 1, 5, 3, 7, 2, 8, 4,  2, 8, 7, 4, 1, 9, 6, 3, 5,  3, 4, 5, 2, 8, 6, 1, 7, 9};

constexpr std::uint64_t kSeeds[] = {0, 1, 2, 3, 42, 1234, 99999};

int clues_of(const Board& b) {
  int n = 0;
  for (auto c : b.cells()) n += (c != 0);
  return n;
}

// Full independent check of a successful result against `solution`.
void check_valid_puzzle(const PuzzleResult& r, const Board& solution) {
  CHECK(r.puzzle.has_value() && r.solution.has_value());
  if (!r.puzzle || !r.solution) return;
  CHECK(*r.solution == solution);
  CHECK(r.puzzle->is_consistent());
  CHECK(!r.puzzle->is_complete() || r.clue_count == kCellCount);
  CHECK(r.clue_count == clues_of(*r.puzzle));
  CHECK(r.clue_count >= kMinPuzzleClues);
  for (int i = 0; i < kCellCount; ++i) {  // clues never altered
    CHECK(r.puzzle->get(i) == 0 || r.puzzle->get(i) == solution.get(i));
  }
  // Exhaustive count capped at two: exactly one solution, equal to solution.
  const CountResult c = count_solutions(*r.puzzle, 2);
  CHECK(c.status == SearchStatus::Exhausted && c.is_exact() && c.count == 1);
  CHECK(c.first_solution && *c.first_solution == solution);
  CHECK(classify_uniqueness(*r.puzzle) == SolutionClass::Unique);
  CHECK(r.stats.removed == kCellCount - r.clue_count);
}

void test_invalid_inputs() {
  const Board incomplete{};  // empty
  Board with_hole(kSolved);
  with_hole.set(4, 4, 0);
  Board conflicting(kSolved);
  conflicting.set(0, 0, 3);  // complete, but 3 now repeats in row 0, box 0
  conflicting.set(0, 1, 3);
  for (const Board* b : std::initializer_list<const Board*>{&incomplete, &with_hole, &conflicting}) {
    const Board before = *b;
    const PuzzleResult r = remove_clues(*b, 1);
    CHECK(r.status == PuzzleStatus::InvalidSolution);
    CHECK(!r.puzzle && !r.solution && r.clue_count == 0);
    CHECK(*b == before);
  }
}

void test_targets() {
  const Board solution(kSolved);
  PuzzleOptions o;
  o.target_clues = 81;
  PuzzleResult r = remove_clues(solution, 1, o);
  CHECK(r.status == PuzzleStatus::TargetReached);
  CHECK(r.puzzle && *r.puzzle == solution && r.clue_count == 81);
  CHECK(r.stats.cells_considered == 0);
  check_valid_puzzle(r, solution);

  for (int bad : {16, 0, -1, 82, 1000}) {
    o.target_clues = bad;
    r = remove_clues(solution, 1, o);
    CHECK(r.status == PuzzleStatus::InvalidTarget);
    CHECK(!r.puzzle && r.clue_count == 0);
  }
  // An invalid target is reported even for an invalid solution (documented).
  o.target_clues = 5;
  CHECK(remove_clues(Board{}, 1, o).status == PuzzleStatus::InvalidTarget);
  CHECK_THROWS_AS(remove_clues(solution, 1, [] {
                    PuzzleOptions z; z.node_budget = 0; return z; }()),
                  std::invalid_argument);
}

void test_removal_on_known_grid() {
  const Board solution(kSolved);
  const Board before = solution;
  for (std::uint64_t seed : kSeeds) {
    for (int target : {60, 40, 30, 17}) {
      PuzzleOptions o;
      o.target_clues = target;
      const PuzzleResult r = remove_clues(solution, seed, o);
      CHECK(solution == before);  // input untouched
      CHECK(r.status == PuzzleStatus::TargetReached ||
            r.status == PuzzleStatus::TargetNotReached);
      CHECK(r.clue_count >= target);  // never below the target
      CHECK((r.status == PuzzleStatus::TargetReached) == (r.clue_count == target));
      check_valid_puzzle(r, solution);
    }
    // Moderate targets are reachable for every test seed.
    PuzzleOptions o;
    o.target_clues = 40;
    const PuzzleResult r = remove_clues(solution, seed, o);
    CHECK(r.status == PuzzleStatus::TargetReached && r.clue_count == 40);
  }
}

void test_reproducible_and_seed_dependent() {
  const Board solution(kSolved);
  PuzzleOptions o;
  o.target_clues = 28;
  std::set<std::vector<std::uint8_t>> distinct;
  for (std::uint64_t seed : kSeeds) {
    const PuzzleResult a = remove_clues(solution, seed, o);
    const PuzzleResult b = remove_clues(solution, seed, o);
    CHECK(a.puzzle && b.puzzle && *a.puzzle == *b.puzzle);
    CHECK(a.status == b.status && a.clue_count == b.clue_count);
    CHECK(a.stats.cells_considered == b.stats.cells_considered);
    CHECK(a.stats.removed == b.stats.removed);
    CHECK(a.stats.rejected_not_unique == b.stats.rejected_not_unique);
    if (a.puzzle) {
      distinct.insert(std::vector<std::uint8_t>(a.puzzle->cells().begin(),
                                                a.puzzle->cells().end()));
    }
  }
  CHECK(distinct.size() >= 2);
}

void test_unreachable_target_reports_partial_success() {
  // 17 clues is the theoretical floor; one greedy pass does not reach it, so
  // the result must say so and still be a verified unique puzzle.
  const Board solution(kSolved);
  PuzzleOptions o;
  o.target_clues = kMinPuzzleClues;
  bool saw_not_reached = false;
  for (std::uint64_t seed : kSeeds) {
    const PuzzleResult r = remove_clues(solution, seed, o);
    if (r.status == PuzzleStatus::TargetNotReached) {
      saw_not_reached = true;
      CHECK(r.clue_count > kMinPuzzleClues);
      CHECK(r.stats.cells_considered == kCellCount);  // exhausted all cells
      check_valid_puzzle(r, solution);
    }
  }
  CHECK(saw_not_reached);
}

// Inconclusive counts (node budget hit) must revert the removal. With a
// budget of 1 the only affordable search is a grid with no empty cell, so
// every tentative removal is inconclusive and none may be kept.
void test_inconclusive_counts_revert() {
  const Board solution(kSolved);
  PuzzleOptions o;
  o.target_clues = 40;
  o.node_budget = 1;
  const PuzzleResult r = remove_clues(solution, 3, o);
  CHECK(r.status == PuzzleStatus::TargetNotReached);
  CHECK(r.clue_count == kCellCount && r.stats.removed == 0);
  CHECK(r.stats.rejected_inconclusive == kCellCount);
  CHECK(r.stats.rejected_not_unique == 0);
  CHECK(r.puzzle && *r.puzzle == solution);
  CHECK(count_solutions(*r.puzzle, 2, SearchOptions{1}).count == 1);

  // Sweep: for any budget the result is a puzzle verified unique with an
  // unlimited count, and rejection counters add up.
  for (std::uint64_t budget : {2, 3, 5, 10, 30, 100, 1000}) {
    o.node_budget = budget;
    const PuzzleResult s = remove_clues(solution, 3, o);
    CHECK(s.status == PuzzleStatus::TargetReached ||
          s.status == PuzzleStatus::TargetNotReached);
    check_valid_puzzle(s, solution);
    CHECK(s.stats.cells_considered ==
          s.stats.removed + s.stats.rejected_not_unique +
              s.stats.rejected_inconclusive);
  }
}

void test_end_to_end() {
  for (std::uint64_t seed : kSeeds) {
    PuzzleOptions o;
    o.target_clues = 35;
    const PuzzleResult r = generate_puzzle(seed, o);
    CHECK(r.status == PuzzleStatus::TargetReached);
    const Board grid = *generate_complete_grid(seed).grid;
    check_valid_puzzle(r, grid);  // derived from the M04 grid of that seed
    const PuzzleResult again = generate_puzzle(seed, o);
    CHECK(r.puzzle && again.puzzle && *r.puzzle == *again.puzzle);
  }
}

}  // namespace

int main() {
  test_invalid_inputs();
  test_targets();
  test_removal_on_known_grid();
  test_reproducible_and_seed_dependent();
  test_unreachable_target_reports_partial_success();
  test_inconclusive_counts_revert();
  test_end_to_end();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
