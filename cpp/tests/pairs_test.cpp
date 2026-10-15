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

CandidateGrid grid_of(const Board& b) {
  CandidateInitResult r = CandidateGrid::from_board(b);
  CHECK(r.status == InitStatus::Ok);
  return *r.grid;
}
CandidateGrid empty_grid() { return grid_of(Board{}); }

// Leave only `keep` as candidates of `cell`.
void restrict_to(CandidateGrid& g, int cell, const Digits& keep) {
  for (int d = 1; d <= 9; ++d) {
    if (std::find(keep.begin(), keep.end(), d) == keep.end()) g.eliminate(cell, d);
  }
  CHECK(g.candidates(cell).digits() == keep);
}
// Remove `digit` from every listed cell that has it.
void strip(CandidateGrid& g, const Cells& cells, int digit) {
  for (int c : cells) CHECK_CTX(g.eliminate(c, digit) == EliminateOutcome::Eliminated, "strip cell " + std::to_string(c) + " digit " + std::to_string(digit));
}
Cells others(const Unit& unit, const Cells& except) {
  Cells out;
  for (int c : unit_cells(unit)) {
    if (std::find(except.begin(), except.end(), c) == except.end()) out.push_back(c);
  }
  return out;
}

std::vector<Elimination> expected_naked_targets(const CandidateGrid& g, const Unit& unit,
                                                const Cells& pair_cells, const Digits& pair) {
  std::vector<Elimination> out;
  for (int c : others(unit, pair_cells))
    for (int d : pair) if (g.is_candidate(c, d)) out.push_back({c, d});
  return out;
}

void check_deduction(const Deduction& d, Technique tech, const Unit& unit, const Cells& cells,
                     const Digits& digits, const std::vector<Elimination>& targets,
                     const std::string& ctx) {
  CHECK_CTX(d.technique() == tech, ctx);
  CHECK_CTX(d.action() == DeductionAction::EliminateCandidates, ctx);
  CHECK_CTX(d.evidence().unit == std::optional<Unit>(unit), ctx);
  CHECK_CTX(!d.evidence().target_unit.has_value(), ctx);
  CHECK_CTX(d.evidence().cells == cells, ctx);
  CHECK_CTX(d.evidence().digits.digits() == digits, ctx);
  CHECK_CTX(d.targets() == targets, ctx);
  CHECK_CTX(!d.explanation().empty(), ctx);
}

// A naked pair whose two cells share exactly one unit of the given kind.
struct Geometry { UnitKind kind; int index; Cells cells; };
const Geometry kGeometries[] = {
    {UnitKind::Row, 0, {0, 4}},     // same row only
    {UnitKind::Column, 0, {0, 36}}, // same column only
    {UnitKind::Box, 0, {0, 10}},    // same box only
};

// ---------- naked pairs ----------
void test_naked_pairs_each_unit_kind() {
  for (const Geometry& geo : kGeometries) {
    const std::string ctx = std::string("naked ") + std::to_string(static_cast<int>(geo.kind));
    CandidateGrid g = empty_grid();
    for (int c : geo.cells) restrict_to(g, c, {2, 7});
    const Unit unit{geo.kind, geo.index};
    const DetectResult r = find_naked_pairs(g);
    CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.size() == 1, ctx);
    if (r.deductions.size() != 1) continue;
    const auto targets = expected_naked_targets(g, unit, geo.cells, {2, 7});
    CHECK_CTX(targets.size() == 14, ctx);  // 7 other cells x 2 digits
    check_deduction(r.deductions[0], Technique::NakedPair, unit, geo.cells, {2, 7}, targets, ctx);
    CHECK_CTX(find_pairs(g).deductions == r.deductions, ctx);  // no hidden pair here

    // Apply: only the targets change; pair cells and everything outside the
    // unit keep their candidates.
    const CandidateGrid before = g;
    const AppliedDeduction a = apply(g, r.deductions[0]);
    CHECK_CTX(a.status == ApplyStatus::Applied && a.effective == targets, ctx);
    for (int c = 0; c < 81; ++c) {
      DigitSet want = before.candidates(c);
      const auto members = unit_cells(unit);
      const bool in_unit = std::find(members.begin(), members.end(), c) != members.end();
      const bool is_pair = std::find(geo.cells.begin(), geo.cells.end(), c) != geo.cells.end();
      if (in_unit && !is_pair) { want.erase(2); want.erase(7); }
      CHECK_CTX(g.candidates(c) == want, ctx + " cell " + std::to_string(c));
    }
    CHECK_CTX(g.board() == before.board() && g.invariants_hold(), ctx);
    CHECK_CTX(apply(g, r.deductions[0]).status == ApplyStatus::NoChange, ctx);  // repeat
    CHECK_CTX(find_naked_pairs(g).deductions.empty(), ctx);  // nothing left to remove
  }
}

