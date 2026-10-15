#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/locked.hpp"
#include "sudoku/singles.hpp"

using namespace sudoku;

namespace {

Board parse(const std::string& text) {
  std::array<int, kCellCount> v{};
  int n = 0;
  for (char ch : text) {
    if (ch == ' ') continue;
    v[static_cast<std::size_t>(n++)] = ch == '.' ? 0 : ch - '0';
  }
  CHECK(n == kCellCount);
  return Board(v);
}

const char* kPuzzle =  // Wikipedia example (solution below)
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1"
    "7...2...6" ".6....28." "...419..5" "....8..79";
const char* kSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791"
    "713924856" "961537284" "287419635" "345286179";

CandidateGrid grid_of(const Board& b) {
  CandidateInitResult r = CandidateGrid::from_board(b);
  CHECK(r.status == InitStatus::Ok);
  return *r.grid;
}
CandidateGrid empty_grid() { return grid_of(Board{}); }

using Cells = std::vector<int>;
Cells cells(std::initializer_list<int> l) { return Cells(l); }

// Remove `digit` from every cell in `which` (all must currently hold it).
void strip(CandidateGrid& g, const Cells& which, int digit) {
  for (int c : which) CHECK(g.eliminate(c, digit) == EliminateOutcome::Eliminated);
}
Cells box_cells_except(int box, const Cells& keep) {
  Cells out;
  for (int c : unit_cells({UnitKind::Box, box})) {
    if (std::find(keep.begin(), keep.end(), c) == keep.end()) out.push_back(c);
  }
  return out;
}
Cells line_cells_except(UnitKind kind, int idx, const Cells& keep) {
  Cells out;
  for (int c : unit_cells({kind, idx})) {
    if (std::find(keep.begin(), keep.end(), c) == keep.end()) out.push_back(c);
  }
  return out;
}

Cells target_cells(const Deduction& d) {
  Cells out;
  for (const Elimination& e : d.targets()) out.push_back(e.cell);
  return out;
}

// Check one deduction's shape and evidence.
void check_deduction(const Deduction& d, Technique tech, Unit source, Unit target,
                     int digit, const Cells& positions, const Cells& targets,
                     const std::string& ctx) {
  CHECK_CTX(d.technique() == tech, ctx);
  CHECK_CTX(d.action() == DeductionAction::EliminateCandidates, ctx);
  CHECK_CTX(d.evidence().unit == std::optional<Unit>(source), ctx);
  CHECK_CTX(d.evidence().target_unit == std::optional<Unit>(target), ctx);
  CHECK_CTX(d.evidence().cells == positions, ctx);
  CHECK_CTX(d.evidence().digits.digits() == std::vector<int>{digit}, ctx);
  CHECK_CTX(target_cells(d) == targets, ctx);
  for (const Elimination& e : d.targets()) CHECK_CTX(e.digit == digit, ctx);
  CHECK_CTX(!d.explanation().empty(), ctx);
}

// ---------- pointing ----------
void test_pointing_row_and_column() {
  // Box 0, digit 5 confined to row 0 (3 positions): targets row 0 cols 3..8.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({0, 1, 2})), 5);
    const DetectResult r = find_locked_candidates(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 1);
    if (r.deductions.size() == 1)
      check_deduction(r.deductions[0], Technique::Pointing, {UnitKind::Box, 0},
                      {UnitKind::Row, 0}, 5, cells({0, 1, 2}),
                      cells({3, 4, 5, 6, 7, 8}), "3 in row");
  }
  // Two positions in row 0.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({0, 1})), 5);
    const auto r = find_pointing(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Pointing, {UnitKind::Box, 0}, {UnitKind::Row, 0},
                      5, cells({0, 1}), cells({3, 4, 5, 6, 7, 8}), "2 in row");
  }
  // Three positions in column 0 (cells 0, 9, 18): targets rows 3..8.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({0, 9, 18})), 5);
    const auto r = find_pointing(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Pointing, {UnitKind::Box, 0}, {UnitKind::Column, 0},
                      5, cells({0, 9, 18}), cells({27, 36, 45, 54, 63, 72}), "3 in column");
  }
  // Two positions in column 2 of box 0 (cells 2, 11).
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({2, 11})), 5);
    const auto r = find_pointing(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Pointing, {UnitKind::Box, 0}, {UnitKind::Column, 2},
                      5, cells({2, 11}), cells({29, 38, 47, 56, 65, 74}), "2 in column");
  }
  // A middle box, to exercise non-origin geometry: box 4, digit 3 in row 4.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(4, cells({39, 40, 41})), 3);
    const auto r = find_pointing(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Pointing, {UnitKind::Box, 4}, {UnitKind::Row, 4},
                      3, cells({39, 40, 41}), cells({36, 37, 38, 42, 43, 44}), "box 4");
  }
}

