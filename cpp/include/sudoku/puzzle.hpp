// Puzzle creation by uniqueness-preserving clue removal.
//
// remove_clues() starts from a verified complete Sudoku solution and visits
// the 81 cells in an order shuffled by a seeded std::mt19937_64. For each
// cell it tentatively empties it and counts solutions of the result with
// count_solutions(puzzle, 2, ...). The removal is KEPT only if the count is
// conclusive and exactly one: SearchStatus::Exhausted with count == 1
// (SolutionClass::Unique). Zero solutions, two or more, and any search
// stopped early (node budget) all REVERT the removal. Because the puzzle is
// always a subset of the starting solution, "exactly one solution" means
// that solution is the starting grid. Before returning, the final puzzle is
// re-counted and checked to have exactly that one solution.
//
// Visiting stops as soon as the clue count equals the target, or after every
// cell has been considered once. A single pass is not guaranteed to reach low
// targets: a cell rejected early is not retried after later removals (and a
// retry could not help, since removing more clues only adds solutions).
//
// Results (PuzzleStatus)
//   TargetReached    : unique puzzle with exactly target_clues clues.
//   TargetNotReached : valid unique puzzle with MORE clues than requested
//                      (still verified; not a failure of uniqueness).
//   InvalidSolution  : the input is not a complete conflict-free grid.
//   InvalidTarget    : target_clues outside [17, 81]. Checked first.
// In the Invalid* cases no puzzle is produced. A target of 81 returns the
// verified, unchanged grid. Ordinary outcomes are not exceptions; a zero
// node budget throws std::invalid_argument, and a verification failure that
// "cannot happen" (e.g. final recount not Unique) throws std::logic_error.
//
// Reproducibility: same solution, seed, options and implementation version
// give the same result. Only the removal order depends on the seed. The
// shuffle uses raw std::mt19937_64 output, but cross-compiler identity is
// only verified for the compiler named in the README, and the output may
// change if the algorithm changes. No global state or clock is used.
//
// Scope: this does not rate difficulty or use logical techniques.
#pragma once

#include <cstdint>
#include <optional>

#include "sudoku/board.hpp"
#include "sudoku/solver.hpp"

namespace sudoku {

inline constexpr int kMinPuzzleClues = 17;  // no uniquely solvable 16-clue
                                            // puzzle is known

struct PuzzleOptions {
  // Desired number of clues, in [kMinPuzzleClues, kCellCount].
  int target_clues = kMinPuzzleClues;
  // Node budget applied to EACH solution count (SearchOptions::node_budget);
  // nullopt = unlimited. A count that hits the budget is inconclusive and
  // its removal is reverted. Zero is invalid.
  std::optional<std::uint64_t> node_budget;
};

enum class PuzzleStatus {
  TargetReached,
  TargetNotReached,
  InvalidSolution,
  InvalidTarget,
};

struct RemovalStats {
  int cells_considered = 0;         // positions visited
  int removed = 0;                  // removals kept
  int rejected_not_unique = 0;      // reverted: 0 or >= 2 solutions
  int rejected_inconclusive = 0;    // reverted: count stopped by the budget
};

struct PuzzleResult {
  PuzzleStatus status = PuzzleStatus::InvalidSolution;
  std::optional<Board> puzzle;    // engaged iff status is TargetReached or
                                  // TargetNotReached; verified unique
  std::optional<Board> solution;  // copy of the input solution, same condition
  int clue_count = 0;             // nonzero cells of `puzzle`; 0 if none
  RemovalStats stats;
};

PuzzleResult remove_clues(const Board& solution, std::uint64_t seed,
                          const PuzzleOptions& options = {});

// End to end: generate_complete_grid(seed), then remove_clues() with a seed
// derived from `seed` (so the two random streams are distinct).
// Deterministic in `seed`. options.node_budget applies to the clue-removal
// counts only; grid generation is unbounded.
PuzzleResult generate_puzzle(std::uint64_t seed, const PuzzleOptions& options = {});

}  // namespace sudoku
