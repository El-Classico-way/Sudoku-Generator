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

void restrict_to(CandidateGrid& g, int cell, const Digits& keep) {
  for (int d = 1; d <= 9; ++d) {
    if (std::find(keep.begin(), keep.end(), d) == keep.end()) g.eliminate(cell, d);
  }
  CHECK(g.candidates(cell).digits() == keep);
}
void strip(CandidateGrid& g, const Cells& cells, int digit) {
  for (int c : cells) {
    CHECK_CTX(g.eliminate(c, digit) == EliminateOutcome::Eliminated,
              "strip cell " + std::to_string(c) + " digit " + std::to_string(digit));
  }
}
Cells others(const Unit& unit, const Cells& except) {
  Cells out;
  for (int c : unit_cells(unit)) {
    if (std::find(except.begin(), except.end(), c) == except.end()) out.push_back(c);
  }
  return out;
}

void check_deduction(const Deduction& d, Technique tech, const Unit& unit, const Cells& cells,
                     const Digits& digits, const Targets& targets, const std::string& ctx) {
  CHECK_CTX(d.technique() == tech, ctx);
  CHECK_CTX(d.action() == DeductionAction::EliminateCandidates, ctx);
  CHECK_CTX(d.evidence().unit == std::optional<Unit>(unit), ctx);
  CHECK_CTX(!d.evidence().target_unit.has_value(), ctx);
  CHECK_CTX(d.evidence().cells == cells, ctx);
  CHECK_CTX(d.evidence().digits.digits() == digits, ctx);
  CHECK_CTX(d.targets() == targets, ctx);
  CHECK_CTX(!d.explanation().empty(), ctx);
}

// Triple cells that share exactly one unit of the given kind.
struct Geometry { UnitKind kind; int index; Cells cells; };
const Geometry kGeometries[] = {
    {UnitKind::Row, 0, {0, 4, 8}},       // one row; three different boxes/columns
    {UnitKind::Column, 0, {0, 27, 54}},  // one column; three different boxes/rows
    {UnitKind::Box, 0, {0, 10, 20}},     // one box; three different rows/columns
};

// Two shapes that both make a naked triple on digits {2,5,9}.
const std::vector<std::vector<Digits>> kNakedShapes = {
    {{2, 5}, {5, 9}, {2, 9}},        // the 3-cycle
    {{2, 5, 9}, {2, 5, 9}, {2, 5}},  // full + partial
    {{2, 5, 9}, {2, 5, 9}, {2, 5, 9}},
};

Targets naked_targets(const CandidateGrid& g, const Unit& unit, const Cells& triple, const Digits& t) {
  Targets out;
  for (int c : others(unit, triple))
    for (int d : t) if (g.is_candidate(c, d)) out.push_back({c, d});
  return out;
}

// ---------- naked triples ----------
void test_naked_triples() {
  for (const Geometry& geo : kGeometries) {
    for (std::size_t shape = 0; shape < kNakedShapes.size(); ++shape) {
      const std::string ctx = "naked kind " + std::to_string(static_cast<int>(geo.kind)) +
                              " shape " + std::to_string(shape);
      CandidateGrid g = empty_grid();
      for (std::size_t i = 0; i < 3; ++i) restrict_to(g, geo.cells[i], kNakedShapes[shape][i]);
      const Unit unit{geo.kind, geo.index};
      const DetectResult r = find_naked_triples(g);
      CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.size() == 1, ctx);
      if (r.deductions.size() != 1) continue;
      const Targets targets = naked_targets(g, unit, geo.cells, {2, 5, 9});
      CHECK_CTX(targets.size() == 18, ctx);  // 6 other cells x 3 digits
      check_deduction(r.deductions[0], Technique::NakedTriple, unit, geo.cells, {2, 5, 9}, targets, ctx);
      CHECK_CTX(find_triples(g).deductions == r.deductions, ctx);

      // Apply: only the targets change.
      const CandidateGrid before = g;
      const AppliedDeduction a = apply(g, r.deductions[0]);
      CHECK_CTX(a.status == ApplyStatus::Applied && a.effective == targets, ctx);
      const auto members = unit_cells(unit);
      for (int c = 0; c < 81; ++c) {
        DigitSet want = before.candidates(c);
        const bool in_unit = std::find(members.begin(), members.end(), c) != members.end();
        const bool in_triple = std::find(geo.cells.begin(), geo.cells.end(), c) != geo.cells.end();
        if (in_unit && !in_triple) { want.erase(2); want.erase(5); want.erase(9); }
        CHECK_CTX(g.candidates(c) == want, ctx + " cell " + std::to_string(c));
      }
      CHECK_CTX(g.board() == before.board() && g.invariants_hold(), ctx);
      CHECK_CTX(apply(g, r.deductions[0]).status == ApplyStatus::NoChange, ctx);
      CHECK_CTX(find_naked_triples(g).deductions.empty(), ctx);
    }
  }
}