void test_pointing_single_position_and_negatives() {
  // One position: aligned in both its row and its column -> two deductions,
  // row first. Targets lie outside the box only.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({10})), 5);
    const auto r = find_pointing(g).deductions;
    CHECK(r.size() == 2);
    if (r.size() == 2) {
      check_deduction(r[0], Technique::Pointing, {UnitKind::Box, 0}, {UnitKind::Row, 1}, 5,
                      cells({10}), cells({12, 13, 14, 15, 16, 17}), "single/row");
      check_deduction(r[1], Technique::Pointing, {UnitKind::Box, 0}, {UnitKind::Column, 1}, 5,
                      cells({10}), cells({28, 37, 46, 55, 64, 73}), "single/column");
    }
  }
  // Positions spread over several rows and columns: nothing.
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({0, 10})), 5);
    const DetectResult r = find_pointing(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.empty());
  }
  // Aligned, but no target holds the digit any more: no pointing. (Claiming
  // finds nothing either: once the digit is confined in the box, no other cell
  // of the box has it.)
  {
    CandidateGrid g = empty_grid();
    strip(g, box_cells_except(0, cells({0, 1, 2})), 5);
    strip(g, cells({3, 4, 5, 6, 7, 8}), 5);
    const DetectResult all = find_locked_candidates(g);
    CHECK(all.status == DetectStatus::Ok && all.deductions.empty());
  }
}

// ---------- claiming ----------
void test_claiming() {
  // Row 0, digit 5 confined to box 0 (3 positions): targets rows 1,2 of box 0.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Row, 0, cells({0, 1, 2})), 5);
    const DetectResult r = find_locked_candidates(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 1);
    if (r.deductions.size() == 1)
      check_deduction(r.deductions[0], Technique::Claiming, {UnitKind::Row, 0},
                      {UnitKind::Box, 0}, 5, cells({0, 1, 2}),
                      cells({9, 10, 11, 18, 19, 20}), "row 3");
  }
  // Two positions.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Row, 0, cells({0, 1})), 5);
    const auto r = find_claiming(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Claiming, {UnitKind::Row, 0}, {UnitKind::Box, 0}, 5,
                      cells({0, 1}), cells({9, 10, 11, 18, 19, 20}), "row 2");
  }
  // Column 0 confined to box 0, three and two positions.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Column, 0, cells({0, 9, 18})), 5);
    const auto r = find_claiming(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Claiming, {UnitKind::Column, 0}, {UnitKind::Box, 0}, 5,
                      cells({0, 9, 18}), cells({1, 2, 10, 11, 19, 20}), "col 3");
    CandidateGrid h = empty_grid();
    strip(h, line_cells_except(UnitKind::Column, 0, cells({9, 18})), 5);
    const auto r2 = find_claiming(h).deductions;
    CHECK(r2.size() == 1);
    if (r2.size() == 1) CHECK(r2[0].evidence().cells == cells({9, 18}));
  }
  // Lower-right geometry: column 8 confined to box 8.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Column, 8, cells({62, 71, 80})), 9);
    const auto r = find_claiming(g).deductions;
    CHECK(r.size() == 1);
    if (r.size() == 1)
      check_deduction(r[0], Technique::Claiming, {UnitKind::Column, 8}, {UnitKind::Box, 8}, 9,
                      cells({62, 71, 80}), cells({60, 61, 69, 70, 78, 79}), "col 8");
  }
  // Spanning two boxes: no claiming.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Row, 0, cells({0, 3})), 5);
    const DetectResult r = find_claiming(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.empty());
  }
  // Confined, but the box has no other cell with the digit: no deduction.
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Row, 0, cells({0, 1, 2})), 5);
    strip(g, cells({9, 10, 11, 18, 19, 20}), 5);
    CHECK(find_claiming(g).deductions.empty());
  }
}

