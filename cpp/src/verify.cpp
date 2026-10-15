#include "sudoku/verify.hpp"

#include <array>

namespace sudoku {
namespace {

// First digit that does not occur exactly once among the nine cells
// first_cell, first_cell + step, ... (one row or column), or 0 if all do.
int bad_digit(const Board& b, int first_cell, int step) {
  std::array<int, kMaxDigit + 1> seen{};
  for (int i = 0; i < kBoardSize; ++i) ++seen[static_cast<std::size_t>(b.get(first_cell + i * step))];
  for (int d = kMinDigit; d <= kMaxDigit; ++d) {
    if (seen[static_cast<std::size_t>(d)] != 1) return d;
  }
  return 0;
}

}  // namespace

SolutionVerdict verify_solution(const Board& solution, const Board& givens) {
  for (int i = 0; i < kCellCount; ++i) {
    if (solution.get(i) == kEmptyCell) return {SolutionFault::EmptyCell, i, 0};
  }
  for (int i = 0; i < kCellCount; ++i) {
    if (givens.get(i) != kEmptyCell && givens.get(i) != solution.get(i)) {
      return {SolutionFault::GivenChanged, i, givens.get(i)};
    }
  }
  for (int r = 0; r < kBoardSize; ++r) {
    if (const int d = bad_digit(solution, r * kBoardSize, 1)) return {SolutionFault::RowViolation, r, d};
  }
  for (int c = 0; c < kBoardSize; ++c) {
    if (const int d = bad_digit(solution, c, kBoardSize)) return {SolutionFault::ColumnViolation, c, d};
  }
  // Boxes: gather each box's nine cells explicitly.
  for (int box = 0; box < kBoardSize; ++box) {
    std::array<int, kMaxDigit + 1> seen{};
    const int top = (box / kBoxSize) * kBoxSize, left = (box % kBoxSize) * kBoxSize;
    for (int dr = 0; dr < kBoxSize; ++dr)
      for (int dc = 0; dc < kBoxSize; ++dc)
        ++seen[static_cast<std::size_t>(solution.get(top + dr, left + dc))];
    for (int d = kMinDigit; d <= kMaxDigit; ++d) {
      if (seen[static_cast<std::size_t>(d)] != 1) return {SolutionFault::BoxViolation, box, d};
    }
  }
  return {};
}

}  // namespace sudoku
