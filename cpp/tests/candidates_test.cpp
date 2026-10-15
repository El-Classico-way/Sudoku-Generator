#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/candidates.hpp"

using namespace sudoku;

namespace {

Board parse(const std::string& text) {
  std::array<int, kCellCount> v{};
  int n = 0;
  for (char ch : text) {
    if (ch == ' ' || ch == '\n') continue;
    CHECK(n < kCellCount);
    v[static_cast<std::size_t>(n++)] = ch == '.' ? 0 : ch - '0';
  }
  CHECK(n == kCellCount);
  return Board(v);
}

// Wikipedia example puzzle (unique solution; see solver_test.cpp).
const char* kPuzzle =
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1"
    "7...2...6" ".6....28." "...419..5" "....8..79";
const char* kSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791"
    "713924856" "961537284" "287419635" "345286179";
// Row 0 holds 1..8, (1,8)=9: cell (0,8) is left with no candidates.
const char* kDeadCell =
    "12345678." "........9" "........." "........." "........."
    "........." "........." "........." ".........";

CandidateGrid grid_of(const Board& b) {
  CandidateInitResult r = CandidateGrid::from_board(b);
  CHECK(r.status == InitStatus::Ok && r.grid.has_value());
  return *r.grid;
}

std::vector<int> v(std::initializer_list<int> l) { return std::vector<int>(l); }

// ---- DigitSet ----
void test_digit_set() {
  DigitSet s;
  CHECK(s.empty() && s.size() == 0 && s.mask() == 0 && s.digits().empty());
  const DigitSet full = DigitSet::all();
  CHECK(full.size() == 9 && !full.empty() && full.mask() == 0x1FF);
  for (int d = 1; d <= 9; ++d) CHECK(full.contains(d) && !s.contains(d));
  CHECK(full.digits() == v({1, 2, 3, 4, 5, 6, 7, 8, 9}));

  // bit mapping: digit d <-> bit d-1
  CHECK(s.insert(1) && s.mask() == 0x001);
  CHECK(s.insert(9) && s.mask() == 0x101);
  CHECK(s.insert(5) && s.mask() == 0x111);
  CHECK(!s.insert(5));  // repeated insert: no change
  CHECK(s.size() == 3 && s.digits() == v({1, 5, 9}));  // ascending
  CHECK(s.contains(5) && !s.contains(4));  // valid digit, not a member
  CHECK(s.erase(5) && !s.contains(5) && s.mask() == 0x101);
  CHECK(!s.erase(5));  // repeated erase: no change
  CHECK(!s.erase(2));  // never a member: no change
  CHECK(s.mask() == 0x101);

  // Invalid digits are rejected by every operation and change nothing.
  const DigitSet before = s;
  for (int bad : {0, -1, 10, 100, std::numeric_limits<int>::min(),
                  std::numeric_limits<int>::max()}) {
    CHECK_THROWS_AS(s.contains(bad), std::invalid_argument);
    CHECK_THROWS_AS(s.insert(bad), std::invalid_argument);
    CHECK_THROWS_AS(s.erase(bad), std::invalid_argument);
  }
  CHECK(s == before);

  CHECK(DigitSet::from_mask(0x1FF) == full);
  CHECK(DigitSet::from_mask(0) == DigitSet{});
  CHECK(DigitSet::from_mask(0x101).digits() == v({1, 9}));
  CHECK_THROWS_AS(DigitSet::from_mask(0x200), std::invalid_argument);
  CHECK_THROWS_AS(DigitSet::from_mask(0xFFFFFFFFu), std::invalid_argument);
}

// ---- Units ----
void test_units() {
  // Rows / columns / boxes of boundary and interior cells.
  struct Case { int cell, row, col, box; };
  for (Case c : {Case{0, 0, 0, 0}, Case{8, 0, 8, 2}, Case{40, 4, 4, 4},
                 Case{72, 8, 0, 6}, Case{80, 8, 8, 8}, Case{30, 3, 3, 4},
                 Case{26, 2, 8, 2}}) {
    CHECK(cell_row(c.cell) == c.row);
    CHECK(cell_column(c.cell) == c.col);
    CHECK(cell_box(c.cell) == c.box);
    CHECK((unit_of(UnitKind::Row, c.cell) == Unit{UnitKind::Row, c.row}));
    CHECK((unit_of(UnitKind::Column, c.cell) == Unit{UnitKind::Column, c.col}));
    CHECK((unit_of(UnitKind::Box, c.cell) == Unit{UnitKind::Box, c.box}));
  }
  CHECK((unit_cells({UnitKind::Row, 0}) == std::array<int, 9>{0, 1, 2, 3, 4, 5, 6, 7, 8}));
  CHECK((unit_cells({UnitKind::Column, 8}) == std::array<int, 9>{8, 17, 26, 35, 44, 53, 62, 71, 80}));
  CHECK((unit_cells({UnitKind::Box, 0}) == std::array<int, 9>{0, 1, 2, 9, 10, 11, 18, 19, 20}));
  CHECK((unit_cells({UnitKind::Box, 8}) == std::array<int, 9>{60, 61, 62, 69, 70, 71, 78, 79, 80}));
  CHECK((unit_cells({UnitKind::Box, 4}) == std::array<int, 9>{30, 31, 32, 39, 40, 41, 48, 49, 50}));

  // Every unit: nine distinct cells, ascending, all belonging to that unit,
  // and every cell is in exactly one row, one column and one box.
  std::array<int, kCellCount> seen_row{}, seen_col{}, seen_box{};
  for (int u = 0; u < 9; ++u) {
    for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
      const auto cells = unit_cells({kind, u});
      CHECK(std::is_sorted(cells.begin(), cells.end()));
      CHECK(std::adjacent_find(cells.begin(), cells.end()) == cells.end());
      for (int c : cells) {
        CHECK((unit_of(kind, c) == Unit{kind, u}));
        auto& seen = kind == UnitKind::Row ? seen_row : kind == UnitKind::Column ? seen_col : seen_box;
        ++seen[static_cast<std::size_t>(c)];
      }
    }
  }
  for (int c = 0; c < kCellCount; ++c) {
    CHECK(seen_row[static_cast<std::size_t>(c)] == 1 && seen_col[static_cast<std::size_t>(c)] == 1 && seen_box[static_cast<std::size_t>(c)] == 1);
  }
  // Agrees with Board's own box convention.
  Board b;
  b.set(cell_row(30) , cell_column(30), 7);
  b.set(unit_cells({UnitKind::Box, 4})[8], 7);  // cell 50, same box
  CHECK(b.box_has_conflict(4) && cell_box(30) == cell_box(50));

