// Naked-single and hidden-single detection. Detection is pure: it inspects a
// CandidateGrid and returns proposed Deductions; it never modifies the grid,
// the Board, or anything else. Use apply() (deduction.hpp) to carry one out.
// No search, counting, randomness or backtracking is involved: every result
// follows from the candidate sets alone.
//
// Naked single : an EMPTY cell whose candidate set is exactly {d}.
//   Deduction: place d in the cell. Evidence: cells = {cell}, digits = {d}
//   (the cell's whole candidate set), no unit.
// Hidden single: a unit U and a digit d such that U does not already contain
//   d and exactly one empty cell of U has d as a candidate.
//   Deduction: place d in that cell. Evidence: unit = U, cells = {cell},
//   digits = {d}. Zero locations is NOT a hidden single; it is a
//   contradiction (reported as DetectStatus::Contradiction). Two or more
//   locations is no deduction.
// Neither claims anything about original givens: only current Board values
// and candidates are examined.
//
// Results and errors (DetectStatus):
//   Ok            : state is healthy; `deductions` may legitimately be empty.
//   Contradiction : the grid has a local contradiction (empty cell with no
//                   candidates, or a digit with no location in a unit);
//                   `contradictions` lists them (CandidateGrid::
//                   find_contradictions order) and NO deductions are given.
//   InvalidBoard  : only from the Board overload: the Board repeats a digit
//                   in a unit, so no candidate state exists; `conflicts`
//                   lists them.
//
// Ordering (deterministic, ascending indices):
//   find_naked_singles  : by cell index.
//   find_hidden_singles : rows 0..8, then columns 0..8, then boxes 0..8; within
//                         a unit by digit 1..9.
//   find_singles        : all naked singles, then all hidden singles.
// Overlap policy: results are technique-specific and are NOT merged. A cell
// that is a naked single for d and also the hidden single for d in its row
// yields one NakedSingle and one HiddenSingle deduction (different technique
// and evidence); the same placement can also appear as a hidden single in its
// row, column and box separately. Two deductions in one list may target the
// same cell with different digits only if the state is contradictory beyond
// what the local checks see; the list is computed from one snapshot, so
// never apply it blindly: apply() re-validates each deduction, and applying
// one may make later ones stale (re-detect after applying).
#pragma once

#include <vector>

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"

namespace sudoku {

DetectResult find_naked_singles(const CandidateGrid& grid);
DetectResult find_hidden_singles(const CandidateGrid& grid);
DetectResult find_singles(const CandidateGrid& grid);

// Builds candidates with CandidateGrid::from_board, then find_singles().
DetectResult find_singles(const Board& board);

}  // namespace sudoku
