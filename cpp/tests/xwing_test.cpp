#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/locked.hpp"
#include "sudoku/pairs.hpp"
#include "sudoku/singles.hpp"
#include "sudoku/triples.hpp"
#include "sudoku/xwing.hpp"

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

const char* kSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791"
    "713924856" "961537284" "287419635" "345286179";

using Cells = std::vector<int>;
using Digits = std::vector<int>;
using Targets = std::vector<Elimination>;

CandidateGrid grid_of(const Board& b) {
  CandidateInitResult r = CandidateGrid::from_board(b);
  CHECK(r.status == InitStatus::Ok);
  return *r.grid;
}
CandidateGrid empty_grid() { return grid_of(Board{}); }

// Remove `digit` from every listed cell that still has it.
void strip(CandidateGrid& g, const Cells& cells, int digit) {
  for (int c : cells) g.eliminate(c, digit);
}
Cells unit_cells_vec(const Unit& u) { auto a = unit_cells(u); return Cells(a.begin(), a.end()); }
Cells row_cells(int r) { auto a = unit_cells({UnitKind::Row, r}); return Cells(a.begin(), a.end()); }
Cells col_cells(int c) { auto a = unit_cells({UnitKind::Column, c}); return Cells(a.begin(), a.end()); }
Cells except(Cells all, const Cells& keep) {
  all.erase(std::remove_if(all.begin(), all.end(), [&](int c) {
    return std::find(keep.begin(), keep.end(), c) != keep.end(); }), all.end());
  return all;
}

// Row-oriented X-Wing for `digit`: base rows r1, r2 keep the digit only in
// columns c1, c2.
void make_row_xwing(CandidateGrid& g, int digit, int r1, int r2, int c1, int c2) {
  for (int r : {r1, r2}) strip(g, except(row_cells(r), {r * 9 + c1, r * 9 + c2}), digit);
}
// Column-oriented: base columns c1, c2 keep the digit only in rows r1, r2.
void make_col_xwing(CandidateGrid& g, int digit, int c1, int c2, int r1, int r2) {
  for (int c : {c1, c2}) strip(g, except(col_cells(c), {r1 * 9 + c, r2 * 9 + c}), digit);
}

void check_deduction(const Deduction& d, const Unit& first, const Unit& second, int digit,
                     const Cells& corners, const Targets& targets, const std::string& ctx) {
  CHECK_CTX(d.technique() == Technique::XWing, ctx);
  CHECK_CTX(d.action() == DeductionAction::EliminateCandidates, ctx);
  CHECK_CTX(d.evidence().unit == std::optional<Unit>(first), ctx);
  CHECK_CTX(d.evidence().second_unit == std::optional<Unit>(second), ctx);
  CHECK_CTX(!d.evidence().target_unit.has_value(), ctx);
  CHECK_CTX(d.evidence().cells == corners, ctx);
  CHECK_CTX(d.evidence().digits.digits() == Digits({digit}), ctx);
  CHECK_CTX(d.targets() == targets, ctx);
  CHECK_CTX(!d.explanation().empty(), ctx);
}

Targets targets_for(const Cells& cells, int digit) {
  Targets t;
  for (int c : cells) t.push_back({c, digit});
  return t;
}