void test_naked_pair_two_units_and_multiple() {
  // Cells 0 and 1 share both row 0 and box 0 -> one deduction per unit, row first.
  CandidateGrid g = empty_grid();
  restrict_to(g, 0, {2, 7});
  restrict_to(g, 1, {2, 7});
  const auto r = find_naked_pairs(g).deductions;
  CHECK(r.size() == 2);
  if (r.size() == 2) {
    CHECK((r[0].evidence().unit == std::optional<Unit>(Unit{UnitKind::Row, 0})));
    CHECK((r[1].evidence().unit == std::optional<Unit>(Unit{UnitKind::Box, 0})));
    // Overlapping targets: after the row deduction, the box one only has the
    // box cells outside the row left to remove.
    const CandidateGrid before = g;
    CHECK(apply(g, r[0]).status == ApplyStatus::Applied);
    const AppliedDeduction second = apply(g, r[1]);
    CHECK(second.status == ApplyStatus::Applied);
    CHECK(second.effective.size() == 12);  // 6 box cells outside row 0 x 2 digits
    CHECK(apply(g, r[1]).status == ApplyStatus::NoChange);
    CHECK(g.invariants_hold() && g.board() == before.board());
  }

  // Two different pairs in one row, ordered by their digits.
  CandidateGrid m = empty_grid();
  restrict_to(m, 4, {3, 8});   // cells 1 and 4... deliberately created out of order
  restrict_to(m, 0, {2, 7});
  restrict_to(m, 8, {2, 7});
  restrict_to(m, 5, {3, 8});
  const auto rows = find_naked_pairs(m).deductions;
  std::vector<Deduction> in_row0;
  for (const Deduction& d : rows) if (*d.evidence().unit == Unit{UnitKind::Row, 0}) in_row0.push_back(d);
  CHECK(in_row0.size() == 2);
  if (in_row0.size() == 2) {
    CHECK(in_row0[0].evidence().digits.digits() == Digits({2, 7}));
    CHECK(in_row0[0].evidence().cells == Cells({0, 8}));
    CHECK(in_row0[1].evidence().digits.digits() == Digits({3, 8}));
    CHECK(in_row0[1].evidence().cells == Cells({4, 5}));
    for (const Deduction& d : in_row0) {
      CHECK(std::is_sorted(d.targets().begin(), d.targets().end(), [](auto x, auto y) {
        return x.cell != y.cell ? x.cell < y.cell : x.digit < y.digit; }));
    }
  }
}

void test_naked_pair_negatives() {
  // Single occurrence of {a,b}.
  {
    CandidateGrid g = empty_grid();
    restrict_to(g, 0, {2, 7});
    const DetectResult r = find_naked_pairs(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.empty());
  }
  // Three cells with the same {a,b} in a row: ambiguous, nothing is deduced.
  {
    CandidateGrid g = empty_grid();
    for (int c : {0, 4, 8}) restrict_to(g, c, {2, 7});
    const DetectResult r = find_naked_pairs(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.empty());
  }
  // A three-candidate cell is not a pair cell.
  {
    CandidateGrid g = empty_grid();
    restrict_to(g, 0, {2, 7});
    restrict_to(g, 4, {2, 7, 9});
    CHECK(find_naked_pairs(g).deductions.empty());
  }
  // Sharing only one candidate.
  {
    CandidateGrid g = empty_grid();
    restrict_to(g, 0, {2, 7});
    restrict_to(g, 4, {2, 8});
    CHECK(find_naked_pairs(g).deductions.empty());
  }
  // A real pair, but nothing left to eliminate.
  {
    CandidateGrid g = empty_grid();
    restrict_to(g, 0, {2, 7});
    restrict_to(g, 4, {2, 7});
    for (int c : others(Unit{UnitKind::Row, 0}, {0, 4})) { g.eliminate(c, 2); g.eliminate(c, 7); }
    const DetectResult r = find_naked_pairs(g);
    CHECK(r.status == DetectStatus::Ok);
    for (const Deduction& d : r.deductions) CHECK(*d.evidence().unit != (Unit{UnitKind::Row, 0}));
  }
  // Filled cells are never pair cells.
  CHECK(find_pairs(grid_of(parse(kSolved))).deductions.empty());
}

