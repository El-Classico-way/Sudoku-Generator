#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "sudoku/deduction.hpp"

using namespace sudoku;

namespace {

constexpr Technique kAll[] = {
    Technique::NakedSingle,       Technique::HiddenSingle,
    Technique::Pointing,  Technique::Claiming,  Technique::NakedPair,
    Technique::HiddenPair,        Technique::NakedTriple,
    Technique::HiddenTriple,      Technique::XWing};

void test_techniques() {
  std::vector<std::string_view> names;
  for (Technique t : kAll) {
    // Only the two singles (M08) are implemented so far.
    CHECK(is_implemented(t) == (t == Technique::NakedSingle || t == Technique::HiddenSingle ||
                                t == Technique::Pointing || t == Technique::Claiming ||
                                t == Technique::NakedPair || t == Technique::HiddenPair ||
                                t == Technique::NakedTriple || t == Technique::HiddenTriple ||
                                t == Technique::XWing));
    const auto n = technique_name(t);
    CHECK(!n.empty() && n != "unknown");
    for (auto m : names) CHECK(m != n);  // names are distinct
    names.push_back(n);
  }
  CHECK(technique_name(Technique::XWing) == "X-Wing");
  CHECK(technique_name(Technique::NakedSingle) == "naked single");
}

void test_construction() {
  Evidence ev;
  ev.unit = Unit{UnitKind::Row, 0};
  ev.cells = {2};
  ev.digits.insert(4);

  const Deduction place = Deduction::place(Technique::NakedSingle, 2, 4, ev, "only candidate");
  CHECK(place.technique() == Technique::NakedSingle);
  CHECK(place.action() == DeductionAction::PlaceDigit);
  CHECK((place.targets() == std::vector<Elimination>{{2, 4}}));
  CHECK(place.evidence() == ev && place.explanation() == "only candidate");

  const Deduction elim = Deduction::eliminate(
      Technique::Pointing, {{3, 1}, {4, 1}}, Evidence{}, "pointing");
  CHECK(elim.action() == DeductionAction::EliminateCandidates);
  CHECK((elim.targets() == std::vector<Elimination>{{3, 1}, {4, 1}}));
  CHECK(!elim.evidence().unit.has_value() && elim.evidence().cells.empty());

  // Malformed data is rejected at construction.
  CHECK_THROWS_AS(Deduction::place(Technique::NakedSingle, -1, 4, ev, "x"), std::out_of_range);
  CHECK_THROWS_AS(Deduction::place(Technique::NakedSingle, 81, 4, ev, "x"), std::out_of_range);
  CHECK_THROWS_AS(Deduction::place(Technique::NakedSingle, 2, 0, ev, "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::place(Technique::NakedSingle, 2, 10, ev, "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::place(Technique::NakedSingle, 2, 4, ev, ""), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {}, ev, "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {{1, 1}, {1, 1}}, ev, "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {{1, 11}}, ev, "x"), std::invalid_argument);
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {{99, 1}}, ev, "x"), std::out_of_range);
  Evidence bad_unit;  bad_unit.unit = Unit{UnitKind::Box, 9};
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {{1, 1}}, bad_unit, "x"), std::out_of_range);
  Evidence bad_cell;  bad_cell.cells = {81};
  CHECK_THROWS_AS(Deduction::eliminate(Technique::XWing, {{1, 1}}, bad_cell, "x"), std::out_of_range);
}

// Row 0 of an otherwise empty grid with a naked pair {1,2} in cells 0 and 4,
// so removing 1 and 2 from the other cells of row 0 is justified.
CandidateGrid pair_grid(const Board& board = Board{}) {
  CandidateGrid g = *CandidateGrid::from_board(board).grid;
  for (int cell : {0, 4}) for (int d = 3; d <= 9; ++d) g.eliminate(cell, d);
  return g;
}
Evidence pair_evidence() {
  Evidence ev;
  ev.unit = Unit{UnitKind::Row, 0};
  ev.cells = {0, 4};
  ev.digits.insert(1);
  ev.digits.insert(2);
  return ev;
}