  for (int bad : {-1, 81}) {
    CHECK_THROWS_AS(cell_row(bad), std::out_of_range);
    CHECK_THROWS_AS(cell_column(bad), std::out_of_range);
    CHECK_THROWS_AS(cell_box(bad), std::out_of_range);
    CHECK_THROWS_AS(unit_of(UnitKind::Row, bad), std::out_of_range);
  }
  for (int bad : {-1, 9}) {
    for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
      CHECK_THROWS_AS(unit_cells({kind, bad}), std::out_of_range);
    }
  }
}

// ---- Initialization ----
// Independent expected set via the pairwise reference.
DigitSet expected_candidates(const Board& b, int cell) {
  DigitSet s;
  if (b.get(cell) != 0) return s;
  ref::Grid g{};
  for (int i = 0; i < 81; ++i) g[static_cast<std::size_t>(i)] = b.get(i);
  for (int d = 1; d <= 9; ++d) if (ref::placeable(g, cell, d)) s.insert(d);
  return s;
}

void test_initialization() {
  // Empty board: every cell has all nine.
  const CandidateGrid empty = grid_of(Board{});
  for (int c = 0; c < 81; ++c) CHECK(empty.candidates(c) == DigitSet::all());
  CHECK(empty.invariants_hold() && !empty.has_contradiction());

  // Solved board: nothing has candidates; no contradiction.
  const Board solved = parse(kSolved);
  const CandidateGrid sg = grid_of(solved);
  for (int c = 0; c < 81; ++c) {
    CHECK(sg.candidates(c).empty());
    CHECK(!sg.has_no_candidates(c));  // filled, so not a contradiction
  }
  CHECK(!sg.has_contradiction() && sg.invariants_hold());

  // Partial puzzle: every cell matches the independent reference.
  const Board puzzle = parse(kPuzzle);
  const Board puzzle_before = puzzle;
  const CandidateGrid pg = grid_of(puzzle);
  CHECK(puzzle == puzzle_before);  // input untouched
  for (int c = 0; c < 81; ++c) {
    CHECK(pg.candidates(c) == expected_candidates(puzzle, c));
    if (puzzle.get(c) != 0) CHECK(pg.candidate_count(c) == 0);
  }
  CHECK(pg.invariants_hold() && !pg.has_contradiction());
  // Cell (0,2): row has 5,3,7; column has 8; box has 5,3,6,9,8 -> {1,2,4}.
  CHECK(pg.candidates(0, 2).digits() == v({1, 2, 4}));
  CHECK(pg.candidates(2) == pg.candidates(0, 2));

  // Restriction by exactly one unit kind.
  Board row_only;  row_only.set(0, 0, 5);   // affects (0,8): row only
  Board col_only;  col_only.set(0, 0, 5);   // affects (8,0): column only
  Board box_only;  box_only.set(0, 0, 5);   // affects (2,2): box only
  CHECK(!grid_of(row_only).is_candidate(8, 5));
  CHECK(grid_of(row_only).candidates(8).size() == 8);
  CHECK(!grid_of(col_only).is_candidate(72, 5));
  CHECK(!grid_of(box_only).is_candidate(20, 5));
  // ...and a cell sharing no unit with (0,0) is unaffected.
  CHECK(grid_of(box_only).candidates(40) == DigitSet::all());
  // Several units at once: (4,4) sees 1 in its row, 2 in column, 3 in box.
  Board multi;
  multi.set(4, 0, 1); multi.set(0, 4, 2); multi.set(3, 3, 3);
  CHECK(grid_of(multi).candidates(4, 4).digits() == v({4, 5, 6, 7, 8, 9}));

  // The snapshot is a copy: changing the original Board does not alter it.
  Board mutable_board = puzzle;
  const CandidateGrid snap = grid_of(mutable_board);
  mutable_board.set(0, 2, 4);
  CHECK(snap.board() == puzzle && !snap.matches(mutable_board));
  CHECK(snap.matches(puzzle));
  CHECK(snap.candidates(0, 2).digits() == v({1, 2, 4}));  // not recomputed
}