void test_naked_triple_negatives() {
  const Unit row{UnitKind::Row, 0};
  auto none = [&](const CandidateGrid& g, const char* what) {
    const DetectResult r = find_naked_triples(g);
    CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.empty(), what);
  };
  // Only two cells confined to the digit set.
  { CandidateGrid g = empty_grid(); restrict_to(g, 0, {2, 5}); restrict_to(g, 4, {5, 9}); none(g, "two cells"); }
  // Third cell has a fourth candidate.
  { CandidateGrid g = empty_grid(); restrict_to(g, 0, {2, 5}); restrict_to(g, 4, {5, 9});
    restrict_to(g, 8, {2, 7, 9}); none(g, "four-digit cell"); }
  // Union smaller than three digits (three cells, two digits).
  { CandidateGrid g = empty_grid(); for (int c : {0, 4, 8}) restrict_to(g, c, {2, 5}); none(g, "union of two"); }
  // Four cells confined to three digits: ambiguous, nothing deduced.
  { CandidateGrid g = empty_grid(); restrict_to(g, 0, {2, 5}); restrict_to(g, 4, {5, 9});
    restrict_to(g, 8, {2, 9}); restrict_to(g, 2, {2, 5, 9}); none(g, "four cells"); }
  // A single-candidate cell inside the set is not a triple member.
  { CandidateGrid g = empty_grid(); restrict_to(g, 0, {2}); restrict_to(g, 4, {5, 9});
    restrict_to(g, 8, {2, 9}); none(g, "single member"); }
  // A genuine triple with nothing left to eliminate.
  { CandidateGrid g = empty_grid();
    restrict_to(g, 0, {2, 5}); restrict_to(g, 4, {5, 9}); restrict_to(g, 8, {2, 9});
    for (int c : others(row, {0, 4, 8})) for (int d : {2, 5, 9}) g.eliminate(c, d);
    for (const Deduction& d : find_naked_triples(g).deductions) CHECK(*d.evidence().unit != row); }
  // A pair is not a triple: two cells {2,5} with a third cell unrelated.
  { CandidateGrid g = empty_grid(); restrict_to(g, 0, {2, 5}); restrict_to(g, 4, {2, 5}); none(g, "pair only"); }
  // Filled cells never take part.
  CHECK(find_triples(grid_of(parse(kSolved))).deductions.empty());
}

// ---------- hidden triples ----------
// Digits 3,6,8 confined to the three cells P, each digit in two or three of
// them; every other cell of the unit loses the three digits.
void make_hidden(CandidateGrid& g, const Unit& unit, const Cells& p, const std::vector<Cells>& where) {
  const Digits digits = {3, 6, 8};
  for (std::size_t i = 0; i < 3; ++i) {
    Cells remove;
    for (int c : unit_cells(unit)) {
      if (std::find(where[i].begin(), where[i].end(), c) == where[i].end()) remove.push_back(c);
    }
    strip(g, remove, digits[i]);
  }
  (void)p;
}