void test_placed_digits_and_contradictions() {
  // Digit already placed in the unit: never a locked candidate there.
  Board placed;
  placed.set(0, 0, 5);
  const CandidateGrid g = grid_of(placed);
  const DetectResult r = find_locked_candidates(g);
  CHECK(r.status == DetectStatus::Ok);
  for (const Deduction& d : r.deductions) {
    const int digit = d.evidence().digits.digits().front();
    for (int c : unit_cells(*d.evidence().unit)) CHECK(placed.get(c) != digit);
  }
  // Placed in a source row / column / box explicitly.
  for (const Unit& u : {Unit{UnitKind::Row, 0}, Unit{UnitKind::Column, 0}, Unit{UnitKind::Box, 0}}) {
    for (const Deduction& d : r.deductions) {
      CHECK(!(d.evidence().unit == std::optional<Unit>(u) && d.evidence().digits.contains(5)));
    }
  }

  // Missing digit with no location in a row / column / box: Contradiction,
  // never a locked candidate.
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    CandidateGrid c = empty_grid();
    const auto members = unit_cells({kind, 4});
    strip(c, Cells(members.begin(), members.end()), 5);
    for (const DetectResult& d : {find_pointing(c), find_claiming(c), find_locked_candidates(c)}) {
      CHECK(d.status == DetectStatus::Contradiction && d.deductions.empty());
      CHECK(d.contradictions.size() == 1);
      if (d.contradictions.size() == 1) {
        CHECK(d.contradictions[0].kind == ContradictionKind::DigitHasNoLocation);
        CHECK((d.contradictions[0].unit == Unit{kind, 4}) && d.contradictions[0].digit == 5);
      }
    }
  }
  // Empty cell with no candidates.
  const DetectResult dead = find_locked_candidates(parse(
      "12345678." "........9" "........." "........." "........." "........." "........." "........." "........."));
  CHECK(dead.status == DetectStatus::Contradiction && dead.deductions.empty());
  CHECK(!dead.contradictions.empty());
  // Duplicate digits in the Board.
  Board bad;
  bad.set(0, 0, 4);  bad.set(4, 0, 4);
  const DetectResult inv = find_locked_candidates(bad);
  CHECK(inv.status == DetectStatus::InvalidBoard && inv.deductions.empty() && !inv.conflicts.empty());
  // A healthy state with no locked candidates is Ok + empty.
  const DetectResult none = find_locked_candidates(Board{});
  CHECK(none.status == DetectStatus::Ok && none.deductions.empty());
}