// ---------- hidden pairs ----------
void test_hidden_pairs_each_unit_kind() {
  for (const Geometry& geo : kGeometries) {
    const std::string ctx = std::string("hidden ") + std::to_string(static_cast<int>(geo.kind));
    const Unit unit{geo.kind, geo.index};
    CandidateGrid g = empty_grid();
    for (int d : {4, 6}) strip(g, others(unit, geo.cells), d);
    const DetectResult r = find_hidden_pairs(g);
    CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.size() == 1, ctx);
    if (r.deductions.size() != 1) continue;
    std::vector<Elimination> targets;
    for (int c : geo.cells)
      for (int d : {1, 2, 3, 5, 7, 8, 9}) targets.push_back({c, d});
    check_deduction(r.deductions[0], Technique::HiddenPair, unit, geo.cells, {4, 6}, targets, ctx);
    CHECK_CTX(find_naked_pairs(g).deductions.empty(), ctx);

    const CandidateGrid before = g;
    const AppliedDeduction a = apply(g, r.deductions[0]);
    CHECK_CTX(a.status == ApplyStatus::Applied && a.effective == targets, ctx);
    for (int c = 0; c < 81; ++c) {
      const bool is_pair = std::find(geo.cells.begin(), geo.cells.end(), c) != geo.cells.end();
      CHECK_CTX(g.candidates(c) == (is_pair ? DigitSet::from_mask((1u << 3) | (1u << 5))
                                            : before.candidates(c)), ctx);
    }
    CHECK_CTX(g.invariants_hold() && g.board() == before.board(), ctx);
    // A repeat is a no-op: the pair digits still have the same two positions
    // and every target candidate is already gone.
    const CandidateGrid restricted = g;
    const AppliedDeduction again = apply(g, r.deductions[0]);
    CHECK_CTX(again.status == ApplyStatus::NoChange && g == restricted, ctx);
    CHECK_CTX(find_hidden_pairs(g).deductions.empty(), ctx);
  }
}

void test_hidden_pair_preserves_extras_correctly() {
  // The example from the technique's definition: cells A={1,4,6}, B={2,4,6}.
  CandidateGrid g = empty_grid();
  const Unit row{UnitKind::Row, 0};
  restrict_to(g, 0, {1, 4, 6});
  restrict_to(g, 4, {2, 4, 6});
  // 4 and 6 must not appear elsewhere in the row.
  for (int c : others(row, {0, 4})) { g.eliminate(c, 4); g.eliminate(c, 6); }
  const auto r = find_hidden_pairs(g).deductions;
  CHECK(r.size() == 1);
  if (r.size() == 1) {
    CHECK((r[0].targets() == std::vector<Elimination>{{0, 1}, {4, 2}}));
    CHECK(r[0].evidence().digits.digits() == Digits({4, 6}));
  }
  // Multiple hidden pairs in one row, ordered by digits.
  CandidateGrid m = empty_grid();
  for (int d : {4, 6}) strip(m, others(row, {0, 4}), d);
  for (int d : {1, 3}) strip(m, others(row, {1, 5}), d);
  Cells row0;
  std::vector<Deduction> hp;
  for (const Deduction& d : find_hidden_pairs(m).deductions) if (*d.evidence().unit == row) hp.push_back(d);
  // Digits 1,3 sit in {1,5}; digits 4,6 in {0,4}: ascending by lower digit.
  CHECK(hp.size() == 2);
  if (hp.size() == 2) {
    CHECK(hp[0].evidence().digits.digits() == Digits({1, 3}) && hp[0].evidence().cells == Cells({1, 5}));
    CHECK(hp[1].evidence().digits.digits() == Digits({4, 6}) && hp[1].evidence().cells == Cells({0, 4}));
  }
}