// ---------- detection ----------
void test_row_oriented() {
  CandidateGrid g = empty_grid();
  make_row_xwing(g, 4, 1, 5, 2, 6);
  const DetectResult r = find_x_wings(g);
  CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 1);
  if (r.deductions.size() != 1) return;
  // Corners (1,2)(1,6)(5,2)(5,6); targets: columns 2 and 6 in the other seven rows.
  check_deduction(r.deductions[0], {UnitKind::Row, 1}, {UnitKind::Row, 5}, 4,
                  {11, 15, 47, 51},
                  targets_for({2, 6, 20, 24, 29, 33, 38, 42, 56, 60, 65, 69, 74, 78}, 4), "row");

  // Application: exactly those candidates go; everything else is untouched.
  const CandidateGrid before = g;
  const AppliedDeduction a = apply(g, r.deductions[0]);
  CHECK(a.status == ApplyStatus::Applied && a.effective == r.deductions[0].targets());
  for (int c = 0; c < 81; ++c) {
    DigitSet want = before.candidates(c);
    const bool target = std::any_of(r.deductions[0].targets().begin(), r.deductions[0].targets().end(),
                                    [&](const Elimination& e) { return e.cell == c; });
    if (target) want.erase(4);
    CHECK_CTX(g.candidates(c) == want, "cell " + std::to_string(c));
  }
  CHECK(g.board() == before.board() && g.invariants_hold());
  // The base rows still show the pattern and every target is gone: a repeat is
  // a no-op, and nothing new is left to find (the columns now confine the
  // digit to the same rows, but there is nothing to remove from those rows).
  const CandidateGrid after = g;
  CHECK(apply(g, r.deductions[0]).status == ApplyStatus::NoChange && g == after);
  CHECK(find_x_wings(g).deductions.empty());
}

void test_column_oriented() {
  CandidateGrid g = empty_grid();
  make_col_xwing(g, 7, 3, 8, 0, 4);
  const DetectResult r = find_x_wings(g);
  CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 1);
  if (r.deductions.size() != 1) return;
  // Corners (0,3)(0,8)(4,3)(4,8); targets: rows 0 and 4 in the other seven columns.
  check_deduction(r.deductions[0], {UnitKind::Column, 3}, {UnitKind::Column, 8}, 7,
                  {3, 8, 39, 44},
                  targets_for({0, 1, 2, 4, 5, 6, 7, 36, 37, 38, 40, 41, 42, 43}, 7), "col");
  const CandidateGrid before = g;
  const AppliedDeduction a = apply(g, r.deductions[0]);
  CHECK(a.status == ApplyStatus::Applied && a.effective.size() == 14);
  CHECK(g.board() == before.board() && g.invariants_hold());
  for (int c = 0; c < 81; ++c) {
    const bool in_rows = cell_row(c) == 0 || cell_row(c) == 4;
    const bool corner_col = cell_column(c) == 3 || cell_column(c) == 8;
    DigitSet want = before.candidates(c);
    if (in_rows && !corner_col) want.erase(7);
    CHECK_CTX(g.candidates(c) == want, "cell " + std::to_string(c));
  }
}

void test_both_orientations_and_ordering() {
  CandidateGrid g = empty_grid();
  make_row_xwing(g, 4, 1, 5, 2, 6);   // digit 4, row-oriented
  make_col_xwing(g, 4, 0, 8, 3, 7);   // digit 4, column-oriented (cols 0 and 8)
  make_col_xwing(g, 2, 3, 5, 0, 8);   // digit 2, column-oriented
  const DetectResult r = find_x_wings(g);
  CHECK(r.status == DetectStatus::Ok);
  std::vector<std::pair<int, UnitKind>> got;
  for (const Deduction& d : r.deductions) got.push_back({d.evidence().digits.digits().front(), d.evidence().unit->kind});
  const std::vector<std::pair<int, UnitKind>> want = {
      {2, UnitKind::Column}, {4, UnitKind::Row}, {4, UnitKind::Column}};
  CHECK(got == want);
  // Deterministic, pure, targets ascending and all currently present.
  const CandidateGrid before = g;
  CHECK(find_x_wings(g).deductions == r.deductions);
  CHECK(g == before);
  for (const Deduction& d : r.deductions) {
    CHECK(std::is_sorted(d.targets().begin(), d.targets().end(), [](auto x, auto y) { return x.cell < y.cell; }));
    for (const Elimination& e : d.targets()) CHECK(g.is_candidate(e.cell, e.digit));
  }
  // Two X-Wings of one digit and orientation, ordered by base unit indices.
  CandidateGrid m = empty_grid();
  make_row_xwing(m, 6, 0, 3, 1, 2);
  make_row_xwing(m, 6, 5, 8, 4, 7);
  std::vector<Cells> bases;
  for (const Deduction& d : find_x_wings(m).deductions)
    bases.push_back({d.evidence().unit->index, d.evidence().second_unit->index});
  CHECK((bases == std::vector<Cells>{{0, 3}, {5, 8}}));
}