void test_hidden_triples() {
  for (const Geometry& geo : kGeometries) {
    const std::vector<std::vector<Cells>> shapes = {
        {{geo.cells[0], geo.cells[1]}, {geo.cells[1], geo.cells[2]}, {geo.cells[0], geo.cells[2]}},
        {{geo.cells[0], geo.cells[1], geo.cells[2]}, {geo.cells[0], geo.cells[1], geo.cells[2]},
         {geo.cells[0], geo.cells[1], geo.cells[2]}},
    };
    for (std::size_t s = 0; s < shapes.size(); ++s) {
      const std::string ctx = "hidden kind " + std::to_string(static_cast<int>(geo.kind)) +
                              " shape " + std::to_string(s);
      const Unit unit{geo.kind, geo.index};
      CandidateGrid g = empty_grid();
      make_hidden(g, unit, geo.cells, shapes[s]);
      const DetectResult r = find_hidden_triples(g);
      CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.size() == 1, ctx);
      if (r.deductions.size() != 1) continue;
      Targets targets;
      for (int c : geo.cells)
        for (int d : {1, 2, 4, 5, 7, 9}) targets.push_back({c, d});
      check_deduction(r.deductions[0], Technique::HiddenTriple, unit, geo.cells, {3, 6, 8}, targets, ctx);
      CHECK_CTX(find_naked_triples(g).deductions.empty(), ctx);

      const CandidateGrid before = g;
      const AppliedDeduction a = apply(g, r.deductions[0]);
      CHECK_CTX(a.status == ApplyStatus::Applied && a.effective == targets, ctx);
      for (int c = 0; c < 81; ++c) {
        const bool in_triple = std::find(geo.cells.begin(), geo.cells.end(), c) != geo.cells.end();
        // Triple cells keep only the triple digits they already had; every
        // other cell is untouched.
        DigitSet want = before.candidates(c);
        if (in_triple) {
          for (int d = 1; d <= 9; ++d) if (d != 3 && d != 6 && d != 8) want.erase(d);
        }
        CHECK_CTX(g.candidates(c) == want, ctx + " cell " + std::to_string(c));
        if (in_triple) CHECK_CTX(!g.candidates(c).empty(), ctx);
      }
      CHECK_CTX(g.invariants_hold() && g.board() == before.board(), ctx);
      CHECK_CTX(apply(g, r.deductions[0]).status == ApplyStatus::NoChange, ctx);
      CHECK_CTX(find_hidden_triples(g).deductions.empty(), ctx);
    }
  }
}

void test_hidden_triple_negatives() {
  const Unit row{UnitKind::Row, 0};
  auto no_row_triple = [&](const CandidateGrid& g, const char* what) {
    const DetectResult r = find_hidden_triples(g);
    CHECK_CTX(r.status == DetectStatus::Ok, what);
    for (const Deduction& d : r.deductions) CHECK_CTX(*d.evidence().unit != row, what);
  };
  // A digit in four cells.
  { CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 1, 2, 3}), 3); strip(g, others(row, {1, 2}), 6); strip(g, others(row, {0, 2}), 8);
    no_row_triple(g, "four-cell digit"); }
  // A digit with a single location (hidden single) among the three.
  { CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 1}), 3); strip(g, others(row, {1, 2}), 6); strip(g, others(row, {2}), 8);
    no_row_triple(g, "single location"); }
  // Only two digits confined (a hidden pair, not a triple).
  { CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 1}), 3); strip(g, others(row, {0, 1}), 6);
    no_row_triple(g, "pair only"); }
  // Four digits confined to three cells: ambiguous.
  { CandidateGrid g = empty_grid();
    strip(g, others(row, {0, 1, 2}), 3); strip(g, others(row, {0, 1, 2}), 6);
    strip(g, others(row, {0, 1, 2}), 8); strip(g, others(row, {0, 1, 2}), 4);
    no_row_triple(g, "four digits"); }
  // A digit already placed in the unit is not eligible.
  { Board b; b.set(0, 8, 3);  // 3 placed in row 0
    CandidateGrid g = grid_of(b);
    strip(g, others(row, {0, 1, 2, 8}), 6);  // cell 8 is filled
    strip(g, others(row, {0, 1, 2, 8}), 8);
    for (const Deduction& d : find_hidden_triples(g).deductions)
      if (*d.evidence().unit == row) CHECK(!d.evidence().digits.contains(3)); }
  // The three cells hold only the triple digits: nothing to eliminate.
  { CandidateGrid g = empty_grid();
    for (int d : {3, 6, 8}) strip(g, others(row, {0, 1, 2}), d);
    for (int c : {0, 1, 2}) restrict_to(g, c, {3, 6, 8});
    no_row_triple(g, "nothing extra"); }
  // A digit with no location is a contradiction.
  { CandidateGrid g = empty_grid();
    const Cells whole = others(row, {});
    strip(g, whole, 3);
    for (const DetectResult& r : {find_hidden_triples(g), find_naked_triples(g), find_triples(g)}) {
      CHECK(r.status == DetectStatus::Contradiction && r.deductions.empty());
      CHECK(r.contradictions.size() == 1 && r.contradictions[0].kind == ContradictionKind::DigitHasNoLocation);
    } }
}