void test_initialization_failure() {
  Board row, col, box;
  row.set(0, 0, 5); row.set(0, 8, 5);
  col.set(0, 0, 5); col.set(8, 0, 5);
  box.set(0, 0, 5); box.set(2, 2, 5);
  for (const Board& b : {row, col, box}) {
    const Board before = b;
    const CandidateInitResult r = CandidateGrid::from_board(b);
    CHECK(r.status == InitStatus::InconsistentBoard);
    CHECK(!r.grid.has_value());
    CHECK(r.conflicts == b.find_conflicts() && !r.conflicts.empty());
    CHECK(b == before);
  }
  // Out-of-range values cannot exist in a Board (rejected at construction).
  std::array<int, kCellCount> bad{};
  bad[3] = 10;
  CHECK_THROWS_AS(Board{bad}, std::invalid_argument);

  // Consistent but locally dead: initialization succeeds and the problem is
  // visible through the contradiction API; a board that is consistent but
  // unsolvable is NOT rejected (no search is done).
  const CandidateGrid dead = grid_of(parse(kDeadCell));
  CHECK(dead.candidates(8).empty() && dead.has_no_candidates(8));
}

// ---- Queries and elimination ----
void test_queries_and_elimination() {
  CandidateGrid g = grid_of(parse(kPuzzle));  // (0,2) -> {1,2,4}; (0,0) filled
  for (int bad : {-1, 81}) {
    CHECK_THROWS_AS(g.candidates(bad), std::out_of_range);
    CHECK_THROWS_AS(g.is_candidate(bad, 1), std::out_of_range);
    CHECK_THROWS_AS(g.candidate_count(bad), std::out_of_range);
    CHECK_THROWS_AS(g.has_no_candidates(bad), std::out_of_range);
    CHECK_THROWS_AS(g.eliminate(bad, 1), std::out_of_range);
  }
  CHECK_THROWS_AS(g.candidates(9, 0), std::out_of_range);
  CHECK_THROWS_AS(g.candidates(0, -1), std::out_of_range);
  for (int bad : {0, 10, -3}) {
    CHECK_THROWS_AS(g.is_candidate(2, bad), std::invalid_argument);
    CHECK_THROWS_AS(g.eliminate(2, bad), std::invalid_argument);
    CHECK_THROWS_AS(g.cells_with_candidate({UnitKind::Row, 0}, bad), std::invalid_argument);
  }
  CHECK_THROWS_AS(g.cells_with_candidate({UnitKind::Row, 9}, 1), std::out_of_range);
  CHECK(g.is_candidate(2, 1) && !g.is_candidate(2, 5));  // 5 valid, not a candidate

  const CandidateGrid before = g;
  CHECK(g == before);

  // Filled cell: rejected, unchanged.
  CHECK(g.eliminate(0, 5) == EliminateOutcome::FilledCell);
  CHECK(g.eliminate(0, 1) == EliminateOutcome::FilledCell);
  CHECK(g == before);
  // Valid no-op: digit already absent.
  CHECK(g.eliminate(2, 5) == EliminateOutcome::AlreadyAbsent);
  CHECK(g == before);

  // Real elimination, repeated elimination, and the last-candidate guard.
  CHECK(g.eliminate(2, 1) == EliminateOutcome::Eliminated);
  CHECK(g.candidates(2).digits() == v({2, 4}));
  CHECK(g.eliminate(2, 1) == EliminateOutcome::AlreadyAbsent);
  CHECK(g.eliminate(2, 2) == EliminateOutcome::Eliminated);
  CHECK(g.candidates(2).digits() == v({4}));
  const CandidateGrid at_last = g;
  CHECK(g.eliminate(2, 4) == EliminateOutcome::WouldEmptyCell);
  CHECK(g == at_last);                       // untouched
  CHECK(g.candidates(2).digits() == v({4}));
  CHECK(!g.has_no_candidates(2) && !g.has_contradiction());
  CHECK(g.invariants_hold());
  CHECK(g.board() == before.board());        // Board never changes

  // Eliminations only ever remove: every set is a subset of its initial set.
  for (int c = 0; c < 81; ++c) {
    CHECK((g.candidates(c).mask() & ~before.candidates(c).mask()) == 0);
  }
}

