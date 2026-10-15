// Locked candidates: pointing and claiming. Detection is pure: it inspects a
// CandidateGrid and returns proposed elimination Deductions; nothing is
// modified. Apply them with apply() (deduction.hpp), which re-validates the
// evidence. No search, guessing or randomness is used.
//
// Pointing (Technique::Pointing): for a box B and digit d not placed in B,
//   let P = the empty cells of B that have d as a candidate (P is non-empty
//   in a healthy state). If every cell of P lies in one row R (or one column
//   C), then d cannot be a candidate of any other cell of R (C). The
//   deduction eliminates d from the cells of R (C) OUTSIDE B that currently
//   have d as a candidate. Cells inside B are never targeted.
// Claiming (Technique::Claiming): for a row or column L and digit d not
//   placed in L, let P = the empty cells of L with d as a candidate. If every
//   cell of P lies in one box B, then d cannot be a candidate of any other
//   cell of B. The deduction eliminates d from the cells of B OUTSIDE L that
//   currently have d as a candidate.
// Filled cells are never targets (they have no candidates). A deduction with
// no target is never emitted. If P has a single cell it is aligned in both
// row and column, so pointing yields up to two deductions (row, then column);
// for claiming a single cell yields the one for its box.
//
// Evidence on every deduction: unit = source unit (the box for pointing, the
// row/column for claiming), target_unit = the line (pointing) or box
// (claiming) that holds the targets, cells = P (the candidate positions that
// establish the alignment), digits = {d}; targets are the (cell, d) pairs.
// P is the full set of positions checked, not a subset.
//
// Results: DetectResult as for singles (deduction.hpp): Ok with a possibly
// empty list; Contradiction (no deductions) when the grid has a local
// contradiction, including a digit missing from a unit with no location
// (never treated as a locked candidate); InvalidBoard for the Board overload.
//
// Order: find_pointing: source box 0..8, digit 1..9, then row before column;
// find_claiming: rows 0..8 then columns 0..8, digit 1..9; targets ascending.
// find_locked_candidates: all pointing, then all claiming.
//
// Duplicates: results are technique-specific and not merged. Two deductions
// may remove the same candidate from the same cell (each keeps its own
// evidence); application is idempotent per candidate, so the second one
// reports NoChange.
//
// A detected list is a snapshot. Applying one deduction can make later ones
// partly or wholly redundant; re-detect after applying.
#pragma once

#include "sudoku/board.hpp"
#include "sudoku/candidates.hpp"
#include "sudoku/deduction.hpp"

namespace sudoku {

DetectResult find_pointing(const CandidateGrid& grid);
DetectResult find_claiming(const CandidateGrid& grid);
DetectResult find_locked_candidates(const CandidateGrid& grid);

// Builds candidates with CandidateGrid::from_board, then
// find_locked_candidates().
DetectResult find_locked_candidates(const Board& board);

}  // namespace sudoku
