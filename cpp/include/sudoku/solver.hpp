// Backtracking Sudoku search: find one solution, or count solutions up to a
// limit. Built on sudoku::Board; the caller's Board is never modified.
//
// Terminology
//   invalid input   : the puzzle has a repeated digit in a row, column or box
//                     (Board::is_consistent() is false). Nothing is searched.
//   unsolvable      : a consistent puzzle for which an EXHAUSTIVE search found
//                     no completion. Never inferred from a stopped search.
//   exhaustive      : the whole search space was explored (SearchStatus::
//                     Exhausted). Only then is a count exact.
//   lower bound     : the search stopped early (solution limit or node
//                     budget); the true number of solutions is >= count.
//
// Search: depth-first backtracking. At each node the empty cell with the
// fewest legal digits is chosen (ties: lowest row-major index) and digits are
// tried in ascending order, so results are deterministic. A cell with no
// legal digit ends its branch. Each completed grid is re-checked with
// Board::is_valid_solution() before it is counted.
//
// Errors: ordinary outcomes (invalid input, no solution, early stop) are
// reported in the result, not thrown. Invalid arguments (a zero solution
// limit or zero node budget) throw std::invalid_argument.
#pragma once

#include <cstdint>
#include <optional>

#include "sudoku/board.hpp"

namespace sudoku {

struct SearchOptions {
  // Maximum number of search nodes to visit; nullopt = unlimited. Must not
  // be zero (std::invalid_argument). A "node" is one entry into the recursive
  // search routine, including the root and nodes that hold a full grid. The
  // budget is checked on entry to each node: when `budget` nodes have already
  // been visited the search stops with SearchStatus::BudgetExhausted.
  std::optional<std::uint64_t> node_budget;
};

// Work done by one search. Informational only; never affects correctness.
struct SearchStats {
  std::uint64_t nodes = 0;        // nodes visited (see SearchOptions)
  std::uint64_t assignments = 0;  // digits tentatively placed in a cell
  std::uint64_t backtracks = 0;   // placements undone
};

enum class SearchStatus {
  InvalidInput,         // inconsistent puzzle; no search was performed
  Exhausted,            // search space fully explored; count is EXACT
  SolutionLimitReached, // stopped at the limit; count is a LOWER BOUND
  BudgetExhausted,      // stopped by node budget; count is a LOWER BOUND
};

struct CountResult {
  SearchStatus status = SearchStatus::InvalidInput;
  // Solutions found. Never exceeds the requested limit. Exact total iff
  // status == Exhausted; otherwise the true total is at least this value.
  std::uint64_t count = 0;
  // The first solution found (search order), if any.
  std::optional<Board> first_solution;
  SearchStats stats;

  bool is_exact() const noexcept { return status == SearchStatus::Exhausted; }
};

enum class SolveStatus {
  Solved,         // `solution` holds a complete valid Sudoku
  Unsolvable,     // consistent puzzle, exhaustive search found nothing
  InvalidInput,   // inconsistent puzzle
  BudgetExhausted // stopped before finding a solution or proving there is none
};

struct SolveResult {
  SolveStatus status = SolveStatus::InvalidInput;
  std::optional<Board> solution;  // engaged iff status == Solved
  SearchStats stats;
};

// What a search has established about a puzzle.
enum class SolutionClass {
  InvalidInput,  // inconsistent puzzle
  NoSolution,    // proven: exhaustive search, zero solutions
  Unique,        // proven: exhaustive search, exactly one solution
  Multiple,      // proven: at least two solutions (exact or lower bound)
  Unknown,       // search stopped before establishing any of the above
};

// Finds a solution (the first in search order) and stops. Does not prove
// uniqueness. Finding one for an empty board is fast.
SolveResult find_solution(const Board& puzzle, const SearchOptions& options = {});

// Counts solutions, stopping once `limit` are found. `limit` must be >= 1
// (std::invalid_argument otherwise). Reaching the limit yields
// SolutionLimitReached with count == limit: the puzzle has AT LEAST that
// many solutions. If the search ends with fewer than `limit` solutions
// found and not interrupted, the count is exact. Note that limit == 1 cannot
// establish uniqueness.
CountResult count_solutions(const Board& puzzle, std::uint64_t limit,
                            const SearchOptions& options = {});

// Interprets a CountResult. Unique requires an exhaustive search that found
// exactly one solution; a stopped search with one solution is Unknown.
SolutionClass classify(const CountResult& result) noexcept;

// Convenience: count_solutions(puzzle, 2, options) followed by classify().
SolutionClass classify_uniqueness(const Board& puzzle,
                                  const SearchOptions& options = {});

}  // namespace sudoku
