// Candidate model: which digits may still go in each empty cell.
//
// Three separate things (do not conflate them):
//   * Board values      : what is placed (sudoku/board.hpp).
//   * Candidate state   : CandidateGrid below, a pencil-mark layer that only
//                         ever shrinks after initialization.
//   * Deductions        : descriptions of changes (sudoku/deduction.hpp).
// The Board does not track original givens, so nothing here can tell a given
// from a digit placed later; a filled cell is only "filled".
//
// Conventions (shared with Board): cells are indexed 0..80 row-major, digits
// are 1..9, 0 is never a candidate. Units are rows, columns and 3x3 boxes
// (UnitKind from board.hpp); box = (row/3)*3 + column/3.
//
// Errors: invalid cell/unit indices throw std::out_of_range; invalid digits
// throw std::invalid_argument. A *valid* request that cannot be performed is
// reported through return values (see EliminateOutcome), never by throwing.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "sudoku/board.hpp"

namespace sudoku {

// ---- Units -----------------------------------------------------------

struct Unit {
  UnitKind kind;
  int index;  // 0..8
  friend bool operator==(const Unit&, const Unit&) = default;
};

// Row / column / box (0..8) of a cell index 0..80. Throw std::out_of_range.
int cell_row(int cell);
int cell_column(int cell);
int cell_box(int cell);
// The unit of the given kind that contains `cell`.
Unit unit_of(UnitKind kind, int cell);
// The nine cells of a unit in ascending cell-index order (row-major).
// Throws std::out_of_range if unit.index is outside 0..8.
std::array<int, kBoardSize> unit_cells(const Unit& unit);

// ---- DigitSet --------------------------------------------------------

// A set of digits 1..9 stored in nine bits: digit d <-> bit (d - 1). Cheap to
// copy and compare. Invalid digits are rejected, never ignored; "not in the
// set" and "not a digit" are different outcomes.
class DigitSet {
 public:
  DigitSet() = default;  // empty
  static DigitSet all() noexcept;
  // Bit (d-1) of `mask` <-> digit d. Throws std::invalid_argument if bits
  // above bit 8 are set.
  static DigitSet from_mask(unsigned mask);

  bool contains(int digit) const;  // invalid digit: std::invalid_argument
  // insert/erase return true iff the set changed. Invalid digit throws.
  bool insert(int digit);
  bool erase(int digit);

  int size() const noexcept;
  bool empty() const noexcept { return bits_ == 0; }
  unsigned mask() const noexcept { return bits_; }
  // Members in ascending order.
  std::vector<int> digits() const;

  friend bool operator==(const DigitSet&, const DigitSet&) = default;

