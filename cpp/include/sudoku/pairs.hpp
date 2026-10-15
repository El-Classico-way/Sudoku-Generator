// Naked-pair and hidden-pair detection. Detection is pure: it inspects a
// CandidateGrid and returns proposed elimination Deductions; apply() carries
// them out (re-validating the evidence). No search, guessing or randomness.
//
// Naked pair (Technique::NakedPair), in a unit U: exactly two empty cells of U
//   have the identical candidate set {a, b} (two digits). Then a and b belong
//   to those two cells, so both are eliminated from every OTHER cell of U that
//   currently has them. The pair cells are never targeted.
// Hidden pair (Technique::HiddenPair), in a unit U: two digits a, b, neither
//   placed in U, each have exactly the same two candidate cells in U. Then a
//   and b belong to those two cells, so every OTHER candidate is eliminated
//   from those two cells. The pair digits are never removed.
// Both are checked in every row, column and box.
//
// Ambiguous patterns are not used: if three or more cells of a unit share the
// same two-candidate set {a, b} (naked), or three or more digits share the
// same two cells (hidden), the state cannot be completed but the local
// contradiction checks do not flag it; no deduction is emitted for that
// pattern rather than picking two arbitrary members. A digit with one location
// is a hidden single (singles.hpp) and a digit with none is a contradiction.
// A deduction is only emitted when it has at least one target.
//
// Evidence: unit = the source unit, cells = the two pair cells (ascending),
// digits = the two pair digits; targets are the exact (cell, digit)
// candidates to remove, ordered by cell then digit, every one currently
// present.
//
// Results: DetectResult as for the other techniques (deduction.hpp): Ok (list
// possibly empty), Contradiction (no deductions) or InvalidBoard (Board
// overload).
//
// Order: units rows 0..8, columns 0..8, boxes 0..8; within a unit by pair
// digits ascending (lower digit, then higher). find_pairs returns all naked
// pairs, then all hidden pairs. Deductions are not merged: the same pair seen
// from its row and its box gives two deductions, and eliminations may
// overlap; application is idempotent per candidate (the later one reports
// NoChange or, if the earlier one invalidated its evidence, Unsupported).
// A detected list is a snapshot; re-detect after applying.
#pragma once

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"

namespace sudoku {

DetectResult find_naked_pairs(const CandidateGrid& grid);
DetectResult find_hidden_pairs(const CandidateGrid& grid);
DetectResult find_pairs(const CandidateGrid& grid);

// Builds candidates with CandidateGrid::from_board, then find_pairs().
DetectResult find_pairs(const Board& board);

}  // namespace sudoku
