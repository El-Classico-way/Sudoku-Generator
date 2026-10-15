#include "sudoku/board.hpp"

#include <stdexcept>
#include <string>

namespace sudoku {
namespace {

void check_index(int value, int limit, const char* what) {
  if (value < 0 || value >= limit) {
    throw std::out_of_range(std::string(what) + " out of range");
  }
}

void check_value(int value) {
  if (value < kEmptyCell || value > kMaxDigit) {
    throw std::invalid_argument("cell value must be 0..9");
  }
}

// Cell index of the k-th cell (0..8) of a unit.
int row_cell(int row, int k) { return row * kBoardSize + k; }
int column_cell(int column, int k) { return k * kBoardSize + column; }
int box_cell(int box, int k) {
  const int top = (box / kBoxSize) * kBoxSize;
  const int left = (box % kBoxSize) * kBoxSize;
  return (top + k / kBoxSize) * kBoardSize + left + k % kBoxSize;
}

// Bitmask (bit d set => digit d appears more than once) for one unit.
template <typename CellOf>
unsigned duplicate_digits(const Board::Cells& cells, int unit, CellOf cell_of) {
  unsigned seen = 0;
  unsigned dup = 0;
  for (int k = 0; k < kBoardSize; ++k) {
    const unsigned digit = cells[static_cast<std::size_t>(cell_of(unit, k))];
    if (digit == kEmptyCell) continue;
    const unsigned bit = 1u << digit;
    dup |= seen & bit;
    seen |= bit;
  }
  return dup;
}

}  // namespace

Board::Board(const std::array<int, kCellCount>& values) {
  for (int v : values) check_value(v);  // validate everything before storing
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    cells_[i] = static_cast<std::uint8_t>(values[i]);
  }
}

int Board::get(int row, int column) const {
  check_index(row, kBoardSize, "row");
  check_index(column, kBoardSize, "column");
  return cells_[static_cast<std::size_t>(row_cell(row, column))];
}

int Board::get(int index) const {
  check_index(index, kCellCount, "index");
  return cells_[static_cast<std::size_t>(index)];
}

void Board::set(int row, int column, int value) {
  check_index(row, kBoardSize, "row");
  check_index(column, kBoardSize, "column");
  check_value(value);
  cells_[static_cast<std::size_t>(row_cell(row, column))] =
      static_cast<std::uint8_t>(value);
}

void Board::set(int index, int value) {
  check_index(index, kCellCount, "index");
  check_value(value);
  cells_[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
}

bool Board::is_complete() const noexcept {
  for (auto c : cells_) {
    if (c == kEmptyCell) return false;
  }
  return true;
}

bool Board::row_has_conflict(int row) const {
  check_index(row, kBoardSize, "row");
  return duplicate_digits(cells_, row, row_cell) != 0;
}

bool Board::column_has_conflict(int column) const {
  check_index(column, kBoardSize, "column");
  return duplicate_digits(cells_, column, column_cell) != 0;
}

bool Board::box_has_conflict(int box) const {
  check_index(box, kBoardSize, "box");
  return duplicate_digits(cells_, box, box_cell) != 0;
}

bool Board::is_consistent() const noexcept {
  for (int u = 0; u < kBoardSize; ++u) {
    if (duplicate_digits(cells_, u, row_cell) != 0 ||
        duplicate_digits(cells_, u, column_cell) != 0 ||
        duplicate_digits(cells_, u, box_cell) != 0) {
      return false;
    }
  }
  return true;
}

bool Board::is_valid_solution() const noexcept {
  return is_complete() && is_consistent();
}

std::vector<Conflict> Board::find_conflicts() const {
  std::vector<Conflict> out;
  auto scan = [&](UnitKind kind, int (*cell_of)(int, int)) {
    for (int u = 0; u < kBoardSize; ++u) {
      const unsigned dup = duplicate_digits(cells_, u, cell_of);
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        if (dup & (1u << d)) out.push_back({kind, u, d});
      }
    }
  };
  scan(UnitKind::Row, row_cell);
  scan(UnitKind::Column, column_cell);
  scan(UnitKind::Box, box_cell);
  return out;
}

}  // namespace sudoku