void test_cells_with_candidate() {
  const CandidateGrid g = grid_of(parse(kPuzzle));
  // Row 0: ". . 4 6 . 8 9 1 2" candidates; digit 4 can go in (0,2) only?
  // Compute expectations from the reference rather than by hand.
  const Board puzzle = parse(kPuzzle);
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < 9; ++u) {
      for (int d = 1; d <= 9; ++d) {
        std::vector<int> expect;
        for (int cell : unit_cells({kind, u})) {
          if (puzzle.get(cell) == 0 && expected_candidates(puzzle, cell).contains(d)) {
            expect.push_back(cell);
          }
        }
        CHECK(g.cells_with_candidate({kind, u}, d) == expect);  // ascending
      }
    }
  }
  // A digit already placed in the unit has no location in it.
  CHECK(g.cells_with_candidate({UnitKind::Row, 0}, 5).empty());
}

// find_contradictions was optimized to one pass per unit (M22). This is the original
// definition, written out naively, as an independent oracle.
std::vector<Contradiction> naive_contradictions(const CandidateGrid& g, const Board& board) {
  std::vector<Contradiction> out;
  for (int cell = 0; cell < kCellCount; ++cell) {
    if (g.has_no_candidates(cell)) out.push_back({ContradictionKind::EmptyCellNoCandidates, cell, Unit{UnitKind::Row, -1}, 0});
  }
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < kBoardSize; ++u) {
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        bool placed = false, possible = false;
        for (int cell : unit_cells({kind, u})) {
          if (board.get(cell) == d) placed = true;
          if (g.candidates(cell).contains(d)) possible = true;
        }
        if (!placed && !possible) out.push_back({ContradictionKind::DigitHasNoLocation, -1, Unit{kind, u}, d});
      }
    }
  }
  return out;
}

void test_contradictions_match_the_naive_definition() {
  std::uint64_t state = 0x123456789abcdefull;
  auto next = [&](int bound) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<int>((state >> 33) % static_cast<std::uint64_t>(bound));
  };
  const Board solved = parse(kSolved);
  int nonempty = 0;
  for (int round = 0; round < 400; ++round) {
    Board board = solved;
    const int blanks = next(70);
    for (int k = 0; k < blanks; ++k) board.set(next(kCellCount), 0);
    CandidateGrid g = grid_of(board);
    const int eliminations = next(400);
    for (int k = 0; k < eliminations; ++k) g.eliminate(next(kCellCount), 1 + next(9));
    const auto fast = g.find_contradictions();
    CHECK(fast == naive_contradictions(g, board));
    if (!fast.empty()) ++nonempty;
  }
  CHECK(nonempty > 20);  // the sweep really reaches contradictory states
}