void test_no_targets_and_negatives() {
  auto none = [](const CandidateGrid& g, const char* what) {
    const DetectResult r = find_x_wings(g);
    CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.empty(), what);
  };
  // Pattern present but the cross units already lack the digit: nothing to remove.
  { CandidateGrid g = empty_grid(); make_row_xwing(g, 4, 1, 5, 2, 6);
    strip(g, except(col_cells(2), {11, 47}), 4); strip(g, except(col_cells(6), {15, 51}), 4);
    none(g, "no targets"); }
  // A base row with a single candidate position.
  { CandidateGrid g = empty_grid();
    strip(g, except(row_cells(1), {11}), 4); strip(g, except(row_cells(5), {47, 51}), 4); none(g, "one position"); }
  // A base row with three positions.
  { CandidateGrid g = empty_grid();
    strip(g, except(row_cells(1), {11, 15, 12}), 4); strip(g, except(row_cells(5), {47, 51}), 4); none(g, "three positions"); }
  // Different position pairs: row 1 uses columns 2,6; row 5 uses columns 2,7.
  { CandidateGrid g = empty_grid();
    strip(g, except(row_cells(1), {11, 15}), 4); strip(g, except(row_cells(5), {47, 52}), 4); none(g, "different columns"); }
  // Only one qualifying row.
  { CandidateGrid g = empty_grid(); strip(g, except(row_cells(1), {11, 15}), 4); none(g, "single base"); }
  // Three rows with the same two columns: impossible/ambiguous, nothing deduced.
  { CandidateGrid g = empty_grid();
    for (int r : {1, 5, 8}) strip(g, except(row_cells(r), {r * 9 + 2, r * 9 + 6}), 4);
    none(g, "three rows"); }
  // Column-orientation negatives mirror the row ones.
  { CandidateGrid g = empty_grid();
    strip(g, except(col_cells(3), {3, 39}), 7); strip(g, except(col_cells(8), {8, 53}), 7); none(g, "different rows"); }
  // Digit already placed in a base row: that row cannot be a base.
  { Board b; b.set(5, 0, 4);              // 4 placed in row 5
    CandidateGrid g = grid_of(b);
    strip(g, except(row_cells(1), {11, 15}), 4);
    none(g, "placed in base row"); }
  { Board b; b.set(0, 3, 7);              // 7 placed in column 3 (a would-be base)
    CandidateGrid g = grid_of(b);
    strip(g, except(col_cells(8), {8, 44}), 7);
    none(g, "placed in base column"); }
  // Healthy empty state and a solved grid.
  none(empty_grid(), "empty");
  none(grid_of(parse(kSolved)), "solved");
}

void test_invalid_and_contradictory() {
  // Digit with no location in a row: contradiction, even with an X-Wing present.
  CandidateGrid g = empty_grid();
  make_row_xwing(g, 4, 1, 5, 2, 6);
  strip(g, row_cells(3), 4);
  const DetectResult r = find_x_wings(g);
  CHECK(r.status == DetectStatus::Contradiction && r.deductions.empty());
  CHECK(!r.contradictions.empty() &&
        r.contradictions[0].kind == ContradictionKind::DigitHasNoLocation);
  const DetectResult dead = find_x_wings(parse(
      "12345678." "........9" "........." "........." "........." "........." "........." "........." "........."));
  CHECK(dead.status == DetectStatus::Contradiction && dead.deductions.empty());
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  const DetectResult inv = find_x_wings(bad);
  CHECK(inv.status == DetectStatus::InvalidBoard && inv.deductions.empty() && !inv.conflicts.empty());
  const DetectResult ok = find_x_wings(Board{});
  CHECK(ok.status == DetectStatus::Ok && ok.deductions.empty());
}