void test_multiple_and_ordering() {
  CandidateGrid g = empty_grid();
  strip(g, box_cells_except(8, cells({62, 71, 80})), 7);          // pointing: box 8 digit 7 -> col 8
  strip(g, box_cells_except(0, cells({0, 1, 2})), 5);             // pointing: box 0 digit 5 -> row 0
  strip(g, line_cells_except(UnitKind::Row, 4, cells({36, 37, 38})), 2);   // claiming: row 4 digit 2 -> box 3
  strip(g, line_cells_except(UnitKind::Column, 6, cells({6, 15, 24})), 1); // claiming: col 6 digit 1 -> box 2
  const DetectResult r = find_locked_candidates(g);
  CHECK(r.status == DetectStatus::Ok);
  // Pointing first (box 0, then box 8), then claiming (rows before columns).
  std::vector<std::pair<Technique, Unit>> got;
  for (const Deduction& d : r.deductions) got.push_back({d.technique(), *d.evidence().unit});
  const std::vector<std::pair<Technique, Unit>> want = {
      {Technique::Pointing, {UnitKind::Box, 0}}, {Technique::Pointing, {UnitKind::Box, 8}},
      {Technique::Claiming, {UnitKind::Row, 4}}, {Technique::Claiming, {UnitKind::Column, 6}}};
  CHECK(got == want);
  CHECK(find_pointing(g).deductions.size() == 2 && find_claiming(g).deductions.size() == 2);
  for (const Deduction& d : r.deductions) {
    const Cells t = target_cells(d);
    CHECK(!t.empty() && std::is_sorted(t.begin(), t.end()));
  }
}

// ---------- independent reference ----------
struct RefDed { Technique t; Unit source, target; int digit; Cells positions, targets; };

std::vector<RefDed> reference_locked(const Board& b) {
  ref::Grid g{};
  for (int i = 0; i < 81; ++i) g[static_cast<std::size_t>(i)] = b.get(i);
  auto row = [](int c) { return c / 9; };
  auto col = [](int c) { return c % 9; };
  auto box = [](int c) { return (c / 9 / 3) * 3 + (c % 9) / 3; };
  auto cand = [&](int c, int d) { return g[static_cast<std::size_t>(c)] == 0 && ref::placeable(g, c, d); };
  auto placed_in = [&](int kind, int u, int d) {
    for (int c = 0; c < 81; ++c) {
      const int mine = kind == 0 ? row(c) : kind == 1 ? col(c) : box(c);
      if (mine == u && g[static_cast<std::size_t>(c)] == d) return true;
    }
    return false;
  };
  auto uk = [](int kind) { return kind == 0 ? UnitKind::Row : kind == 1 ? UnitKind::Column : UnitKind::Box; };
  std::vector<RefDed> out;
  for (int bx = 0; bx < 9; ++bx) {
    for (int d = 1; d <= 9; ++d) {
      if (placed_in(2, bx, d)) continue;
      Cells pos;
      for (int c = 0; c < 81; ++c) if (box(c) == bx && cand(c, d)) pos.push_back(c);
      for (int kind = 0; kind < 2; ++kind) {  // row, then column
        auto line = [&](int c) { return kind == 0 ? row(c) : col(c); };
        if (pos.empty() || !std::all_of(pos.begin(), pos.end(), [&](int c) { return line(c) == line(pos[0]); })) continue;
        Cells tg;
        for (int c = 0; c < 81; ++c) if (line(c) == line(pos[0]) && box(c) != bx && cand(c, d)) tg.push_back(c);
        if (!tg.empty()) out.push_back({Technique::Pointing, {UnitKind::Box, bx}, {uk(kind), line(pos[0])}, d, pos, tg});
      }
    }
  }
  for (int kind = 0; kind < 2; ++kind) {
    for (int u = 0; u < 9; ++u) {
      for (int d = 1; d <= 9; ++d) {
        if (placed_in(kind, u, d)) continue;
        Cells pos;
        for (int c = 0; c < 81; ++c) if ((kind == 0 ? row(c) : col(c)) == u && cand(c, d)) pos.push_back(c);
        if (pos.empty() || !std::all_of(pos.begin(), pos.end(), [&](int c) { return box(c) == box(pos[0]); })) continue;
        Cells tg;
        for (int c = 0; c < 81; ++c) if (box(c) == box(pos[0]) && (kind == 0 ? row(c) : col(c)) != u && cand(c, d)) tg.push_back(c);
        if (!tg.empty()) out.push_back({Technique::Claiming, {uk(kind), u}, {UnitKind::Box, box(pos[0])}, d, pos, tg});
      }
    }
  }
  return out;
}