void test_invalid_inputs() {
  const DetectResult dead = find_triples(parse(
      "12345678." "........9" "........." "........." "........." "........." "........." "........." "........."));
  CHECK(dead.status == DetectStatus::Contradiction && dead.deductions.empty());
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  const DetectResult inv = find_triples(bad);
  CHECK(inv.status == DetectStatus::InvalidBoard && inv.deductions.empty() && !inv.conflicts.empty());
  const DetectResult none = find_triples(Board{});
  CHECK(none.status == DetectStatus::Ok && none.deductions.empty());
}

// ---------- ordering, purity ----------
void test_ordering_and_purity() {
  // Two naked triples in row 0: {1,2,3} on cells 0,1,2 and {4,5,6} on cells 3,4,5.
  CandidateGrid g = empty_grid();
  for (int c : {0, 1, 2}) restrict_to(g, c, {1, 2, 3});
  for (int c : {3, 4, 5}) restrict_to(g, c, {4, 5, 6});
  const CandidateGrid before = g;
  const DetectResult r = find_naked_triples(g);
  CHECK(g == before);  // pure
  std::vector<Deduction> in_row0;
  for (const Deduction& d : r.deductions) if (*d.evidence().unit == Unit{UnitKind::Row, 0}) in_row0.push_back(d);
  CHECK(in_row0.size() == 2);
  if (in_row0.size() == 2) {
    CHECK(in_row0[0].evidence().digits.digits() == Digits({1, 2, 3}) && in_row0[0].evidence().cells == Cells({0, 1, 2}));
    CHECK(in_row0[1].evidence().digits.digits() == Digits({4, 5, 6}) && in_row0[1].evidence().cells == Cells({3, 4, 5}));
  }
  CHECK(find_naked_triples(g).deductions == r.deductions);  // deterministic
  for (const Deduction& d : r.deductions) {
    CHECK(std::is_sorted(d.targets().begin(), d.targets().end(), [](auto x, auto y) {
      return x.cell != y.cell ? x.cell < y.cell : x.digit < y.digit; }));
    for (const Elimination& e : d.targets()) CHECK(g.is_candidate(e.cell, e.digit));
  }
  // Boxes share the same cells {0,1,2}: row 0 AND box 0 each give a deduction, row first.
  std::vector<Unit> sources;
  for (const Deduction& d : r.deductions) sources.push_back(*d.evidence().unit);
  const auto row_pos = std::find(sources.begin(), sources.end(), Unit{UnitKind::Row, 0});
  const auto box_pos = std::find(sources.begin(), sources.end(), Unit{UnitKind::Box, 0});
  CHECK(row_pos != sources.end() && box_pos != sources.end() && row_pos < box_pos);
  // Naked triples come before hidden triples in find_triples.
  CandidateGrid m = empty_grid();
  for (int c : {0, 4, 8}) restrict_to(m, c, {1, 2, 3});
  const Unit col{UnitKind::Column, 1};
  make_hidden(m, col, {}, {{1, 10}, {10, 19}, {1, 19}});
  bool seen_hidden = false;
  for (const Deduction& d : find_triples(m).deductions) {
    if (d.technique() == Technique::HiddenTriple) seen_hidden = true;
    else CHECK(!seen_hidden);
  }
}

