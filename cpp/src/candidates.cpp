#include "sudoku/candidates.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace sudoku {
namespace {

void check_cell(int cell) {
  if (cell < 0 || cell >= kCellCount) throw std::out_of_range("cell out of range");
}
void check_unit_index(int index) {
  if (index < 0 || index >= kBoardSize) throw std::out_of_range("unit index out of range");
}
void check_digit(int digit) {
  if (digit < kMinDigit || digit > kMaxDigit) {
    throw std::invalid_argument("digit must be 1..9");
  }
}
constexpr unsigned kFullMask = (1u << kBoardSize) - 1u;

}  // namespace

int cell_row(int cell) { check_cell(cell); return cell / kBoardSize; }
int cell_column(int cell) { check_cell(cell); return cell % kBoardSize; }
int cell_box(int cell) {
  check_cell(cell);
  return (cell / kBoardSize / kBoxSize) * kBoxSize + (cell % kBoardSize) / kBoxSize;
}

Unit unit_of(UnitKind kind, int cell) {
  switch (kind) {
    case UnitKind::Row: return {kind, cell_row(cell)};
    case UnitKind::Column: return {kind, cell_column(cell)};
    case UnitKind::Box: return {kind, cell_box(cell)};
  }
  throw std::invalid_argument("unknown unit kind");
}

std::array<int, kBoardSize> unit_cells(const Unit& unit) {
  check_unit_index(unit.index);
  std::array<int, kBoardSize> out{};
  for (int k = 0; k < kBoardSize; ++k) {
    switch (unit.kind) {
      case UnitKind::Row: out[static_cast<std::size_t>(k)] = unit.index * kBoardSize + k; break;
      case UnitKind::Column: out[static_cast<std::size_t>(k)] = k * kBoardSize + unit.index; break;
      case UnitKind::Box: {
        const int top = (unit.index / kBoxSize) * kBoxSize;
        const int left = (unit.index % kBoxSize) * kBoxSize;
        out[static_cast<std::size_t>(k)] = (top + k / kBoxSize) * kBoardSize + left + k % kBoxSize;
        break;
      }
    }
  }
  return out;
}

// ---- DigitSet ----------------------------------------------------------

DigitSet DigitSet::all() noexcept {
  DigitSet s;
  s.bits_ = static_cast<std::uint16_t>(kFullMask);
  return s;
}

DigitSet DigitSet::from_mask(unsigned mask) {
  if ((mask & ~kFullMask) != 0) throw std::invalid_argument("mask has bits above digit 9");
  DigitSet s;
  s.bits_ = static_cast<std::uint16_t>(mask);
  return s;
}

bool DigitSet::contains(int digit) const {
  check_digit(digit);
  return (bits_ >> (digit - 1)) & 1u;
}

bool DigitSet::insert(int digit) {
  check_digit(digit);
  const auto before = bits_;
  bits_ = static_cast<std::uint16_t>(bits_ | (1u << (digit - 1)));
  return bits_ != before;
}

bool DigitSet::erase(int digit) {
  check_digit(digit);
  const auto before = bits_;
  bits_ = static_cast<std::uint16_t>(bits_ & ~(1u << (digit - 1)));
  return bits_ != before;
}

int DigitSet::size() const noexcept { return std::popcount(static_cast<unsigned>(bits_)); }

std::vector<int> DigitSet::digits() const {
  std::vector<int> out;
  for (int d = kMinDigit; d <= kMaxDigit; ++d) {
    if ((bits_ >> (d - 1)) & 1u) out.push_back(d);
  }
  return out;
}

// ---- CandidateGrid -----------------------------------------------------

CandidateInitResult CandidateGrid::from_board(const Board& board) {
  CandidateInitResult result;
  if (!board.is_consistent()) {
    result.status = InitStatus::InconsistentBoard;
    result.conflicts = board.find_conflicts();
    return result;
  }
  CandidateGrid grid;
  grid.board_ = board;
  for (int cell = 0; cell < kCellCount; ++cell) {
    if (board.get(cell) != kEmptyCell) continue;  // filled: no candidates
    DigitSet set = DigitSet::all();
    for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
      for (int peer : unit_cells(unit_of(kind, cell))) {
        const int v = board.get(peer);
        if (v != kEmptyCell) set.erase(v);
      }
    }
    grid.cells_[static_cast<std::size_t>(cell)] = set;
  }
  result.status = InitStatus::Ok;
  result.grid = std::move(grid);
  return result;
}