void test_hidden_pair_negatives() {
  const Unit row{UnitKind::Row, 0};
  // Different position sets: 4 in {0,4}, 6 in {0,5}.
  {
    CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 4}), 4);
    strip(g, others(row, {0, 5}), 6);
    CHECK(find_hidden_pairs(g).deductions.empty());
  }
  // One digit in three cells.
  {
    CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 4}), 6);
    strip(g, others(row, {0, 4, 8}), 4);
    CHECK(find_hidden_pairs(g).deductions.empty());
  }
  // One digit with a single position (a hidden single), the other with two.
  {
    CandidateGrid g = empty_grid();
    strip(g, others(row, {0}), 4);
    strip(g, others(row, {0, 4}), 6);
    CHECK(find_hidden_pairs(g).deductions.empty());
    CHECK(!find_hidden_singles(g).deductions.empty());
  }
  // A digit already placed in the unit cannot be part of a pair.
  {
    Board b;
    b.set(0, 8, 4);  // 4 placed in row 0
    CandidateGrid g = grid_of(b);
    for (int c : others(row, {0, 4, 8})) g.eliminate(c, 6);
    for (const Deduction& d : find_hidden_pairs(g).deductions) {
      if (*d.evidence().unit == row) CHECK(!d.evidence().digits.contains(4));
    }
  }
  // Nothing extra to remove: the two cells hold only the pair digits.
  {
    CandidateGrid g = empty_grid();
    for (int d : {4, 6}) strip(g, others(row, {0, 4}), d);
    restrict_to(g, 0, {4, 6});
    restrict_to(g, 4, {4, 6});
    const DetectResult r = find_hidden_pairs(g);
    CHECK(r.status == DetectStatus::Ok);
    for (const Deduction& d : r.deductions) CHECK(*d.evidence().unit != row);
  }
  // Three digits sharing the same two cells: ambiguous, nothing deduced.
  {
    CandidateGrid g = empty_grid();
    for (int d : {4, 6, 8}) strip(g, others(row, {0, 4}), d);
    for (const Deduction& d : find_hidden_pairs(g).deductions) CHECK(*d.evidence().unit != row);
  }
  // A digit with zero positions is a contradiction, not a hidden pair.
  {
    CandidateGrid g = empty_grid();
    strip(g, others(row, {}), 4);  // every cell of the row loses 4
    for (const DetectResult& r : {find_hidden_pairs(g), find_naked_pairs(g), find_pairs(g)}) {
      CHECK(r.status == DetectStatus::Contradiction && r.deductions.empty());
      CHECK(r.contradictions.size() == 1 &&
            r.contradictions[0].kind == ContradictionKind::DigitHasNoLocation);
    }
  }
}

void test_invalid_and_contradictory_states() {
  const DetectResult dead = find_pairs(parse(
      "12345678." "........9" "........." "........." "........." "........." "........." "........." "........."));
  CHECK(dead.status == DetectStatus::Contradiction && dead.deductions.empty());
  CHECK(!dead.contradictions.empty() &&
        dead.contradictions[0].kind == ContradictionKind::EmptyCellNoCandidates);
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  const DetectResult inv = find_pairs(bad);
  CHECK(inv.status == DetectStatus::InvalidBoard && inv.deductions.empty() && !inv.conflicts.empty());
  const DetectResult none = find_pairs(Board{});
  CHECK(none.status == DetectStatus::Ok && none.deductions.empty());
}

// ---------- purity / determinism ----------
CandidateGrid mixed_grid() {
  CandidateGrid g = empty_grid();
  restrict_to(g, 0, {2, 7});
  restrict_to(g, 4, {2, 7});
  for (int d : {4, 6}) strip(g, others(Unit{UnitKind::Column, 5}, {5, 41}), d);
  return g;
}