// ---------- application: malformed, stale, atomic ----------
void test_apply_validation() {
  CandidateGrid base = empty_grid();
  make_row_xwing(base, 4, 1, 5, 2, 6);
  const Deduction good = find_x_wings(base).deductions.at(0);
  auto expect = [&](const CandidateGrid& start, const Deduction& d, ApplyStatus want, const char* what) {
    CandidateGrid g = start;
    const AppliedDeduction a = apply(g, d);
    CHECK_CTX(a.status == want, what);
    if (want != ApplyStatus::Applied) CHECK_CTX(g == start && a.effective.empty(), what);
  };
  auto with = [&](const Evidence& e, Targets t = {}) {
    return Deduction::eliminate(Technique::XWing, t.empty() ? good.targets() : t, e, "x");
  };
  const Evidence ev = good.evidence();

  // Stale / foreign states.
  expect(empty_grid(), good, ApplyStatus::Unsupported, "foreign grid");
  { CandidateGrid g = base; g.eliminate(11, 4);       // base row 1 lost a position
    expect(g, good, ApplyStatus::Unsupported, "base lost a position"); }
  { CandidateGrid g = base; g.eliminate(47, 4); g.eliminate(51, 4);  // row 5 lost both
    expect(g, good, ApplyStatus::Unsupported, "row lost positions"); }
  { Board b; b.set(5, 0, 4); CandidateGrid g = grid_of(b);
    strip(g, except(row_cells(1), {11, 15}), 4);
    expect(g, good, ApplyStatus::Unsupported, "digit placed in a base row"); }
  { // Positions no longer match (row 5 now confined to a different pair).
    CandidateGrid g = base; g.eliminate(47, 4);
    CandidateGrid h = empty_grid();
    strip(h, except(row_cells(1), {11, 15}), 4); strip(h, except(row_cells(5), {47, 52}), 4);
    expect(h, good, ApplyStatus::Unsupported, "positions differ"); }
  // Targets already absent are skipped; the rest still apply.
  { CandidateGrid g = base; g.eliminate(2, 4); g.eliminate(78, 4);
    const AppliedDeduction a = apply(g, good);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == good.targets().size() - 2); }

  // Malformed evidence.
  Evidence e = ev; e.unit.reset();
  expect(base, with(e), ApplyStatus::Unsupported, "no first unit");
  e = ev; e.second_unit.reset();
  expect(base, with(e), ApplyStatus::Unsupported, "no second unit");
  e = ev; e.second_unit = e.unit;
  expect(base, with(e), ApplyStatus::Unsupported, "same unit twice");
  e = ev; e.second_unit = Unit{UnitKind::Column, 5};
  expect(base, with(e), ApplyStatus::Unsupported, "mixed kinds");
  e = ev; e.unit = Unit{UnitKind::Box, 0}; e.second_unit = Unit{UnitKind::Box, 1};
  expect(base, with(e), ApplyStatus::Unsupported, "boxes");
  e = ev; e.second_unit = Unit{UnitKind::Row, 4};
  expect(base, with(e), ApplyStatus::Unsupported, "wrong second row");
  e = ev; e.cells = {11, 15, 47};
  expect(base, with(e), ApplyStatus::Unsupported, "three cells");
  e = ev; e.cells = {11, 15, 47, 52};
  expect(base, with(e), ApplyStatus::Unsupported, "wrong corner");
  e = ev; e.cells = {11, 15, 47, 47};
  expect(base, with(e), ApplyStatus::Unsupported, "repeated corner");
  e = ev; e.digits.insert(5);
  expect(base, with(e), ApplyStatus::Unsupported, "two digits");
  e = ev; e.digits = DigitSet{}; e.digits.insert(5);
  expect(base, with(e), ApplyStatus::Unsupported, "digit not the pattern's");
  { // Shuffled corner order is the same set of cells: accepted.
    e = ev; e.cells = {51, 11, 47, 15};
    CandidateGrid g = base;
    CHECK(apply(g, with(e)).status == ApplyStatus::Applied); }
  { // Orientation mismatch: row units with column-style corners.
    e = ev; e.cells = {20, 24, 56, 60};
    expect(base, with(e), ApplyStatus::Unsupported, "corners of the wrong orientation"); }

  // The distinct-unit/kind guards in isolation (other checks alone would let
  // these through, with unsound eliminations).
  { // One row named twice with matching duplicated corners.
    e = ev; e.second_unit = e.unit; e.cells = {11, 11, 15, 15};
    expect(base, with(e), ApplyStatus::Unsupported, "same row twice, duplicated corners"); }
  { // Two boxes whose digit sits in the same two rows: the corner arithmetic
    // would be meaningless, so boxes must be refused outright.
    CandidateGrid g = empty_grid();
    strip(g, except(unit_cells_vec(Unit{UnitKind::Box, 0}), {0, 9}), 4);
    strip(g, except(unit_cells_vec(Unit{UnitKind::Box, 1}), {3, 12}), 4);
    Evidence be; be.unit = Unit{UnitKind::Box, 0}; be.second_unit = Unit{UnitKind::Box, 1};
    be.cells = {0, 9, 1, 10}; be.digits.insert(4);
    expect(g, Deduction::eliminate(Technique::XWing, {{5, 4}}, be, "x"), ApplyStatus::Unsupported, "two boxes"); }
  { // A base row with THREE positions must not pass as an X-Wing.
    CandidateGrid g = empty_grid();
    strip(g, except(row_cells(1), {11, 15, 16}), 4);   // row 1: columns 2, 6, 7
    strip(g, except(row_cells(5), {47, 51}), 4);       // row 5: columns 2, 6
    expect(g, good, ApplyStatus::Unsupported, "base with three positions"); }

  // Invalid targets.
  expect(base, with(ev, {{3, 4}}), ApplyStatus::Unsupported, "target outside the cross columns");
  expect(base, with(ev, {{11, 4}}), ApplyStatus::Unsupported, "target is a corner (base row)");
  expect(base, with(ev, {{9, 4}}), ApplyStatus::Unsupported, "target in a base row");
  expect(base, with(ev, {{2, 5}}), ApplyStatus::Unsupported, "wrong digit");
  CHECK_THROWS_AS(with(ev, {{81, 4}}), std::out_of_range);
  CHECK_THROWS_AS(with(ev, {{2, 0}}), std::invalid_argument);
  CHECK_THROWS_AS(with(ev, {{2, 10}}), std::invalid_argument);
  { // A filled target cell is refused.
    Board b; b.set(0, 2, 8);  // cell 2 filled
    CandidateGrid g = grid_of(b);
    make_row_xwing(g, 4, 1, 5, 2, 6);
    expect(g, with(ev, {{6, 4}, {2, 4}}), ApplyStatus::Rejected, "filled target"); }
  // Atomic refusal: one target holds only the digit, so removing it would
  // empty that cell; the valid targets before it must not stay applied.
  { CandidateGrid g = base;
    for (int d = 1; d <= 9; ++d) if (d != 4) g.eliminate(78, d);
    CHECK(g.candidates(78).digits() == Digits({4}));
    expect(g, good, ApplyStatus::WouldContradict, "would empty a cell"); }

  // Column orientation gets the same checks.
  CandidateGrid col = empty_grid();
  make_col_xwing(col, 7, 3, 8, 0, 4);
  const Deduction cgood = find_x_wings(col).deductions.at(0);
  expect(empty_grid(), cgood, ApplyStatus::Unsupported, "foreign grid (columns)");
  { CandidateGrid g = col; g.eliminate(3, 7); expect(g, cgood, ApplyStatus::Unsupported, "column lost a position"); }
  { Evidence ce = cgood.evidence(); ce.cells = {11, 15, 47, 51};
    expect(col, Deduction::eliminate(Technique::XWing, cgood.targets(), ce, "x"), ApplyStatus::Unsupported, "row-style corners on columns"); }
  { CandidateGrid g = col; const AppliedDeduction a = apply(g, cgood);
    CHECK(a.status == ApplyStatus::Applied && g.invariants_hold()); }
}