DigitSet CandidateGrid::candidates(int cell) const {
  check_cell(cell);
  return cells_[static_cast<std::size_t>(cell)];
}

DigitSet CandidateGrid::candidates(int row, int column) const {
  if (row < 0 || row >= kBoardSize) throw std::out_of_range("row out of range");
  if (column < 0 || column >= kBoardSize) throw std::out_of_range("column out of range");
  return cells_[static_cast<std::size_t>(row * kBoardSize + column)];
}

bool CandidateGrid::is_candidate(int cell, int digit) const {
  check_cell(cell);
  return cells_[static_cast<std::size_t>(cell)].contains(digit);
}

int CandidateGrid::candidate_count(int cell) const {
  return candidates(cell).size();
}

bool CandidateGrid::has_no_candidates(int cell) const {
  check_cell(cell);
  return board_.get(cell) == kEmptyCell && cells_[static_cast<std::size_t>(cell)].empty();
}

std::vector<int> CandidateGrid::cells_with_candidate(const Unit& unit, int digit) const {
  check_digit(digit);
  std::vector<int> out;
  for (int cell : unit_cells(unit)) {
    if (cells_[static_cast<std::size_t>(cell)].contains(digit)) out.push_back(cell);
  }
  return out;  // filled cells have no candidates, so only empty cells appear
}

EliminateOutcome CandidateGrid::eliminate(int cell, int digit) {
  check_cell(cell);
  check_digit(digit);
  if (board_.get(cell) != kEmptyCell) return EliminateOutcome::FilledCell;
  DigitSet& set = cells_[static_cast<std::size_t>(cell)];
  if (!set.contains(digit)) return EliminateOutcome::AlreadyAbsent;
  if (set.size() == 1) return EliminateOutcome::WouldEmptyCell;
  set.erase(digit);
  return EliminateOutcome::Eliminated;
}

PlaceOutcome CandidateGrid::place(int cell, int digit) {
  check_cell(cell);
  check_digit(digit);
  if (board_.get(cell) != kEmptyCell) return PlaceOutcome::FilledCell;
  if (!cells_[static_cast<std::size_t>(cell)].contains(digit)) return PlaceOutcome::NotACandidate;
  const UnitKind kinds[] = {UnitKind::Row, UnitKind::Column, UnitKind::Box};
  for (UnitKind kind : kinds) {  // check everything before changing anything
    for (int peer : unit_cells(unit_of(kind, cell))) {
      if (peer == cell || board_.get(peer) != kEmptyCell) continue;
      const DigitSet& set = cells_[static_cast<std::size_t>(peer)];
      if (set.size() == 1 && set.contains(digit)) return PlaceOutcome::WouldEmptyPeer;
    }
  }
  board_.set(cell, digit);
  cells_[static_cast<std::size_t>(cell)] = DigitSet{};
  for (UnitKind kind : kinds) {
    for (int peer : unit_cells(unit_of(kind, cell))) {
      if (peer != cell) cells_[static_cast<std::size_t>(peer)].erase(digit);
    }
  }
  return PlaceOutcome::Placed;
}

std::vector<Contradiction> CandidateGrid::find_contradictions() const {
  std::vector<Contradiction> out;
  for (int cell = 0; cell < kCellCount; ++cell) {
    if (has_no_candidates(cell)) {
      out.push_back({ContradictionKind::EmptyCellNoCandidates, cell, Unit{UnitKind::Row, -1}, 0});
    }
  }
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit unit{kind, u};
      // One pass over the unit: digits already placed, and digits still possible somewhere.
      unsigned covered = 0;
      for (int cell : unit_cells(unit)) {
        const int value = board_.get(cell);
        if (value != kEmptyCell) covered |= 1u << (value - kMinDigit);
        covered |= cells_[static_cast<std::size_t>(cell)].mask();
      }
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        if ((covered & (1u << (d - kMinDigit))) == 0) {
          out.push_back({ContradictionKind::DigitHasNoLocation, -1, unit, d});
        }
      }
    }
  }
  return out;
}

bool CandidateGrid::invariants_hold() const {
  for (int cell = 0; cell < kCellCount; ++cell) {
    const DigitSet set = cells_[static_cast<std::size_t>(cell)];
    if (board_.get(cell) != kEmptyCell) {
      if (!set.empty()) return false;
      continue;
    }
    for (int d : set.digits()) {
      for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
        for (int peer : unit_cells(unit_of(kind, cell))) {
          if (board_.get(peer) == d) return false;
        }
      }
    }
  }
  return true;
}

}  // namespace sudoku
