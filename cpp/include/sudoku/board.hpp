// 9x9 Sudoku board representation and validation.
//
// Storage and indexing
//   A Board holds exactly 81 cells in a std::array<std::uint8_t, 81>,
//   row-major: index = row * 9 + column. Rows and columns are 0-based.
//
// Cell values
//   0 means empty; 1..9 are Sudoku digits. Nothing else can be stored:
//   every constructor and mutator rejects other values before modifying
//   the board (strong exception guarantee).
//
// Subgrids (3x3 boxes)
//   Identified by a box index 0..8 in row-major order of the boxes:
//   box = (row / 3) * 3 + (column / 3). Box 0 is top-left, box 2 top-right,
//   box 6 bottom-left, box 8 bottom-right.
//
// Three distinct notions (a Board may hold any of them or none):
//   structurally valid : every cell is 0..9 (always true for a Board).
//   consistent         : no nonzero digit repeats within a row, column or
//                        box. Empty cells never conflict. This says nothing
//                        about whether the board is solvable.
//   valid solution     : complete (no empty cells) and consistent.
//
// Errors
//   Invalid arguments throw; there is no silent fallback and no reliance on
//   assertions. Bad row/column/index/box -> std::out_of_range. Bad cell value
//   -> std::invalid_argument. Arguments are plain int so negative values are
//   detected rather than wrapped by an unsigned conversion.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace sudoku {

inline constexpr int kBoxSize = 3;
inline constexpr int kBoardSize = kBoxSize * kBoxSize;  // rows, columns, boxes
inline constexpr int kCellCount = kBoardSize * kBoardSize;
inline constexpr int kEmptyCell = 0;
inline constexpr int kMinDigit = 1;
inline constexpr int kMaxDigit = kBoardSize;

enum class UnitKind { Row, Column, Box };

// A digit that appears more than once in one unit.
struct Conflict {
  UnitKind unit;
  int index;  // row, column or box index, 0..8
  int digit;  // 1..9
  friend bool operator==(const Conflict&, const Conflict&) = default;
};

class Board {
 public:
  using Cells = std::array<std::uint8_t, kCellCount>;

  // Empty board: all 81 cells are 0.
  Board() = default;

  // Throws std::invalid_argument if any value is outside 0..9. The input
  // is row-major (see header comment). Duplicates are allowed.
  explicit Board(const std::array<int, kCellCount>& values);

  // Reads. Throw std::out_of_range for coordinates outside 0..8 / 0..80.
  int get(int row, int column) const;
  int get(int index) const;

  // Writes. Throw std::out_of_range for bad coordinates and
  // std::invalid_argument for a value outside 0..9. On throw the board is
  // unchanged. Writing a conflicting digit is allowed; use the checks below.
  void set(int row, int column, int value);
  void set(int index, int value);

  // Read-only view of the storage, row-major.
  const Cells& cells() const noexcept { return cells_; }

  // True iff no cell is empty.
  bool is_complete() const noexcept;

  // True iff the given row/column/box has a repeated nonzero digit.
  // Throw std::out_of_range if the index is outside 0..8.
  bool row_has_conflict(int row) const;
  bool column_has_conflict(int column) const;
  bool box_has_conflict(int box) const;

  // True iff no row, column or box has a repeated nonzero digit.
  bool is_consistent() const noexcept;

  // True iff complete and consistent.
  bool is_valid_solution() const noexcept;

  // Every (unit, digit) pair with a repeated digit, ordered rows, then
  // columns, then boxes, each by ascending index and digit. One underlying
  // clash is typically reported in several units (e.g. two equal digits in
  // the same row and the same box give a row entry and a box entry).
  // Empty iff is_consistent().
  std::vector<Conflict> find_conflicts() const;

  friend bool operator==(const Board&, const Board&) = default;

 private:
  Cells cells_{};
};

}  // namespace sudoku