// ---------- pair / triple / locked regressions ----------
void test_other_techniques_unaffected() {
  // The new Evidence field is unused by every other technique.
  CandidateGrid g = empty_grid();
  for (int c : {0, 4}) for (int d = 3; d <= 9; ++d) g.eliminate(c, d);          // naked pair {1,2}
  for (int c : {27, 36, 45}) for (int d : {1, 2, 6, 7, 8, 9}) g.eliminate(c, d); // naked triple {3,4,5}
  const std::vector<Deduction> pairs = find_naked_pairs(g).deductions;
  const std::vector<Deduction> triples = find_naked_triples(g).deductions;
  CHECK(!pairs.empty() && !triples.empty());
  for (const Deduction& d : pairs) CHECK(!d.evidence().second_unit.has_value() && d.evidence().cells.size() == 2);
  for (const Deduction& d : triples) CHECK(!d.evidence().second_unit.has_value() && d.evidence().cells.size() == 3);
  for (const Deduction& d : find_pointing(g).deductions) CHECK(!d.evidence().second_unit.has_value());
  for (const Deduction& d : find_singles(g).deductions) CHECK(!d.evidence().second_unit.has_value());
  // Pair and triple application are unchanged.
  CandidateGrid h = g;
  CHECK(apply(h, pairs.at(0)).status == ApplyStatus::Applied);
  CHECK(apply(h, pairs.at(0)).status == ApplyStatus::NoChange);
  CHECK(apply(h, triples.at(0)).status == ApplyStatus::Applied);
  CHECK(h.invariants_hold());
  // Shapes are not interchangeable with X-Wing evidence.
  CandidateGrid x = empty_grid();
  make_row_xwing(x, 4, 1, 5, 2, 6);
  const Deduction xw = find_x_wings(x).deductions.at(0);
  for (Technique t : {Technique::NakedPair, Technique::HiddenPair, Technique::NakedTriple,
                      Technique::HiddenTriple, Technique::Pointing, Technique::Claiming}) {
    CandidateGrid c = x;
    const Deduction swapped = Deduction::eliminate(t, xw.targets(), xw.evidence(), "x");
    CHECK(apply(c, swapped).status == ApplyStatus::Unsupported && c == x);
  }
  CandidateGrid y = g;
  for (const Deduction& d : {pairs.at(0), triples.at(0)}) {
    CandidateGrid c = y;
    const Deduction swapped = Deduction::eliminate(Technique::XWing, d.targets(), d.evidence(), "x");
    CHECK(apply(c, swapped).status == ApplyStatus::Unsupported && c == y);
  }
}