 private:
  std::uint16_t bits_ = 0;
};

// ---- Candidate state -------------------------------------------------

enum class InitStatus {
  Ok,
  InconsistentBoard,  // repeated digit in a unit; no candidate state exists
};

struct CandidateInitResult;

enum class EliminateOutcome {
  Eliminated,     // the digit was a candidate and has been removed
  AlreadyAbsent,  // valid no-op: the cell is empty, digit was not a candidate
  FilledCell,     // rejected: the cell holds a value; nothing changed
  WouldEmptyCell, // rejected: it is the cell's last candidate; nothing changed
};

enum class PlaceOutcome {
  Placed,         // digit written to the snapshot, candidates updated
  FilledCell,     // rejected: the cell already holds a value; nothing changed
  NotACandidate,  // rejected: digit is not a current candidate of the cell
  WouldEmptyPeer, // rejected: a peer cell (same row/column/box) has the digit
                  // as its only candidate, so placing it would leave that
                  // cell with none; nothing changed
};

enum class ContradictionKind {
  EmptyCellNoCandidates,  // an empty cell has no candidates
  DigitHasNoLocation,     // a unit lacks the digit and no empty cell in it can
                          // take it
};

struct Contradiction {
  ContradictionKind kind;
  int cell = -1;                      // EmptyCellNoCandidates: the cell
  Unit unit{UnitKind::Row, -1};       // DigitHasNoLocation: the unit
  int digit = 0;                      // DigitHasNoLocation: the digit
  friend bool operator==(const Contradiction&, const Contradiction&) = default;
};

// Candidates for every cell of one Board snapshot.
//
// Association with a Board: the grid stores its own COPY of the Board it was
// built from (board()). Later changes to the caller's Board do not affect it
// and are not reflected in it; matches(b) tells whether a given Board is
// still equal to the snapshot. The snapshot changes only through place(),
// which updates the Board copy and the candidates together, so the two cannot
// drift apart.
//
// Invariants (checked by invariants_hold()):
//   1. a filled cell has no candidates;
//   2. no candidate of an empty cell equals a value already placed in that
//      cell's row, column or box in the snapshot;
//   3. every candidate is a digit 1..9 (guaranteed by DigitSet).
// After initialization the only mutations are eliminate() and place(); both
// only remove candidates, so candidates are never introduced.
class CandidateGrid {
 public:
  // Builds candidates from a Board. Every empty cell gets the digits absent
  // from its row, column and box; filled cells get none. Does not search and
  // does not check solvability. A consistent board always initializes, even
  // if some cell already has no candidates (see find_contradictions()).
  static CandidateInitResult from_board(const Board& board);

  const Board& board() const noexcept { return board_; }
  bool matches(const Board& other) const noexcept { return board_ == other; }

  // Queries. Cell outside 0..80: std::out_of_range; digit outside 1..9:
  // std::invalid_argument.
  DigitSet candidates(int cell) const;
  DigitSet candidates(int row, int column) const;
  bool is_candidate(int cell, int digit) const;
  int candidate_count(int cell) const;
  // True iff the cell is empty AND has no candidates (a contradiction).
  bool has_no_candidates(int cell) const;

  // Empty cells of `unit` that have `digit` as a candidate, ascending.
  std::vector<int> cells_with_candidate(const Unit& unit, int digit) const;

  // Removes `digit` from `cell`'s candidates. Throws only for invalid
  // arguments. Rejected outcomes (FilledCell, WouldEmptyCell) leave the grid
  // untouched, so elimination can never create an empty candidate set;
  // a deduction that would is reported to the caller instead.
  EliminateOutcome eliminate(int cell, int digit);

  // Writes `digit` into the snapshot at `cell` and removes it from the
  // candidates of every peer (same row, column or box); the cell itself ends
  // with no candidates. Atomic: any outcome other than Placed leaves the grid
  // exactly as it was. Invalid cell/digit throw as elsewhere. Like
  // eliminate(), it refuses to create a new empty candidate set, so a
  // placement that would contradict (WouldEmptyPeer) is reported instead of
  // applied. Candidates removed earlier are never restored.
  PlaceOutcome place(int cell, int digit);

  // Local contradictions, in this order: empty cells ascending, then rows,
  // columns, boxes by index, each by ascending digit. A DigitHasNoLocation is
  // reported only when the unit's cells do not already contain the digit and
  // no empty cell of the unit has it as a candidate. This is a LOCAL check:
  // an empty result does not prove the puzzle is solvable.
  std::vector<Contradiction> find_contradictions() const;
  bool has_contradiction() const { return !find_contradictions().empty(); }

  bool invariants_hold() const;

  friend bool operator==(const CandidateGrid&, const CandidateGrid&) = default;

 private:
  CandidateGrid() = default;
  Board board_;
  std::array<DigitSet, kCellCount> cells_{};
};


struct CandidateInitResult {
  InitStatus status = InitStatus::InconsistentBoard;
  std::optional<CandidateGrid> grid;  // engaged iff status == Ok
  std::vector<Conflict> conflicts;    // Board::find_conflicts() on failure
};

}  // namespace sudoku