// ---------- evidence-model regression: pairs vs triples ----------
void test_evidence_shapes_do_not_cross() {
  // A state containing both a naked pair and a naked triple.
  CandidateGrid g = empty_grid();
  restrict_to(g, 0, {1, 2});
  restrict_to(g, 4, {1, 2});                         // naked pair {1,2} in row 0
  for (int c : {27, 36, 45}) restrict_to(g, c, {3, 4, 5});  // naked triple in column 0? cells 27,36,45
  const Deduction pair = find_naked_pairs(g).deductions.at(0);
  CHECK(pair.technique() == Technique::NakedPair && pair.evidence().cells.size() == 2);
  const std::vector<Deduction> triples = find_naked_triples(g).deductions;
  CHECK(!triples.empty());
  for (const Deduction& t : triples) {
    CHECK(t.technique() == Technique::NakedTriple && t.evidence().cells.size() == 3);
  }
  // The pair path still behaves as in M10 after the shared-helper change.
  CandidateGrid h = g;
  CHECK(apply(h, pair).status == ApplyStatus::Applied);
  CHECK(apply(h, pair).status == ApplyStatus::NoChange);
  // Shapes must match the technique: a pair's evidence under a triple id (and
  // vice versa) is refused, leaving the grid unchanged.
  auto refuse = [&](const Deduction& d, Technique as, const char* what) {
    CandidateGrid c = g;
    const Deduction swapped = Deduction::eliminate(as, d.targets(), d.evidence(), "x");
    const AppliedDeduction a = apply(c, swapped);
    CHECK_CTX(a.status == ApplyStatus::Unsupported && c == g && a.effective.empty(), what);
  };
  refuse(pair, Technique::NakedTriple, "pair evidence as naked triple");
  refuse(pair, Technique::HiddenTriple, "pair evidence as hidden triple");
  refuse(triples.at(0), Technique::NakedPair, "triple evidence as naked pair");
  refuse(triples.at(0), Technique::HiddenPair, "triple evidence as hidden pair");
}

// ---------- application: stale, malformed, atomic ----------
void test_apply_validation() {
  const Unit row{UnitKind::Row, 0};
  CandidateGrid base = empty_grid();
  for (int c : {0, 4, 8}) restrict_to(base, c, {2, 5, 9});
  const Deduction naked = find_naked_triples(base).deductions.at(0);
  auto expect = [&](const CandidateGrid& start, const Deduction& d, ApplyStatus want, const char* what) {
    CandidateGrid g = start;
    const AppliedDeduction a = apply(g, d);
    CHECK_CTX(a.status == want, what);
    if (want != ApplyStatus::Applied) CHECK_CTX(g == start && a.effective.empty(), what);
  };
  expect(empty_grid(), naked, ApplyStatus::Unsupported, "foreign grid");
  // Shrinking that keeps the union is still supported (documented).
  { CandidateGrid g = base; g.eliminate(0, 9);
    const AppliedDeduction a = apply(g, naked);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == naked.targets().size()); }
  // Shrinking that breaks the union: 9 gone from every triple cell.
  { CandidateGrid g = base; for (int c : {0, 4, 8}) g.eliminate(c, 9);
    expect(g, naked, ApplyStatus::Unsupported, "union broken"); }
  // A triple cell has a candidate outside the set (extra digit): not confined.
  { CandidateGrid h = empty_grid();
    restrict_to(h, 0, {2, 5, 7, 9}); restrict_to(h, 4, {2, 5, 9}); restrict_to(h, 8, {2, 5, 9});
    expect(h, naked, ApplyStatus::Unsupported, "cell not confined"); }
  // Targets already absent are skipped.
  { CandidateGrid g = base; g.eliminate(1, 2); g.eliminate(1, 5);
    const AppliedDeduction a = apply(g, naked);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == naked.targets().size() - 2); }
  // Malformed evidence.
  const Evidence ev = naked.evidence();
  auto with = [&](Evidence e, Targets t = {}) {
    return Deduction::eliminate(Technique::NakedTriple, t.empty() ? naked.targets() : t, e, "x");
  };
  Evidence e = ev; e.unit.reset();
  expect(base, with(e), ApplyStatus::Unsupported, "no unit");
  e = ev; e.cells = {0, 4};
  expect(base, with(e), ApplyStatus::Unsupported, "two cells");
  e = ev; e.cells = {0, 4, 4};
  expect(base, with(e), ApplyStatus::Unsupported, "repeated cell");
  e = ev; e.cells = {0, 4, 40};
  expect(base, with(e), ApplyStatus::Unsupported, "cell outside unit");
  e = ev; e.digits.erase(9);
  expect(base, with(e), ApplyStatus::Unsupported, "two digits");
  e = ev; e.digits.insert(1);
  expect(base, with(e), ApplyStatus::Unsupported, "four digits");
  expect(base, with(ev, {{1, 7}}), ApplyStatus::Unsupported, "non-triple digit target");
  expect(base, with(ev, {{0, 2}}), ApplyStatus::Unsupported, "target is a triple cell");
  expect(base, with(ev, {{9, 2}}), ApplyStatus::Unsupported, "target outside unit");
  CHECK_THROWS_AS(with(ev, {{81, 2}}), std::out_of_range);
  CHECK_THROWS_AS(with(ev, {{1, 10}}), std::invalid_argument);
  // A triple cell that is filled.
  { Board b; b.set(0, 1);  expect(grid_of(b), naked, ApplyStatus::Unsupported, "triple cell filled"); }
  // Atomic refusal: a fourth cell {2,5} would be emptied by the elimination;
  // the valid targets listed before it must not stay applied.
  { CandidateGrid g = base; restrict_to(g, 2, {2, 5});
    expect(g, naked, ApplyStatus::WouldContradict, "empties a cell, whole grid untouched"); }

  // Hidden triple.
  CandidateGrid h = empty_grid();
  make_hidden(h, row, {}, {{0, 4}, {4, 8}, {0, 8}});
  const Deduction hidden = find_hidden_triples(h).deductions.at(0);
  expect(empty_grid(), hidden, ApplyStatus::Unsupported, "foreign (hidden)");
  // Shrinking inside the three cells keeps support (digit 3 loses cell 0).
  { CandidateGrid g = h; g.eliminate(0, 3);
    const AppliedDeduction a = apply(g, hidden);
    CHECK(a.status == ApplyStatus::Applied && a.effective.size() == hidden.targets().size()); }
  // A digit loses all its positions: unsupported.
  { CandidateGrid k = h; k.eliminate(4, 6); k.eliminate(8, 6);  // 6 sat only in {4,8}
    expect(k, hidden, ApplyStatus::Unsupported, "digit has no position"); }
  // A cell is no longer needed (union shrinks to two cells).
  { CandidateGrid g = h; g.eliminate(8, 3); g.eliminate(8, 6); g.eliminate(8, 8);
    expect(g, hidden, ApplyStatus::Unsupported, "union shrank"); }
  // Digit now placed in the unit.
  { Board b; b.set(0, 2, 3);  expect(grid_of(b), hidden, ApplyStatus::Unsupported, "digit placed"); }
  Evidence hev = hidden.evidence();
  expect(h, Deduction::eliminate(Technique::HiddenTriple, {{0, 3}}, hev, "x"), ApplyStatus::Unsupported, "removes a triple digit");
  expect(h, Deduction::eliminate(Technique::HiddenTriple, {{1, 1}}, hev, "x"), ApplyStatus::Unsupported, "target not a triple cell");
}

