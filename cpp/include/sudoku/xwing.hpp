// X-Wing detection. Detection is pure: it inspects a CandidateGrid and returns
// proposed elimination Deductions; apply() (deduction.hpp) carries them out
// after re-validating the evidence. No search, guessing or randomness.
//
// X-Wing (Technique::XWing) for a digit d:
//   Row-oriented   : two distinct rows, d unplaced in both, d having EXACTLY
//                    two candidate cells in each, in the same two columns
//                    c1 < c2. Then d occupies one corner on each diagonal, so d
//                    is eliminated from every other cell of columns c1 and c2
//                    (outside the two base rows) that has it.
//   Column-oriented: the same with rows and columns swapped; d is eliminated
//                    from the other cells of the two rows outside the two base
//                    columns.
// The four corners need not be the only candidates of d in the cross units:
// the confinement that matters is in the two base units. Base units with one,
// three or more candidate positions, a digit already placed in a base unit, or
// different position pairs never qualify.
//
// Skipped as impossible/ambiguous (no deduction): three or more base units of
// the same orientation that share the same two positions (three rows needing
// two columns). The local contradiction checks do not flag that state; no
// deduction is claimed for it. A deduction is emitted only if it has a target.
//
// Evidence (the pattern; the eliminations are separate, in targets()):
//   unit        = first base unit (the lower index), second_unit = the second;
//                 orientation is unit->kind (Row = row-oriented);
//   cells       = the four corner cells ascending;
//   digits      = {d}.
// Targets are the cells to remove d from, ascending by cell index.
//
// Results: DetectResult as for the other techniques (deduction.hpp): Ok (list
// possibly empty), Contradiction (no deductions; a digit missing from a unit
// with no location is a contradiction, never an X-Wing) or InvalidBoard (Board
// overload).
//
// Order: digit 1..9, then row-oriented before column-oriented, then by the
// first and second base unit index. Different X-Wings are not merged and may
// share targets; applying a repeat is idempotent. A detected list is a
// snapshot: re-detect after applying.
#pragma once

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"

namespace sudoku {

DetectResult find_x_wings(const CandidateGrid& grid);

// Builds candidates with CandidateGrid::from_board, then find_x_wings().
DetectResult find_x_wings(const Board& board);

}  // namespace sudoku
