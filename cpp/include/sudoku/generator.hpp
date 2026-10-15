// Random complete-grid generation. Produces COMPLETE 9x9 Sudoku grids only;
// clue removal and puzzle generation are not implemented.
//
// Algorithm: randomized depth-first backtracking from an empty board using
// the same search engine as the solver (sudoku/solver.hpp): the empty cell
// with the fewest legal digits is chosen (ties: lowest row-major index) and
// its legal digits are tried in an order shuffled by a std::mt19937_64 owned
// by the request. The finished grid is verified with
// Board::is_valid_solution() before it is returned.
//
// Seeding model: one request = one seed. Every call builds its own engine
// from the seed; there is no shared or global random state, and no use of
// std::rand, the clock or entropy sources. Nothing is advanced between calls.
//
// Reproducibility: the same seed with the same implementation version and
// options always yields the same result. The shuffle uses only the raw
// output of std::mt19937_64 (fully specified by the standard) and integer
// arithmetic, so results are expected to match across standard libraries, but
// this has only been verified with the compiler listed in the README, and the
// output may change when the algorithm is revised. A node budget changes
// only whether generation finishes, never which grid a seed leads to.
//
// Distribution: NOT claimed to be uniform over all Sudoku grids. Different
// seeds may yield the same grid.
//
// Errors: failure to finish within a node budget is a normal result
// (GenerateStatus::BudgetExhausted), never a partial or default grid. A zero
// budget throws std::invalid_argument. An internal inconsistency (an invalid
// grid from the search) throws std::logic_error. Without a budget the search
// is unbounded; from an empty board it completes quickly in practice.
#pragma once

#include <cstdint>
#include <optional>

#include "sudoku/board.hpp"
#include "sudoku/solver.hpp"

namespace sudoku {

struct GenerateOptions {
  // Maximum search nodes to visit (same definition as
  // SearchOptions::node_budget); nullopt = unlimited. Zero is invalid. A
  // complete grid needs at least 82 nodes (root + one per cell).
  std::optional<std::uint64_t> node_budget;
};

enum class GenerateStatus {
  Generated,        // `grid` holds a complete valid Sudoku
  BudgetExhausted,  // stopped by the node budget; `grid` is empty
};

struct GenerateResult {
  GenerateStatus status = GenerateStatus::BudgetExhausted;
  std::optional<Board> grid;  // engaged iff status == Generated
  SearchStats stats;          // definitions as in solver.hpp
};

GenerateResult generate_complete_grid(std::uint64_t seed,
                                      const GenerateOptions& options = {});

}  // namespace sudoku