void test_purity_and_determinism() {
  const CandidateGrid g = mixed_grid();
  const CandidateGrid before = g;
  const DetectResult a = find_pairs(g);
  const DetectResult b = find_pairs(g);
  CHECK(a.deductions == b.deductions && !a.deductions.empty());
  (void)find_naked_pairs(g);
  (void)find_hidden_pairs(g);
  CHECK(g == before);
  for (const Deduction& d : a.deductions) {
    for (const Elimination& e : d.targets()) CHECK(g.is_candidate(e.cell, e.digit));
  }
  // Naked pairs come before hidden pairs.
  bool seen_hidden = false;
  for (const Deduction& d : a.deductions) {
    if (d.technique() == Technique::HiddenPair) seen_hidden = true;
    else CHECK(!seen_hidden);
  }
}

// ---------- application: staleness and failure ----------
void test_apply_validation() {
  CandidateGrid base = empty_grid();
  restrict_to(base, 0, {2, 7});
  restrict_to(base, 4, {2, 7});
  const Deduction naked = find_naked_pairs(base).deductions.at(0);
  auto expect = [&](const CandidateGrid& start, const Deduction& d, ApplyStatus want, const char* what) {
    CandidateGrid g = start;
    const AppliedDeduction a = apply(g, d);
    CHECK_CTX(a.status == want, what);
    if (want != ApplyStatus::Applied) {
      CHECK_CTX(g == start && a.effective.empty(), what);
    }
  };
  expect(empty_grid(), naked, ApplyStatus::Unsupported, "foreign grid");
  // A pair cell has shrunk since detection: no longer the pattern.
  { CandidateGrid g = base; g.eliminate(0, 7); expect(g, naked, ApplyStatus::Unsupported, "pair cell shrank"); }
  // Targets that are already gone are skipped.
  { CandidateGrid g = base; g.eliminate(2, 2); g.eliminate(2, 7);
    const AppliedDeduction a = apply(g, naked);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == naked.targets().size() - 2); }
  // Malformed evidence / targets.
  Evidence ev = naked.evidence();
  Evidence no_unit = ev;  no_unit.unit.reset();
  expect(base, Deduction::eliminate(Technique::NakedPair, naked.targets(), no_unit, "x"), ApplyStatus::Unsupported, "no unit");
  Evidence one_cell = ev;  one_cell.cells = {0};
  expect(base, Deduction::eliminate(Technique::NakedPair, naked.targets(), one_cell, "x"), ApplyStatus::Unsupported, "one cell");
  Evidence same = ev;  same.cells = {0, 0};
  expect(base, Deduction::eliminate(Technique::NakedPair, naked.targets(), same, "x"), ApplyStatus::Unsupported, "same cell twice");
  Evidence outside = ev;  outside.cells = {0, 40};  // 40 is not in row 0
  expect(base, Deduction::eliminate(Technique::NakedPair, naked.targets(), outside, "x"), ApplyStatus::Unsupported, "cell off unit");
  Evidence three = ev;  three.digits.insert(9);
  expect(base, Deduction::eliminate(Technique::NakedPair, naked.targets(), three, "x"), ApplyStatus::Unsupported, "three digits");
  expect(base, Deduction::eliminate(Technique::NakedPair, {{1, 5}}, ev, "x"), ApplyStatus::Unsupported, "non-pair digit target");
  expect(base, Deduction::eliminate(Technique::NakedPair, {{0, 2}}, ev, "x"), ApplyStatus::Unsupported, "target is a pair cell");
  expect(base, Deduction::eliminate(Technique::NakedPair, {{9, 2}}, ev, "x"), ApplyStatus::Unsupported, "target outside unit");
  CHECK_THROWS_AS(Deduction::eliminate(Technique::NakedPair, {{81, 2}}, ev, "x"), std::out_of_range);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::NakedPair, {{1, 0}}, ev, "x"), std::invalid_argument);
  // A pair cell filled elsewhere (cell 0 now holds a value): unsupported.
  { Board b; b.set(0, 3);  CandidateGrid g = grid_of(b);
    expect(g, naked, ApplyStatus::Unsupported, "pair cell filled"); }
  // Last-candidate elimination: refused atomically (cell 2 holds only {2,7},
  // making a third {2,7} cell, which detection would not use but apply sees).
  {
    CandidateGrid g = base;
    restrict_to(g, 2, {2, 7});
    const Deduction forced = Deduction::eliminate(Technique::NakedPair, {{1, 2}, {2, 2}, {2, 7}}, ev, "x");
    expect(g, forced, ApplyStatus::WouldContradict, "empties a cell");
  }

  // Hidden pair.
  CandidateGrid h = empty_grid();
  const Unit row{UnitKind::Row, 0};
  for (int d : {4, 6}) strip(h, others(row, {0, 4}), d);
  const Deduction hidden = find_hidden_pairs(h).deductions.at(0);
  expect(empty_grid(), hidden, ApplyStatus::Unsupported, "foreign grid (hidden)");
  { CandidateGrid g = h; g.eliminate(4, 4);  // digit 4 now has one position
    expect(g, hidden, ApplyStatus::Unsupported, "digit lost a position"); }
  { CandidateGrid g = h; g.eliminate(0, 1);  // extra candidate already gone: still valid
    const AppliedDeduction a = apply(g, hidden);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == hidden.targets().size() - 1); }
  Evidence hev = hidden.evidence();
  expect(h, Deduction::eliminate(Technique::HiddenPair, {{0, 4}}, hev, "x"), ApplyStatus::Unsupported, "removes a pair digit");
  expect(h, Deduction::eliminate(Technique::HiddenPair, {{1, 1}}, hev, "x"), ApplyStatus::Unsupported, "target not a pair cell");
  { Board b; b.set(8, 4);  CandidateGrid g = grid_of(b);   // 4 placed in the row
    expect(g, hidden, ApplyStatus::Unsupported, "digit placed in unit"); }
}

