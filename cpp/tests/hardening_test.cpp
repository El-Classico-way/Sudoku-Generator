// Cross-component and differential tests (milestone M06). Production
// results are compared against the naive implementations in reference.hpp
// on fixed-seed pseudo-random inputs. Failure messages include the trial
// number and the input grid so a failure can be replayed.
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/puzzle.hpp"
#include "sudoku/solver.hpp"

using namespace sudoku;

namespace {

Board to_board(const ref::Grid& g) { return Board(g); }
ref::Grid to_grid(const Board& b) {
  ref::Grid g{};
  for (int i = 0; i < kCellCount; ++i) g[static_cast<std::size_t>(i)] = b.get(i);
  return g;
}

// Random board with digits 0..9 at the given fill density (may conflict).
ref::Grid random_board(ref::Rng& rng, int percent_filled) {
  ref::Grid g{};
  for (auto& c : g) c = rng.below(100) < percent_filled ? 1 + rng.below(9) : 0;
  return g;
}

// Validators vs. the pairwise reference definition.
void test_validation_differential() {
  ref::Rng rng(2024);
  int inconsistent = 0, consistent = 0;
  for (int trial = 0; trial < 400; ++trial) {
    // Mix densities; sparse boards are often consistent, dense ones rarely.
    const int density = (trial % 4 == 0) ? 3 : (trial % 4 == 1) ? 8 : (trial % 4 == 2) ? 20 : 60;
    const ref::Grid g = random_board(rng, density);
    const Board b = to_board(g);
    const std::string ctx = "trial " + std::to_string(trial) + " " + ref::to_string(g);
    const bool ref_ok = ref::consistent(g);
    (ref_ok ? consistent : inconsistent)++;
    CHECK_CTX(b.is_consistent() == ref_ok, ctx);
    CHECK_CTX(b.find_conflicts().empty() == ref_ok, ctx);
    CHECK_CTX(b.is_valid_solution() == (ref_ok && b.is_complete()), ctx);
    for (int u = 0; u < 9; ++u) {
      CHECK_CTX(b.row_has_conflict(u) == ref::unit_has_pair(g, 0, u), ctx);
      CHECK_CTX(b.column_has_conflict(u) == ref::unit_has_pair(g, 1, u), ctx);
      CHECK_CTX(b.box_has_conflict(u) == ref::unit_has_pair(g, 2, u), ctx);
    }
    // find_conflicts() lists exactly the units the per-unit checks flag.
    std::array<std::array<bool, 9>, 3> flagged{};
    for (const Conflict& c : b.find_conflicts()) {
      flagged[static_cast<std::size_t>(c.unit)][static_cast<std::size_t>(c.index)] = true;
    }
    for (int u = 0; u < 9; ++u) {
      CHECK_CTX(flagged[0][u] == b.row_has_conflict(u), ctx);
      CHECK_CTX(flagged[1][u] == b.column_has_conflict(u), ctx);
      CHECK_CTX(flagged[2][u] == b.box_has_conflict(u), ctx);
    }
  }
  CHECK(consistent > 50 && inconsistent > 50);  // both outcomes were exercised
}

// Solver vs. naive counting on boards derived from solved grids (blanked
// cells) and on random consistent placements (often unsolvable).
void test_solver_differential() {
  ref::Rng rng(7);
  constexpr std::uint64_t kCap = 20;
  int compared = 0, skipped = 0, zero = 0, one = 0, many = 0;
  for (int trial = 0; trial < 250; ++trial) {
    ref::Grid g{};
    const std::string kind = (trial % 2 == 0) ? "blanked" : "placed";
    if (trial % 2 == 0) {
      g = to_grid(*generate_complete_grid(static_cast<std::uint64_t>(trial)).grid);
      const int blanks = 25 + rng.below(30);
      for (int k = 0; k < blanks; ++k) g[static_cast<std::size_t>(rng.below(81))] = 0;
    } else {
      const int target = 17 + rng.below(14);
      for (int placed = 0, attempts = 0; placed < target && attempts < 2000; ++attempts) {
        const int cell = rng.below(81), d = 1 + rng.below(9);
        if (g[static_cast<std::size_t>(cell)] == 0 && ref::placeable(g, cell, d)) {
          g[static_cast<std::size_t>(cell)] = d;
          ++placed;
        }
      }
    }
    const std::string ctx = kind + " trial " + std::to_string(trial) + " " + ref::to_string(g);
    CHECK_CTX(ref::consistent(g), ctx);
    const auto expected = ref::count(g, kCap);
    if (!expected) { ++skipped; continue; }
    ++compared;
    const Board b = to_board(g);
    const Board before = b;

    const CountResult c = count_solutions(b, kCap);
    if (*expected < kCap) {
      CHECK_CTX(c.status == SearchStatus::Exhausted && c.count == *expected, ctx);
    } else {
      CHECK_CTX(c.status == SearchStatus::SolutionLimitReached && c.count == kCap, ctx);
    }
    const SolveResult s = find_solution(b);
    CHECK_CTX((s.status == SolveStatus::Solved) == (*expected > 0), ctx);
    CHECK_CTX((s.status == SolveStatus::Unsolvable) == (*expected == 0), ctx);
    if (s.solution) {
      const ref::Grid sg = to_grid(*s.solution);
      CHECK_CTX(ref::consistent(sg) && ref::count(sg, 2) == 1u, ctx);
      for (int i = 0; i < 81; ++i) {
        CHECK_CTX(g[static_cast<std::size_t>(i)] == 0 || g[static_cast<std::size_t>(i)] == sg[static_cast<std::size_t>(i)], ctx);
      }
    }
    const SolutionClass k = classify_uniqueness(b);
    const SolutionClass want = *expected == 0 ? SolutionClass::NoSolution
                               : *expected == 1 ? SolutionClass::Unique
                                                : SolutionClass::Multiple;
    CHECK_CTX(k == want, ctx);
    CHECK_CTX(b == before, ctx);
    (*expected == 0 ? zero : *expected == 1 ? one : many)++;
  }
  // The sample must actually contain every outcome and few skips.
  CHECK(compared > 150);
  CHECK(zero > 10 && one > 10 && many > 10);
  std::printf("solver differential: compared=%d skipped=%d zero=%d one=%d many=%d\n",
              compared, skipped, zero, one, many);
}

// Backtracking must restore all state: after searching a hard puzzle with
// many backtracks, a second search of the same board gives identical results.
void test_backtracking_state_restored() {
  const char* hard =
      "8........" "..36....." ".7..9.2.." ".5...7..." "....457.."
      "...1...3." "..1....68" "..85...1." ".9....4..";
  ref::Grid g{};
  for (int i = 0; i < 81; ++i) g[static_cast<std::size_t>(i)] = hard[i] == '.' ? 0 : hard[i] - '0';
  const Board b = to_board(g);
  const CountResult a = count_solutions(b, 2);
  CHECK(a.stats.backtracks > 0);
  CHECK(a.stats.assignments == a.stats.backtracks);  // every placement undone
  for (int i = 0; i < 3; ++i) {
    const CountResult again = count_solutions(b, 2);
    CHECK(again.count == 1 && again.status == SearchStatus::Exhausted);
    CHECK(again.stats.nodes == a.stats.nodes);
    CHECK(again.first_solution == a.first_solution);
  }
  // Unlimited-ish count (limit 3) must not exceed the exact total of 1.
  CHECK(count_solutions(b, 3).count == 1);
}

// Pipeline: generate -> validate -> remove clues -> count -> solve -> compare.
void test_pipeline() {
  int minimal_checked = 0;
  for (std::uint64_t seed = 0; seed < 25; ++seed) {
    const std::string ctx = "seed " + std::to_string(seed);
    const GenerateResult g = generate_complete_grid(seed);
    CHECK_CTX(g.status == GenerateStatus::Generated && g.grid, ctx);
    if (!g.grid) continue;
    const Board grid = *g.grid;
    CHECK_CTX(grid.is_valid_solution() && ref::consistent(to_grid(grid)), ctx);
    const CountResult gc = count_solutions(grid, 2);
    CHECK_CTX(gc.status == SearchStatus::Exhausted && gc.count == 1, ctx);

    PuzzleOptions o;
    o.target_clues = 30;
    const Board grid_before = grid;
    const PuzzleResult r = remove_clues(grid, seed, o);
    CHECK_CTX(grid == grid_before, ctx);
    CHECK_CTX(r.puzzle.has_value(), ctx);
    if (!r.puzzle) continue;
    const Board& p = *r.puzzle;
    int clues = 0;
    for (int i = 0; i < kCellCount; ++i) {
      if (p.get(i) != 0) {
        ++clues;
        CHECK_CTX(p.get(i) == grid.get(i), ctx);
      }
    }
    CHECK_CTX(clues == r.clue_count, ctx);
    CHECK_CTX((r.status == PuzzleStatus::TargetReached) == (clues == o.target_clues), ctx);
    CHECK_CTX(clues >= o.target_clues, ctx);
    CHECK_CTX(p.is_consistent() && !p.is_complete(), ctx);

    const CountResult pc = count_solutions(p, 2);
    CHECK_CTX(pc.status == SearchStatus::Exhausted && pc.count == 1, ctx);
    CHECK_CTX(pc.first_solution && *pc.first_solution == grid, ctx);
    CHECK_CTX(classify_uniqueness(p) == SolutionClass::Unique, ctx);
    const SolveResult s = find_solution(p);
    CHECK_CTX(s.status == SolveStatus::Solved && s.solution && *s.solution == grid, ctx);
    // Independent confirmation of uniqueness by the naive counter (only when
    // the naive search is affordable).
    if (const auto n = ref::count(to_grid(p), 2, 50000)) {
      CHECK_CTX(*n == 1, ctx);
    }
    // Determinism of the whole pipeline.
    const PuzzleResult again = generate_puzzle(seed, o);
    const PuzzleResult again2 = generate_puzzle(seed, o);
    CHECK_CTX(again.puzzle && again2.puzzle && *again.puzzle == *again2.puzzle, ctx);
    CHECK_CTX(again.solution && *again.solution == grid, ctx);

    // Restoration check: when a full pass could not reach the target, every
    // remaining clue must be necessary (removing any of them breaks
    // uniqueness). This verifies that rejected removals were restored.
    PuzzleOptions floor;
    floor.target_clues = kMinPuzzleClues;
    const PuzzleResult m = remove_clues(grid, seed, floor);
    if (m.status == PuzzleStatus::TargetNotReached && m.puzzle &&
        m.stats.cells_considered == kCellCount &&
        m.stats.rejected_inconclusive == 0) {
      ++minimal_checked;
      CHECK_CTX(m.stats.rejected_not_unique > 0, ctx);
      for (int i = 0; i < kCellCount; ++i) {
        if (m.puzzle->get(i) == 0) continue;
        Board q = *m.puzzle;
        q.set(i, 0);
        CHECK_CTX(classify_uniqueness(q) == SolutionClass::Multiple, ctx + " cell " + std::to_string(i));
      }
    }
  }
  CHECK(minimal_checked >= 20);
}

// Failed/invalid operations must not mutate their inputs.
void test_inputs_not_mutated_by_failures() {
  Board bad;
  bad.set(0, 0, 4);
  bad.set(0, 5, 4);  // row conflict
  const Board before = bad;
  CHECK(count_solutions(bad, 2).status == SearchStatus::InvalidInput);
  CHECK(find_solution(bad).status == SolveStatus::InvalidInput);
  CHECK(remove_clues(bad, 1).status == PuzzleStatus::InvalidSolution);
  CHECK(bad == before);
  const Board full = *generate_complete_grid(1).grid;
  const Board full_before = full;
  PuzzleOptions o;
  o.target_clues = 3;
  CHECK(remove_clues(full, 1, o).status == PuzzleStatus::InvalidTarget);
  CHECK(full == full_before);
}

}  // namespace

int main() {
  test_validation_differential();
  test_solver_differential();
  test_backtracking_state_restored();
  test_pipeline();
  test_inputs_not_mutated_by_failures();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