// ---- Contradictions ----
void test_contradictions() {
  // Empty cell with no candidates (and the knock-on missing location).
  const CandidateGrid dead = grid_of(parse(kDeadCell));
  const auto found = dead.find_contradictions();
  CHECK(found.size() == 2);
  if (found.size() == 2) {
    CHECK(found[0].kind == ContradictionKind::EmptyCellNoCandidates && found[0].cell == 8);
    CHECK(found[1].kind == ContradictionKind::DigitHasNoLocation);
    CHECK((found[1].unit == Unit{UnitKind::Row, 0}) && found[1].digit == 9);
  }
  CHECK(dead.has_contradiction());

  // Missing location in each of the 27 units, with no empty-cell
  // contradiction: strip digit 5 from every cell of one unit via eliminate().
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < 9; ++u) {
      CandidateGrid g = grid_of(Board{});
      for (int cell : unit_cells({kind, u})) {
        CHECK(g.eliminate(cell, 5) == EliminateOutcome::Eliminated);
      }
      const auto cs = g.find_contradictions();
      CHECK(cs.size() == 1);
      if (cs.size() == 1) {
        CHECK(cs[0].kind == ContradictionKind::DigitHasNoLocation);
        CHECK((cs[0].unit == Unit{kind, u}) && cs[0].digit == 5 && cs[0].cell == -1);
      }
      CHECK(g.invariants_hold());
    }
  }

  // Two independent missing locations are both reported, in documented order
  // (rows, then columns, then boxes).
  {
    CandidateGrid g = grid_of(Board{});
    for (int cell : unit_cells({UnitKind::Box, 8})) g.eliminate(cell, 2);
    for (int cell : unit_cells({UnitKind::Row, 3})) g.eliminate(cell, 7);
    const auto cs = g.find_contradictions();
    // Box 8 / digit 2 also empties no row or column of digit 2 (they keep other
    // cells), so exactly these two:
    CHECK(cs.size() == 2);
    if (cs.size() == 2) {
      CHECK((cs[0].unit == Unit{UnitKind::Row, 3}) && cs[0].digit == 7);
      CHECK((cs[1].unit == Unit{UnitKind::Box, 8}) && cs[1].digit == 2);
    }
  }

  // No contradiction: unit already contains the digit; unit with a location;
  // a completed solution; plain valid partial boards.
  CHECK(!grid_of(parse(kPuzzle)).has_contradiction());
  CHECK(!grid_of(parse(kSolved)).has_contradiction());
  Board placed;  placed.set(0, 0, 5);
  const CandidateGrid pg = grid_of(placed);
  CHECK(!pg.has_contradiction());               // row 0 holds 5: no location needed
  CHECK(pg.cells_with_candidate({UnitKind::Row, 0}, 5).empty());
  // Reducing a candidate set is not itself a contradiction.
  CandidateGrid reduced = grid_of(Board{});
  for (int d = 1; d <= 8; ++d) reduced.eliminate(0, d);
  CHECK(reduced.candidates(0).digits() == v({9}) && !reduced.has_contradiction());
}

// Invariants hold under any sequence of valid operations.
void test_invariants_under_operations() {
  CandidateGrid g = grid_of(parse(kPuzzle));
  ref::Rng rng(11);
  for (int i = 0; i < 3000; ++i) {
    const int cell = rng.below(81), digit = 1 + rng.below(9);
    const DigitSet before = g.candidates(cell);
    const auto out = g.eliminate(cell, digit);
    CHECK((g.candidates(cell).mask() & ~before.mask()) == 0);  // never grows
    if (out == EliminateOutcome::Eliminated) CHECK(g.candidates(cell).size() == before.size() - 1);
    else CHECK(g.candidates(cell) == before);
    CHECK(g.invariants_hold());
    CHECK(!g.has_no_candidates(cell));
  }
  CHECK(g.board() == parse(kPuzzle));
  for (int c = 0; c < 81; ++c) {
    if (g.board().get(c) == 0) CHECK(g.candidate_count(c) >= 1);
  }
}

}  // namespace

int main() {
  test_digit_set();
  test_units();
  test_initialization();
  test_initialization_failure();
  test_queries_and_elimination();
  test_cells_with_candidate();
  test_contradictions();
  test_contradictions_match_the_naive_definition();
  test_invariants_under_operations();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