// ---------- independent reference + soundness ----------
struct RefPair { Technique t; Unit unit; Cells cells; Digits digits; std::vector<Elimination> targets; };

std::vector<RefPair> reference_pairs(const CandidateGrid& g) {
  // Independent, naive formulation: per unit, per digit pair, from plain
  // vectors (no use of the production detectors or unit helpers).
  std::array<Digits, 81> cand;
  for (int c = 0; c < 81; ++c) cand[static_cast<std::size_t>(c)] = g.candidates(c).digits();
  auto has = [&](int c, int d) {
    const auto& cs = cand[static_cast<std::size_t>(c)];
    return std::find(cs.begin(), cs.end(), d) != cs.end();
  };
  std::vector<Unit> units;
  for (UnitKind k : {UnitKind::Row, UnitKind::Column, UnitKind::Box})
    for (int u = 0; u < 9; ++u) units.push_back({k, u});
  std::vector<Cells> member_lists;
  for (const Unit& u : units) {
    Cells m;
    for (int c = 0; c < 81; ++c) {
      const int r = c / 9, col = c % 9, bx = (r / 3) * 3 + col / 3;
      const int mine = u.kind == UnitKind::Row ? r : u.kind == UnitKind::Column ? col : bx;
      if (mine == u.index) m.push_back(c);
    }
    member_lists.push_back(m);
  }
  std::vector<RefPair> out;
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t ui = 0; ui < units.size(); ++ui) {
      const Cells& m = member_lists[ui];
      std::array<Cells, 10> positions;  // empty for a digit placed in the unit
      if (pass == 1) {
        for (int d = 1; d <= 9; ++d) {
          bool placed = false;
          for (int c : m) placed = placed || g.board().get(c) == d;
          if (!placed) for (int c : m) if (has(c, d)) positions[static_cast<std::size_t>(d)].push_back(c);
        }
      }
      for (int a = 1; a <= 9; ++a) for (int b = a + 1; b <= 9; ++b) {
        if (pass == 0) {
          Cells holders;
          for (int c : m) if (cand[static_cast<std::size_t>(c)] == Digits({a, b})) holders.push_back(c);
          if (holders.size() != 2) continue;
          std::vector<Elimination> t;
          for (int c : m) {
            if (c == holders[0] || c == holders[1]) continue;
            for (int d : {a, b}) if (has(c, d)) t.push_back({c, d});
          }
          if (!t.empty()) out.push_back({Technique::NakedPair, units[ui], holders, {a, b}, t});
        } else {
          const Cells& pa = positions[static_cast<std::size_t>(a)];
          if (pa.size() != 2 || pa != positions[static_cast<std::size_t>(b)]) continue;
          int sharing = 0;
          for (int d = 1; d <= 9; ++d) sharing += positions[static_cast<std::size_t>(d)] == pa ? 1 : 0;
          if (sharing != 2) continue;
          std::vector<Elimination> t;
          for (int c : pa) for (int d : cand[static_cast<std::size_t>(c)]) if (d != a && d != b) t.push_back({c, d});
          if (!t.empty()) out.push_back({Technique::HiddenPair, units[ui], pa, {a, b}, t});
        }
      }
    }
  }
  return out;
}