void check_against_reference(const Board& b, const std::string& ctx) {
  const DetectResult r = find_locked_candidates(b);
  CHECK_CTX(r.status == DetectStatus::Ok, ctx);
  const auto want = reference_locked(b);
  CHECK_CTX(r.deductions.size() == want.size(), ctx);
  for (std::size_t i = 0; i < std::min(want.size(), r.deductions.size()); ++i) {
    check_deduction(r.deductions[i], want[i].t, want[i].source, want[i].target,
                    want[i].digit, want[i].positions, want[i].targets, ctx);
  }
}

// Eliminations are sound: the true digit is never removed from a cell.
void test_reference_and_soundness() {
  check_against_reference(parse(kPuzzle), "wikipedia");
  const Board solution = parse(kSolved);
  ref::Rng rng(5);
  int total = 0, pointing = 0, claiming = 0;
  for (int trial = 0; trial < 150; ++trial) {
    Board b = solution;
    const int blanks = 30 + rng.below(35);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    const std::string ctx = "trial " + std::to_string(trial);
    check_against_reference(b, ctx);
    for (const Deduction& d : find_locked_candidates(b).deductions) {
      ++total;
      (d.technique() == Technique::Pointing ? pointing : claiming)++;
      for (const Elimination& e : d.targets()) {
        CHECK_CTX(e.digit != solution.get(e.cell), ctx);  // never the true digit
        CHECK_CTX(b.get(e.cell) == 0, ctx);
      }
    }
  }
  CHECK(total > 100 && pointing > 20 && claiming > 20);
  std::printf("locked sweep: %d deductions (%d pointing, %d claiming)\n", total, pointing, claiming);
}

void test_purity_and_determinism() {
  const Board board = parse(kPuzzle);
  const CandidateGrid grid = grid_of(board);
  const CandidateGrid before = grid;
  const Board board_before = board;
  const DetectResult a = find_locked_candidates(grid);
  const DetectResult b = find_locked_candidates(grid);
  const DetectResult c = find_locked_candidates(board);
  CHECK(a.deductions == b.deductions && a.deductions == c.deductions);
  CHECK(grid == before && board == board_before);
  (void)find_pointing(grid);
  (void)find_claiming(grid);
  CHECK(grid == before);
}

// ---------- application ----------
CandidateGrid pointing_grid() {  // digit 5 in box 0 confined to row 0
  CandidateGrid g = empty_grid();
  strip(g, box_cells_except(0, cells({0, 1, 2})), 5);
  return g;
}

void test_apply_pointing_and_claiming() {
  {
    CandidateGrid g = pointing_grid();
    const Deduction d = find_pointing(g).deductions.at(0);
    const CandidateGrid before = g;
    const AppliedDeduction a = apply(g, d);
    CHECK(a.status == ApplyStatus::Applied);
    CHECK((a.effective == std::vector<Elimination>{{3, 5}, {4, 5}, {5, 5}, {6, 5}, {7, 5}, {8, 5}}));
    for (int c = 0; c < 81; ++c) {
      DigitSet want = before.candidates(c);
      if (c >= 3 && c <= 8) want.erase(5);
      CHECK(g.candidates(c) == want);  // only 5 from the targets changed
    }
    CHECK(g.board() == before.board() && g.invariants_hold());
    CHECK(g.cells_with_candidate({UnitKind::Row, 0}, 5) == cells({0, 1, 2}));
    // Repeating is a documented no-op, and detection no longer proposes it.
    const CandidateGrid after = g;
    CHECK(apply(g, d).status == ApplyStatus::NoChange && g == after);
    CHECK(find_pointing(g).deductions.empty());
  }
  {
    CandidateGrid g = empty_grid();
    strip(g, line_cells_except(UnitKind::Column, 3, cells({3, 12, 21})), 8);
    const auto found = find_claiming(g).deductions;
    CHECK(found.size() == 1);
    if (found.size() != 1) return;
    const CandidateGrid before = g;
    const AppliedDeduction a = apply(g, found[0]);
    CHECK(a.status == ApplyStatus::Applied);
    CHECK(a.effective.size() == 6);
    for (const Elimination& e : a.effective) {
      CHECK(e.digit == 8 && cell_box(e.cell) == 1 && cell_column(e.cell) != 3);
    }
    CHECK(g.board() == before.board() && g.invariants_hold());
    CHECK(g.cells_with_candidate({UnitKind::Box, 1}, 8) == cells({3, 12, 21}));
    CHECK(find_claiming(g).deductions.empty());
  }
}