void test_apply() {
  CandidateGrid grid = pair_grid();
  grid.eliminate(3, 2);  // cell 3 has already lost the 2
  const Board board_before = grid.board();

  // Constructing a deduction changes nothing.
  const Deduction d = Deduction::eliminate(
      Technique::NakedPair, {{2, 1}, {2, 2}, {3, 2}}, pair_evidence(), "test");
  const CandidateGrid untouched = grid;
  CHECK(grid == untouched && grid.is_candidate(2, 1) && grid.is_candidate(2, 2));

  // Applied: (3,2) was already absent, so only two removals are effective.
  AppliedDeduction a = apply(grid, d);
  CHECK(a.status == ApplyStatus::Applied);
  CHECK((a.effective == std::vector<Elimination>{{2, 1}, {2, 2}}));
  CHECK(a.deduction == d);
  CHECK(!grid.is_candidate(2, 1) && !grid.is_candidate(2, 2) && grid.is_candidate(3, 1));
  CHECK(grid.board() == board_before && grid.invariants_hold());

  // Repeating it is a valid no-op.
  a = apply(grid, d);
  CHECK(a.status == ApplyStatus::NoChange && a.effective.empty());

  // WouldContradict (would empty the last candidate): all-or-nothing, nothing
  // changes even though the first elimination in the list would have been fine.
  CandidateGrid g2 = pair_grid();
  for (int digit = 3; digit <= 9; ++digit) g2.eliminate(2, digit);  // cell 2 -> {1,2}
  const CandidateGrid g2_before = g2;
  const Deduction too_much = Deduction::eliminate(
      Technique::NakedPair, {{3, 1}, {2, 1}, {2, 2}}, pair_evidence(), "x");
  a = apply(g2, too_much);
  CHECK(a.status == ApplyStatus::WouldContradict && a.effective.empty());
  CHECK(g2 == g2_before);

  // Rejected: targets a filled cell (cell 8 holds 7), nothing changes.
  Board with_filled;
  with_filled.set(8, 7);
  CandidateGrid g3 = pair_grid(with_filled);
  const CandidateGrid g3_before = g3;
  a = apply(g3, Deduction::eliminate(Technique::NakedPair, {{3, 1}, {8, 1}}, pair_evidence(), "x"));
  CHECK(a.status == ApplyStatus::Rejected && a.effective.empty());
  CHECK(g3 == g3_before);

  // Placement with no supporting evidence is refused (a HiddenSingle without a
  // unit has nothing to justify it).
  const Deduction p = Deduction::place(Technique::HiddenSingle, 2, 1, Evidence{}, "x");
  a = apply(g2, p);
  CHECK(a.status == ApplyStatus::Unsupported && a.effective.empty());
  CHECK(g2 == g2_before);
  CHECK(a.deduction == p);
}

// Technique ids that have no elimination validator can never change a grid.
void test_unsupported_techniques_cannot_mutate() {
  const CandidateGrid base = pair_grid();
  // Targets that WOULD be applicable if the pair evidence were honoured.
  const std::vector<Elimination> targets = {{2, 1}, {2, 2}};
  const Technique unknown = static_cast<Technique>(99);
  for (Technique t : {Technique::NakedSingle, Technique::HiddenSingle, unknown}) {
    for (const Evidence& ev : {Evidence{}, pair_evidence()}) {
      CandidateGrid g = base;
      const AppliedDeduction a = apply(g, Deduction::eliminate(t, targets, ev, "x"));
      CHECK(a.status == ApplyStatus::Unsupported && a.effective.empty());
      CHECK(g == base);  // no mutation, with or without plausible evidence
    }
  }
  // The same targets under a validated technique and matching evidence apply.
  CandidateGrid ok = base;
  CHECK(apply(ok, Deduction::eliminate(Technique::NakedPair, targets, pair_evidence(), "x")).status ==
        ApplyStatus::Applied);
  // Placements: only the two singles may place; anything else is refused.
  for (Technique t : {Technique::Pointing, Technique::Claiming, Technique::NakedPair,
                      Technique::HiddenPair, Technique::NakedTriple, Technique::HiddenTriple,
                      Technique::XWing, unknown}) {
    CandidateGrid g = base;
    const AppliedDeduction a = apply(g, Deduction::place(t, 2, 1, pair_evidence(), "x"));
    CHECK(a.status == ApplyStatus::Rejected && g == base);
  }
}

}  // namespace

int main() {
  test_techniques();
  test_construction();
  test_apply();
  test_unsupported_techniques_cannot_mutate();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