void check_reference(const CandidateGrid& g, const std::string& ctx) {
  const DetectResult r = find_pairs(g);
  CHECK_CTX(r.status == DetectStatus::Ok, ctx);
  const auto want = reference_pairs(g);
  CHECK_CTX(r.deductions.size() == want.size(), ctx);
  for (std::size_t i = 0; i < std::min(want.size(), r.deductions.size()); ++i) {
    check_deduction(r.deductions[i], want[i].t, want[i].unit, want[i].cells, want[i].digits,
                    want[i].targets, ctx);
  }
}

void test_reference_and_soundness() {
  const Board solution = parse(kSolved);
  ref::Rng rng(31);
  int naked = 0, hidden = 0, states = 0, applied = 0;
  for (int trial = 0; trial < 30; ++trial) {
    Board b = solution;
    const int blanks = 40 + rng.below(30);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    CandidateGrid g = grid_of(b);
    for (int step = 0; step < 60; ++step) {
      const std::string ctx = "trial " + std::to_string(trial) + " step " + std::to_string(step);
      const CandidateGrid snapshot = g;
      check_reference(g, ctx);
      CHECK_CTX(g == snapshot, ctx);  // detection did not mutate
      ++states;
      const DetectResult pairs = find_pairs(g);
      for (const Deduction& d : pairs.deductions) {
        (d.technique() == Technique::NakedPair ? naked : hidden)++;
        for (const Elimination& e : d.targets()) {
          CHECK_CTX(e.digit != solution.get(e.cell), ctx);  // never the true digit
          CHECK_CTX(g.is_candidate(e.cell, e.digit), ctx);
        }
      }
      // Advance the state: pairs first (so they are exercised), then locked
      // candidates, then singles.
      DetectResult next = pairs;
      for (const DetectResult& more : {find_locked_candidates(g), find_singles(g)}) {
        next.deductions.insert(next.deductions.end(), more.deductions.begin(), more.deductions.end());
      }
      bool progressed = false;
      for (const Deduction& d : next.deductions) {
        const AppliedDeduction a = apply(g, d);
        CHECK_CTX(a.status == ApplyStatus::Applied || a.status == ApplyStatus::NoChange ||
                      a.status == ApplyStatus::Unsupported || a.status == ApplyStatus::Rejected,
                  ctx);
        if (a.status == ApplyStatus::Applied) {
          progressed = true;
          if (d.technique() == Technique::NakedPair || d.technique() == Technique::HiddenPair) ++applied;
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
  std::printf("pairs sweep: %d states, %d naked / %d hidden detected, %d applied\n", states, naked, hidden, applied);
  CHECK(naked > 20 && hidden > 20 && applied > 50);
}

}  // namespace

int main() {
  test_naked_pairs_each_unit_kind();
  test_naked_pair_two_units_and_multiple();
  test_naked_pair_negatives();
  test_hidden_pairs_each_unit_kind();
  test_hidden_pair_preserves_extras_correctly();
  test_hidden_pair_negatives();
  test_invalid_and_contradictory_states();
  test_purity_and_determinism();
  test_apply_validation();
  test_reference_and_soundness();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