// ---------- independent reference + soundness ----------
struct RefTriple { Technique t; Unit unit; Cells cells; Digits digits; Targets targets; };

std::vector<RefTriple> reference_triples(const CandidateGrid& g) {
  std::array<Digits, 81> cand;
  for (int c = 0; c < 81; ++c) cand[static_cast<std::size_t>(c)] = g.candidates(c).digits();
  auto has = [&](int c, int d) {
    const auto& cs = cand[static_cast<std::size_t>(c)];
    return std::find(cs.begin(), cs.end(), d) != cs.end();
  };
  auto subset = [&](int c, const Digits& t) {
    for (int d : cand[static_cast<std::size_t>(c)]) if (std::find(t.begin(), t.end(), d) == t.end()) return false;
    return true;
  };
  std::vector<Unit> units;
  for (UnitKind k : {UnitKind::Row, UnitKind::Column, UnitKind::Box})
    for (int u = 0; u < 9; ++u) units.push_back({k, u});
  std::vector<RefTriple> out;
  for (int pass = 0; pass < 2; ++pass) {
    for (const Unit& unit : units) {
      Cells m;
      for (int c = 0; c < 81; ++c) {
        const int r = c / 9, col = c % 9, bx = (r / 3) * 3 + col / 3;
        const int mine = unit.kind == UnitKind::Row ? r : unit.kind == UnitKind::Column ? col : bx;
        if (mine == unit.index) m.push_back(c);
      }
      std::array<Cells, 10> pos;
      if (pass == 1) {
        for (int d = 1; d <= 9; ++d) {
          bool placed = false;
          for (int c : m) placed = placed || g.board().get(c) == d;
          if (!placed) for (int c : m) if (has(c, d)) pos[static_cast<std::size_t>(d)].push_back(c);
        }
      }
      for (int a = 1; a <= 9; ++a) for (int b = a + 1; b <= 9; ++b) for (int c = b + 1; c <= 9; ++c) {
        const Digits t = {a, b, c};
        if (pass == 0) {
          Cells conf;
          bool sizes_ok = true;
          std::vector<int> uni;
          for (int cell : m) {
            const auto& cs = cand[static_cast<std::size_t>(cell)];
            if (cs.empty() || !subset(cell, t)) continue;
            conf.push_back(cell);
            if (cs.size() < 2) sizes_ok = false;
            for (int d : cs) if (std::find(uni.begin(), uni.end(), d) == uni.end()) uni.push_back(d);
          }
          if (!sizes_ok || conf.size() != 3 || uni.size() != 3) continue;
          Targets tg;
          for (int cell : m) {
            if (std::find(conf.begin(), conf.end(), cell) != conf.end()) continue;
            for (int d : t) if (has(cell, d)) tg.push_back({cell, d});
          }
          if (!tg.empty()) out.push_back({Technique::NakedTriple, unit, conf, t, tg});
        } else {
          Cells uni;
          bool ok = true;
          for (int d : t) {
            const Cells& w = pos[static_cast<std::size_t>(d)];
            if (w.size() < 2 || w.size() > 3) ok = false;
            for (int cell : w) if (std::find(uni.begin(), uni.end(), cell) == uni.end()) uni.push_back(cell);
          }
          if (!ok || uni.size() != 3) continue;
          std::sort(uni.begin(), uni.end());
          int confined = 0;
          for (int d = 1; d <= 9; ++d) {
            const Cells& w = pos[static_cast<std::size_t>(d)];
            if (w.empty()) continue;
            bool inside = true;
            for (int cell : w) inside = inside && std::find(uni.begin(), uni.end(), cell) != uni.end();
            confined += inside ? 1 : 0;
          }
          if (confined != 3) continue;
          Targets tg;
          for (int cell : uni) for (int d : cand[static_cast<std::size_t>(cell)])
            if (d != a && d != b && d != c) tg.push_back({cell, d});
          if (!tg.empty()) out.push_back({Technique::HiddenTriple, unit, uni, t, tg});
        }
      }
    }
  }
  return out;
}

