#include "sudoku/puzzle.hpp"

#include <array>
#include <random>
#include <stdexcept>

#include "sudoku/generator.hpp"
#include "search.hpp"

namespace sudoku {
namespace {

int count_clues(const Board& b) {
  int n = 0;
  for (auto c : b.cells()) n += (c != kEmptyCell);
  return n;
}

}  // namespace

PuzzleResult remove_clues(const Board& solution, std::uint64_t seed,
                          const PuzzleOptions& options) {
  SearchOptions search_options;
  search_options.node_budget = options.node_budget;
  detail::check_options(search_options);

  PuzzleResult out;
  if (options.target_clues < kMinPuzzleClues ||
      options.target_clues > kCellCount) {
    out.status = PuzzleStatus::InvalidTarget;
    return out;
  }
  if (!solution.is_valid_solution()) {
    out.status = PuzzleStatus::InvalidSolution;
    return out;
  }

  Board puzzle = solution;  // working copy; the input is never modified
  int clues = kCellCount;

  // The starting grid must itself be a conclusively unique puzzle (it is
  // complete, so this takes one node); verified before any removal.
  {
    const CountResult c = count_solutions(puzzle, 2, search_options);
    if (classify(c) != SolutionClass::Unique) {
      throw std::logic_error("complete grid not verified as unique");
    }
  }

  std::array<int, kCellCount> order{};
  for (int i = 0; i < kCellCount; ++i) order[static_cast<std::size_t>(i)] = i;
  std::mt19937_64 rng(seed);
  detail::shuffle_in_place(rng, order.data(), kCellCount);

  for (int index : order) {
    if (clues == options.target_clues) break;
    ++out.stats.cells_considered;
    const int digit = puzzle.get(index);
    puzzle.set(index, kEmptyCell);

    const CountResult c = count_solutions(puzzle, 2, search_options);
    const SolutionClass k = classify(c);
    if (k == SolutionClass::Unique) {  // conclusive and exactly one
      --clues;
      ++out.stats.removed;
      continue;
    }
    puzzle.set(index, digit);  // revert: not unique, or not conclusive
    if (k == SolutionClass::Unknown) {
      ++out.stats.rejected_inconclusive;
    } else {
      ++out.stats.rejected_not_unique;
    }
  }

  // Final independent verification.
  const CountResult fin = count_solutions(puzzle, 2, search_options);
  if (classify(fin) != SolutionClass::Unique || !fin.first_solution ||
      *fin.first_solution != solution) {
    throw std::logic_error("final puzzle failed uniqueness verification");
  }
  for (int i = 0; i < kCellCount; ++i) {
    if (puzzle.get(i) != kEmptyCell && puzzle.get(i) != solution.get(i)) {
      throw std::logic_error("puzzle clue differs from the solution");
    }
  }
  if (count_clues(puzzle) != clues) {
    throw std::logic_error("clue count bookkeeping mismatch");
  }

  out.status = clues == options.target_clues ? PuzzleStatus::TargetReached
                                             : PuzzleStatus::TargetNotReached;
  out.clue_count = clues;
  out.puzzle = puzzle;
  out.solution = solution;
  return out;
}

PuzzleResult generate_puzzle(std::uint64_t seed, const PuzzleOptions& options) {
  GenerateResult g = generate_complete_grid(seed);
  if (g.status != GenerateStatus::Generated || !g.grid) {
    throw std::logic_error("unbounded grid generation did not finish");
  }
  constexpr std::uint64_t kRemovalSalt = 0x9E3779B97F4A7C15ull;
  return remove_clues(*g.grid, seed ^ kRemovalSalt, options);
}

}  // namespace sudoku