// ---------- independent reference + soundness ----------
struct RefX { int digit; UnitKind kind; int u1, u2; Cells corners; Targets targets; };

std::vector<RefX> reference_xwings(const CandidateGrid& g) {
  auto has = [&](int r, int c, int d) { return g.is_candidate(r * 9 + c, d); };
  auto placed = [&](int r, int c, int d) { return g.board().get(r * 9 + c) == d; };
  std::vector<RefX> out;
  for (int d = 1; d <= 9; ++d) {
    for (int orient = 0; orient < 2; ++orient) {  // 0 rows, 1 columns
      auto at = [&](int base, int cross) { return orient == 0 ? std::pair{base, cross} : std::pair{cross, base}; };
      std::vector<std::vector<int>> pos(9);
      for (int b = 0; b < 9; ++b) {
        bool is_placed = false;
        for (int x = 0; x < 9; ++x) { auto [r, c] = at(b, x); is_placed = is_placed || placed(r, c, d); }
        if (is_placed) continue;
        for (int x = 0; x < 9; ++x) { auto [r, c] = at(b, x); if (has(r, c, d)) pos[static_cast<std::size_t>(b)].push_back(x); }
      }
      for (int b1 = 0; b1 < 9; ++b1) for (int b2 = b1 + 1; b2 < 9; ++b2) {
        const auto& p1 = pos[static_cast<std::size_t>(b1)];
        if (p1.size() != 2 || p1 != pos[static_cast<std::size_t>(b2)]) continue;
        int sharing = 0;
        for (int b = 0; b < 9; ++b) sharing += pos[static_cast<std::size_t>(b)] == p1 ? 1 : 0;
        if (sharing != 2) continue;
        Cells corners;
        for (int b : {b1, b2}) for (int x : p1) { auto [r, c] = at(b, x); corners.push_back(r * 9 + c); }
        std::sort(corners.begin(), corners.end());
        Targets t;
        for (int cell = 0; cell < 81; ++cell) {
          const int r = cell / 9, c = cell % 9;
          const int base = orient == 0 ? r : c, cross = orient == 0 ? c : r;
          if ((cross == p1[0] || cross == p1[1]) && base != b1 && base != b2 && has(r, c, d)) t.push_back({cell, d});
        }
        if (!t.empty()) out.push_back({d, orient == 0 ? UnitKind::Row : UnitKind::Column, b1, b2, corners, t});
      }
    }
  }
  return out;
}

