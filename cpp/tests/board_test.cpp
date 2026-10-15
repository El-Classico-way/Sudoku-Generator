#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "sudoku/board.hpp"

using sudoku::Board;
using sudoku::Conflict;
using sudoku::UnitKind;
using Grid = std::array<int, sudoku::kCellCount>;

namespace {

// A well-known solved grid (the solution of the Wikipedia example puzzle).
// Its validity was checked independently of the Board code (set equality of
// every row, column and box against {1..9}), not by the validator under test.
constexpr Grid kSolved = {
    5, 3, 4, 6, 7, 8, 9, 1, 2,  //
    6, 7, 2, 1, 9, 5, 3, 4, 8,  //
    1, 9, 8, 3, 4, 2, 5, 6, 7,  //
    8, 5, 9, 7, 6, 1, 4, 2, 3,  //
    4, 2, 6, 8, 5, 3, 7, 9, 1,  //
    7, 1, 3, 9, 2, 4, 8, 5, 6,  //
    9, 6, 1, 5, 3, 7, 2, 8, 4,  //
    2, 8, 7, 4, 1, 9, 6, 3, 5,  //
    3, 4, 5, 2, 8, 6, 1, 7, 9};

bool has(const std::vector<Conflict>& v, UnitKind k, int index, int digit) {
  return std::find(v.begin(), v.end(), Conflict{k, index, digit}) != v.end();
}

Board solved_board() { return Board(kSolved); }

void test_construction() {
  Board empty;
  for (int i = 0; i < sudoku::kCellCount; ++i) CHECK(empty.get(i) == 0);
  CHECK(Board(Grid{}) == empty);  // all zeros

  Board solved = solved_board();
  for (int i = 0; i < sudoku::kCellCount; ++i) CHECK(solved.get(i) == kSolved[i]);

  Grid bad = kSolved;
  bad[40] = 10;
  CHECK_THROWS_AS(Board{bad}, std::invalid_argument);
  bad[40] = -1;
  CHECK_THROWS_AS(Board{bad}, std::invalid_argument);
  bad[40] = 256 + 5;  // must not wrap to a valid uint8_t value
  CHECK_THROWS_AS(Board{bad}, std::invalid_argument);

  // Duplicates are structurally valid; the board is just inconsistent.
  Grid dup{};
  dup[0] = 9;
  dup[1] = 9;
  Board b(dup);
  CHECK(!b.is_consistent());
}

void test_cell_access() {
  Board b;
  b.set(0, 0, 1);
  b.set(8, 8, 9);
  b.set(3, 5, 7);
  CHECK(b.get(0, 0) == 1 && b.get(0) == 1);
  CHECK(b.get(8, 8) == 9 && b.get(80) == 9);
  CHECK(b.get(3, 5) == 7);
  CHECK(b.get(3 * 9 + 5) == 7);  // row-major
  CHECK(b.cells()[3 * 9 + 5] == 7);
  b.set(41, 4);
  CHECK(b.get(4, 5) == 4);
  b.set(4, 5, 0);  // clearing is allowed
  CHECK(b.get(4, 5) == 0);

  CHECK_THROWS_AS(b.get(-1, 0), std::out_of_range);
  CHECK_THROWS_AS(b.get(9, 0), std::out_of_range);
  CHECK_THROWS_AS(b.get(0, -1), std::out_of_range);
  CHECK_THROWS_AS(b.get(0, 9), std::out_of_range);
  CHECK_THROWS_AS(b.get(-1), std::out_of_range);
  CHECK_THROWS_AS(b.get(81), std::out_of_range);

  // Rejected writes leave the board unchanged.
  const Board before = b;
  CHECK_THROWS_AS(b.set(9, 0, 1), std::out_of_range);
  CHECK_THROWS_AS(b.set(0, 9, 1), std::out_of_range);
  CHECK_THROWS_AS(b.set(-1, 0, 1), std::out_of_range);
  CHECK_THROWS_AS(b.set(81, 1), std::out_of_range);
  CHECK_THROWS_AS(b.set(-1, 1), std::out_of_range);
  CHECK_THROWS_AS(b.set(0, 0, 10), std::invalid_argument);
  CHECK_THROWS_AS(b.set(0, 0, -1), std::invalid_argument);
  CHECK_THROWS_AS(b.set(0, 10), std::invalid_argument);
  CHECK_THROWS_AS(b.set(80, 256 + 3), std::invalid_argument);
  CHECK(b == before);
}

// Each unit check is run against all nine units so a mix-up of indices or
// unit shapes is caught. `place` puts digits into the unit under test.
void test_row_column_box_checks() {
  const Board empty;
  for (int u = 0; u < 9; ++u) {
    CHECK(!empty.row_has_conflict(u));
    CHECK(!empty.column_has_conflict(u));
    CHECK(!empty.box_has_conflict(u));
  }
  for (int bad : {-1, 9}) {
    CHECK_THROWS_AS(empty.row_has_conflict(bad), std::out_of_range);
    CHECK_THROWS_AS(empty.column_has_conflict(bad), std::out_of_range);
    CHECK_THROWS_AS(empty.box_has_conflict(bad), std::out_of_range);
  }

  // Distinct digits and repeated zeros never conflict.
  Board distinct;
  for (int k = 0; k < 9; ++k) {
    distinct.set(2, k, k + 1);  // row 2 holds 1..9
  }
  CHECK(!distinct.row_has_conflict(2));
  CHECK(!distinct.box_has_conflict(0));  // cols 0-2 of row 2: 1,2,3
  CHECK(distinct.is_consistent());
  Board zeros_only;
  zeros_only.set(0, 0, 5);  // other 8 cells of row 0 are repeated zeros
  CHECK(!zeros_only.row_has_conflict(0));

  // Row conflict: two 7s in row r, columns 0 and 4 (different boxes, and
  // different columns), so only the row is affected.
  for (int r = 0; r < 9; ++r) {
    Board b;
    b.set(r, 0, 7);
    b.set(r, 4, 7);
    for (int u = 0; u < 9; ++u) {
      CHECK(b.row_has_conflict(u) == (u == r));
      CHECK(!b.column_has_conflict(u));
      CHECK(!b.box_has_conflict(u));
    }
    CHECK(!b.is_consistent() && b.row_has_conflict(r));
  }
  // Column conflict: two 7s in column c, rows 0 and 4.
  for (int c = 0; c < 9; ++c) {
    Board b;
    b.set(0, c, 7);
    b.set(4, c, 7);
    for (int u = 0; u < 9; ++u) {
      CHECK(b.column_has_conflict(u) == (u == c));
      CHECK(!b.row_has_conflict(u));
      CHECK(!b.box_has_conflict(u));
    }
  }
  // Box conflict: two 7s at (r0,c0) and (r0+1,c0+1), same box only.
  for (int box = 0; box < 9; ++box) {
    const int r0 = (box / 3) * 3, c0 = (box % 3) * 3;
    Board b;
    b.set(r0, c0, 7);
    b.set(r0 + 1, c0 + 1, 7);
    for (int u = 0; u < 9; ++u) {
      CHECK(b.box_has_conflict(u) == (u == box));
      CHECK(!b.row_has_conflict(u));
      CHECK(!b.column_has_conflict(u));
    }
  }
  // The same digit in nine different rows, columns and boxes is fine:
  // row r gets its 3 in column kColumns[r] (one per row, column and box).
  constexpr int kColumns[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
  Board spread;
  for (int r = 0; r < 9; ++r) spread.set(r, kColumns[r], 3);
  CHECK(spread.is_consistent());
  // A duplicate elsewhere is not attributed to an unrelated box: the two
  // 7s share row 0 across boxes 0 and 1 only.
  Board two_boxes;
  two_boxes.set(0, 2, 7);
  two_boxes.set(0, 3, 7);
  CHECK(!two_boxes.box_has_conflict(0) && !two_boxes.box_has_conflict(1));
  CHECK(two_boxes.row_has_conflict(0));
}

void test_whole_board() {
  const Board empty;
  CHECK(empty.is_consistent());
  CHECK(!empty.is_complete());
  CHECK(!empty.is_valid_solution());
  CHECK(empty.find_conflicts().empty());

  Board partial;
  partial.set(0, 0, 5);
  partial.set(0, 1, 3);
  partial.set(1, 0, 6);
  partial.set(4, 4, 5);
  CHECK(partial.is_consistent());
  CHECK(!partial.is_complete());
  CHECK(!partial.is_valid_solution());  // consistent but incomplete

  // A conflict affecting several units: (0,0) and (1,1) both 4 share box 0.
  // Adding (0,5)=4 also clashes with (0,0) in row 0.
  Board multi;
  multi.set(0, 0, 4);
  multi.set(1, 1, 4);
  multi.set(0, 5, 4);
  CHECK(!multi.is_consistent());
  const auto conflicts = multi.find_conflicts();
  CHECK(conflicts.size() == 2);
  CHECK(has(conflicts, UnitKind::Row, 0, 4));
  CHECK(has(conflicts, UnitKind::Box, 0, 4));
}

void test_solved_grid_and_invalid_complete_grids() {
  const Board solved = solved_board();
  CHECK(solved.is_complete());
  CHECK(solved.is_consistent());
  CHECK(solved.is_valid_solution());
  CHECK(solved.find_conflicts().empty());
  for (int u = 0; u < 9; ++u) {
    CHECK(!solved.row_has_conflict(u));
    CHECK(!solved.column_has_conflict(u));
    CHECK(!solved.box_has_conflict(u));
  }

  // Swapping (0,0)=5 and (0,1)=3 keeps row 0 and box 0 valid but puts a
  // duplicate in each of columns 0 and 1.
  {
    Board b = solved;
    b.set(0, 0, 3);
    b.set(0, 1, 5);
    CHECK(b.is_complete());
    CHECK(!b.is_valid_solution());
    CHECK(!b.row_has_conflict(0));
    CHECK(b.column_has_conflict(0) && b.column_has_conflict(1));
    CHECK(!b.box_has_conflict(0));
  }
  // Replace (0,8)=2 with 1: row 0 now has 1 at columns 7 and 8, and box 2
  // holds both of those cells.
  {
    Board b = solved;
    b.set(0, 8, 1);
    CHECK(!b.is_valid_solution());
    CHECK(b.row_has_conflict(0));
    CHECK(has(b.find_conflicts(), UnitKind::Row, 0, 1));
    CHECK(has(b.find_conflicts(), UnitKind::Box, 2, 1));
  }
  // Incomplete: remove one cell from the valid grid.
  {
    Board b = solved;
    b.set(4, 4, 0);
    CHECK(b.is_consistent());
    CHECK(!b.is_complete());
    CHECK(!b.is_valid_solution());
  }
  // Constant full grid is complete but invalid.
  {
    Grid g;
    g.fill(1);
    Board b(g);
    CHECK(b.is_complete());
    CHECK(!b.is_consistent());
    CHECK(!b.is_valid_solution());
  }
}

// Extreme int arguments must be rejected, not wrapped or truncated.
void test_extreme_arguments() {
  Board b = solved_board();
  const Board before = b;
  constexpr int lo = std::numeric_limits<int>::min();
  constexpr int hi = std::numeric_limits<int>::max();
  for (int bad : {lo, hi}) {
    CHECK_THROWS_AS(b.get(bad), std::out_of_range);
    CHECK_THROWS_AS(b.get(bad, 0), std::out_of_range);
    CHECK_THROWS_AS(b.get(0, bad), std::out_of_range);
    CHECK_THROWS_AS(b.set(bad, 1), std::out_of_range);
    CHECK_THROWS_AS(b.set(0, 0, bad), std::invalid_argument);
    CHECK_THROWS_AS(b.set(bad, 0, 1), std::out_of_range);
    CHECK_THROWS_AS(b.row_has_conflict(bad), std::out_of_range);
    CHECK_THROWS_AS(b.column_has_conflict(bad), std::out_of_range);
    CHECK_THROWS_AS(b.box_has_conflict(bad), std::out_of_range);
  }
  CHECK(b == before);
}

// One missing cell / one wrong cell distinguishes the validation concepts.
void test_near_complete_boards() {
  Board almost = solved_board();
  almost.set(40, 0);
  CHECK(almost.is_consistent() && !almost.is_complete() && !almost.is_valid_solution());
  almost.set(40, kSolved[40]);  // restore: valid again
  CHECK(almost.is_valid_solution());
  almost.set(40, kSolved[40] % 9 + 1);  // complete but wrong digit
  CHECK(almost.is_complete() && !almost.is_consistent() && !almost.is_valid_solution());
  CHECK(!almost.find_conflicts().empty());
}

void test_equality_and_copy() {
  Board a = solved_board();
  Board b = solved_board();
  CHECK(a == b);
  Board copy = a;
  CHECK(copy == a);
  copy.set(0, 0, 0);
  CHECK(copy != a);
  CHECK(a.get(0, 0) == 5);  // original unaffected by mutation of the copy
  Board assigned;
  assigned = a;
  CHECK(assigned == a);
  assigned.set(8, 8, 1);
  CHECK(assigned != a && a.get(8, 8) == 9);
}

}  // namespace

int main() {
  test_construction();
  test_cell_access();
  test_row_column_box_checks();
  test_whole_board();
  test_solved_grid_and_invalid_complete_grids();
  test_extreme_arguments();
  test_near_complete_boards();
  test_equality_and_copy();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