void test_apply_revalidation_and_failures() {
  const CandidateGrid base = pointing_grid();
  const Deduction good = find_pointing(base).deductions.at(0);
  auto expect = [&](const CandidateGrid& start, const Deduction& d, ApplyStatus want, const char* what) {
    CandidateGrid g = start;
    const AppliedDeduction a = apply(g, d);
    CHECK_CTX(a.status == want, what);
    if (want != ApplyStatus::Applied) {
      CHECK_CTX(g == start, what);  // nothing changed
      CHECK_CTX(a.effective.empty(), what);
    }
  };
  // Foreign state: positions in the box are not confined to row 0 here.
  expect(empty_grid(), good, ApplyStatus::Unsupported, "foreign grid");
  // Candidate positions may shrink after detection and remain supported.
  {
    CandidateGrid g = base;
    g.eliminate(1, 5);
    CHECK(apply(g, good).status == ApplyStatus::Applied);
    CHECK(g.cells_with_candidate({UnitKind::Row, 0}, 5) == cells({0, 2}));
  }
  // Targets already absent are skipped; the others are still removed.
  {
    CandidateGrid g = base;
    g.eliminate(4, 5);
    const AppliedDeduction a = apply(g, good);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == 5);
  }
  // Digit now placed in the source box: unsupported.
  {
    Board b;
    b.set(10, 5);
    CandidateGrid g = grid_of(b);
    expect(g, good, ApplyStatus::Unsupported, "digit placed in source");
  }
  // Malformed evidence.
  Evidence ev = good.evidence();
  ev.target_unit.reset();
  expect(base, Deduction::eliminate(Technique::Pointing, good.targets(), ev, "x"), ApplyStatus::Unsupported, "no target unit");
  ev = good.evidence();  ev.unit = Unit{UnitKind::Row, 0};  // source of the wrong kind
  expect(base, Deduction::eliminate(Technique::Pointing, good.targets(), ev, "x"), ApplyStatus::Unsupported, "wrong source kind");
  ev = good.evidence();  ev.digits.insert(6);               // two digits in evidence
  expect(base, Deduction::eliminate(Technique::Pointing, good.targets(), ev, "x"), ApplyStatus::Unsupported, "two digits");
  expect(base, Deduction::eliminate(Technique::Claiming, good.targets(), good.evidence(), "x"), ApplyStatus::Unsupported, "technique/shape mismatch");
  // Targets outside the target unit / inside the source box / wrong digit.
  expect(base, Deduction::eliminate(Technique::Pointing, {{12, 5}}, good.evidence(), "x"), ApplyStatus::Unsupported, "target off line");
  expect(base, Deduction::eliminate(Technique::Pointing, {{3, 5}, {2, 5}}, good.evidence(), "x"), ApplyStatus::Unsupported, "target inside box");
  expect(base, Deduction::eliminate(Technique::Pointing, {{3, 4}}, good.evidence(), "x"), ApplyStatus::Unsupported, "wrong digit");
  // Invalid indices and digits cannot even be constructed.
  CHECK_THROWS_AS(Deduction::eliminate(Technique::Pointing, {{81, 5}}, good.evidence(), "x"), std::out_of_range);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::Pointing, {{3, 0}}, good.evidence(), "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::Pointing, {{3, 10}}, good.evidence(), "x"), std::invalid_argument);
  // Filled target cell: rejected, nothing changes.
  {
    Board b;
    b.set(8, 7);  // cell 8 holds 7 (a target of the hand-made deduction)
    CandidateGrid g = grid_of(b);
    strip(g, box_cells_except(0, cells({0, 1, 2})), 5);
    const Deduction d = find_pointing(g).deductions.at(0);
    CHECK(d.targets().size() == 5);                       // detection skips the filled cell
    const Deduction forced = Deduction::eliminate(Technique::Pointing, {{3, 5}, {8, 5}}, d.evidence(), "x");
    expect(g, forced, ApplyStatus::Rejected, "filled target");
  }
  // Would empty a cell: refused, and atomic (the earlier valid target stays).
  {
    CandidateGrid g = base;
    for (int d = 1; d <= 9; ++d) if (d != 5) g.eliminate(7, d);  // cell 7 -> {5}
    CHECK(g.candidates(7).digits() == std::vector<int>{5});
    const Deduction d = find_pointing(g).deductions.at(0);
    CHECK(d.targets().size() == 6);                       // cell 7 is a legal target
    expect(g, d, ApplyStatus::WouldContradict, "last candidate");
  }
}