void check_reference(const CandidateGrid& g, const std::string& ctx) {
  const DetectResult r = find_x_wings(g);
  CHECK_CTX(r.status == DetectStatus::Ok, ctx);
  const auto want = reference_xwings(g);
  CHECK_CTX(r.deductions.size() == want.size(), ctx);
  for (std::size_t i = 0; i < std::min(want.size(), r.deductions.size()); ++i) {
    check_deduction(r.deductions[i], {want[i].kind, want[i].u1}, {want[i].kind, want[i].u2},
                    want[i].digit, want[i].corners, want[i].targets, ctx);
  }
}

void test_reference_and_soundness() {
  const Board solution = parse(kSolved);
  ref::Rng rng(123);
  int found = 0, states = 0, applied = 0;
  for (int trial = 0; trial < 40; ++trial) {
    Board b = solution;
    const int blanks = 50 + rng.below(20);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    CandidateGrid g = grid_of(b);
    for (int step = 0; step < 40; ++step) {
      const std::string ctx = "trial " + std::to_string(trial) + " step " + std::to_string(step);
      const CandidateGrid snapshot = g;
      check_reference(g, ctx);
      CHECK_CTX(g == snapshot, ctx);
      ++states;
      const DetectResult xw = find_x_wings(g);
      for (const Deduction& d : xw.deductions) {
        ++found;
        for (const Elimination& e : d.targets()) {
          CHECK_CTX(e.digit != solution.get(e.cell), ctx);  // never the true digit
          CHECK_CTX(g.is_candidate(e.cell, e.digit), ctx);
        }
      }
      DetectResult next = xw;   // X-Wing first so it is exercised
      for (const DetectResult& more : {find_singles(g), find_locked_candidates(g), find_pairs(g), find_triples(g)}) {
        next.deductions.insert(next.deductions.end(), more.deductions.begin(), more.deductions.end());
      }
      bool progressed = false;
      for (const Deduction& d : next.deductions) {
        const AppliedDeduction a = apply(g, d);
        CHECK_CTX(a.status == ApplyStatus::Applied || a.status == ApplyStatus::NoChange ||
                      a.status == ApplyStatus::Unsupported || a.status == ApplyStatus::Rejected, ctx);
        if (a.status == ApplyStatus::Applied) {
          progressed = true;
          if (d.technique() == Technique::XWing) ++applied;
          break;
        }
      }
      CHECK_CTX(g.invariants_hold() && !g.has_contradiction(), ctx);
      for (int c = 0; c < 81; ++c) {
        if (g.board().get(c) == 0) CHECK_CTX(g.is_candidate(c, solution.get(c)), ctx);
      }
      if (!progressed) break;
    }
  }
  std::printf("xwing sweep: %d states, %d detected, %d applied\n", states, found, applied);
  CHECK(found > 20 && applied > 20);
}

}  // namespace

int main() {
  test_row_oriented();
  test_column_oriented();
  test_both_orientations_and_ordering();
  test_no_targets_and_negatives();
  test_invalid_and_contradictory();
  test_apply_validation();
  test_other_techniques_unaffected();
  test_reference_and_soundness();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
