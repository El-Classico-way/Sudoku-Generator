// Naked-triple and hidden-triple detection. Detection is pure: it inspects a
// CandidateGrid and returns proposed elimination Deductions; apply()
// (deduction.hpp) carries them out after re-validating the evidence. No
// search, guessing or randomness.
//
// Naked triple (Technique::NakedTriple), in a unit U and digit set T of three
//   digits: exactly three empty cells of U whose candidate sets each have 2 or
//   3 digits, all inside T, with union exactly T. The three cells must hold
//   the three digits of T, so T is eliminated from every OTHER cell of U that
//   has them. Cell sets such as {2,5},{5,9},{2,9} and {2,5,9},{2,5,9},{2,5}
//   both qualify. The triple cells are never targeted.
// Hidden triple (Technique::HiddenTriple), in a unit U: three digits T, none
//   placed in U, each with 2 or 3 candidate cells in U, whose candidate cells
//   together are exactly three cells P. Those cells must hold T, so every
//   candidate outside T is eliminated from P. T is never removed.
// Rows, columns and boxes are all checked.
//
// Patterns that are impossible or ambiguous are skipped, never guessed at:
//   naked  : any fourth empty cell whose candidates are also inside T (four
//            cells confined to three digits), or a single-candidate cell
//            inside T, or a union smaller than T;
//   hidden : any fourth digit confined to P, a digit with a single location
//            (a hidden single), or a union of other than three cells.
// The local contradiction checks do not flag these states; the skip is
// sound because no deduction is claimed. A deduction is emitted only if it has
// at least one target.
//
// Evidence (pattern, distinct from the proposed eliminations): unit = source
// unit, cells = the three triple cells ascending, digits = the three triple
// digits. Targets are the exact candidates to remove, ordered by cell then
// digit, each currently present.
//
// Results: DetectResult as for the other techniques (deduction.hpp): Ok (list
// possibly empty), Contradiction (no deductions) or InvalidBoard (Board
// overload).
//
// Order: units rows 0..8, columns 0..8, boxes 0..8; within a unit by triple
// digits ascending (lexicographic). find_triples returns all naked triples,
// then all hidden triples. Deductions are not merged: the same triple seen
// from its row and its box gives two deductions; overlapping eliminations are
// idempotent when applied.
//
// Applying: apply() re-validates against the CURRENT grid (see deduction.hpp).
// Candidates may have shrunk since detection and the triple stays supported
// while the confinement still holds with the same union; a grid that no
// longer shows that pattern gives Unsupported. All-or-nothing: on any failure
// the whole grid is unchanged. A detected list is only a snapshot; re-detect
// after applying.
#pragma once

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"

namespace sudoku {

DetectResult find_naked_triples(const CandidateGrid& grid);
DetectResult find_hidden_triples(const CandidateGrid& grid);
DetectResult find_triples(const CandidateGrid& grid);

// Builds candidates with CandidateGrid::from_board, then find_triples().
DetectResult find_triples(const Board& board);

}  // namespace sudoku
