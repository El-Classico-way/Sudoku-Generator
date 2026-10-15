// Independent final-solution verification. This checks a finished board
// against the Sudoku rules and the original clues using only Board::get()
// and plain counting; it does not use candidates, the solvers, or
// Board::is_valid_solution(), so it can be used to double-check any producer
// of solutions (the logical solver in particular).
#pragma once

#include "sudoku/board.hpp"

namespace sudoku {

enum class SolutionFault {
  None,          // the board is a valid solution that respects the givens
  EmptyCell,     // index = first empty cell
  GivenChanged,  // index = first cell whose nonzero given differs; digit = the given
  RowViolation,  // index = row, digit = first digit not appearing exactly once
  ColumnViolation,  // index = column, digit as above
  BoxViolation,     // index = box (0..8, row-major), digit as above
};

struct SolutionVerdict {
  SolutionFault fault = SolutionFault::None;
  int index = -1;
  int digit = 0;
  bool valid() const noexcept { return fault == SolutionFault::None; }
  friend bool operator==(const SolutionVerdict&, const SolutionVerdict&) = default;
};

// Checks, in this order, and reports the first fault found:
//   1. every cell is filled;
//   2. every nonzero cell of `givens` is unchanged in `solution`;
//   3. each row, 4. each column, 5. each 3x3 box contains every digit 1..9
//      exactly once.
// `givens` may be an empty Board (no clues to preserve).
SolutionVerdict verify_solution(const Board& solution, const Board& givens);

}  // namespace sudoku