// Test-side loop (not a production solver): apply every detected single and
// locked-candidate deduction one at a time; the true digit must never be
// eliminated and every application must be valid.
void test_applying_deductions_is_sound() {
  const Board solution = parse(kSolved);
  ref::Rng rng(21);
  int locked_applied = 0;
  for (int trial = 0; trial < 60; ++trial) {
    Board b = solution;
    const int blanks = 35 + rng.below(30);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    const std::string ctx = "trial " + std::to_string(trial);
    CandidateGrid g = grid_of(b);
    for (int step = 0; step < 400; ++step) {
      DetectResult r = find_locked_candidates(g);  // locked first, then singles
      CHECK_CTX(r.status == DetectStatus::Ok, ctx);
      const DetectResult singles = find_singles(g);
      CHECK_CTX(singles.status == DetectStatus::Ok, ctx);
      r.deductions.insert(r.deductions.end(), singles.deductions.begin(), singles.deductions.end());
      bool progressed = false;
      for (const Deduction& d : r.deductions) {
        const AppliedDeduction a = apply(g, d);
        CHECK_CTX(a.status == ApplyStatus::Applied || a.status == ApplyStatus::NoChange, ctx);
        if (a.status == ApplyStatus::Applied) {
          progressed = true;
          if (d.technique() == Technique::Pointing || d.technique() == Technique::Claiming) ++locked_applied;
          break;
        }
      }
      CHECK_CTX(g.invariants_hold() && !g.has_contradiction(), ctx);
      for (int c = 0; c < 81; ++c) {  // the true digit survives everywhere
        if (g.board().get(c) == 0) CHECK_CTX(g.is_candidate(c, solution.get(c)), ctx);
        else CHECK_CTX(g.board().get(c) == solution.get(c), ctx);
      }
      if (!progressed) break;
    }
  }
  CHECK(locked_applied > 20);
}

}  // namespace

int main() {
  test_pointing_row_and_column();
  test_pointing_single_position_and_negatives();
  test_claiming();
  test_placed_digits_and_contradictions();
  test_multiple_and_ordering();
  test_reference_and_soundness();
  test_purity_and_determinism();
  test_apply_pointing_and_claiming();
  test_apply_revalidation_and_failures();
  test_applying_deductions_is_sound();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