void check_reference(const CandidateGrid& g, const std::string& ctx) {
  const DetectResult r = find_triples(g);
  CHECK_CTX(r.status == DetectStatus::Ok, ctx);
  const auto want = reference_triples(g);
  CHECK_CTX(r.deductions.size() == want.size(), ctx);
  for (std::size_t i = 0; i < std::min(want.size(), r.deductions.size()); ++i) {
    check_deduction(r.deductions[i], want[i].t, want[i].unit, want[i].cells, want[i].digits,
                    want[i].targets, ctx);
  }
}

void test_reference_and_soundness() {
  const Board solution = parse(kSolved);
  ref::Rng rng(77);
  int naked = 0, hidden = 0, states = 0, applied = 0;
  for (int trial = 0; trial < 10; ++trial) {
    Board b = solution;
    const int blanks = 45 + rng.below(25);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    CandidateGrid g = grid_of(b);
    for (int step = 0; step < 40; ++step) {
      const std::string ctx = "trial " + std::to_string(trial) + " step " + std::to_string(step);
      const CandidateGrid snapshot = g;
      check_reference(g, ctx);
      CHECK_CTX(g == snapshot, ctx);
      ++states;
      const DetectResult triples = find_triples(g);
      for (const Deduction& d : triples.deductions) {
        (d.technique() == Technique::NakedTriple ? naked : hidden)++;
        for (const Elimination& e : d.targets()) {
          CHECK_CTX(e.digit != solution.get(e.cell), ctx);  // never the true digit
          CHECK_CTX(g.is_candidate(e.cell, e.digit), ctx);
        }
      }
      // Advance: triples first (so they are exercised), then pairs, locked, singles.
      DetectResult next = triples;
      for (const DetectResult& more : {find_pairs(g), find_locked_candidates(g), find_singles(g)}) {
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
          if (d.technique() == Technique::NakedTriple || d.technique() == Technique::HiddenTriple) ++applied;
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
  std::printf("triples sweep: %d states, %d naked / %d hidden detected, %d applied\n", states, naked, hidden, applied);
  CHECK(naked + hidden > 10 && applied > 5);
}

}  // namespace

int main() {
  test_naked_triples();
  test_naked_triple_negatives();
  test_hidden_triples();
  test_hidden_triple_negatives();
  test_invalid_inputs();
  test_ordering_and_purity();
  test_evidence_shapes_do_not_cross();
  test_apply_validation();
  test_reference_and_soundness();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
