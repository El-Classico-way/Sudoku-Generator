#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
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

// Wikipedia example puzzle and its solution (confirmed independently in
// solver_test.cpp).
const char* kPuzzle =
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1"
    "7...2...6" ".6....28." "...419..5" "....8..79";
const char* kSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791"
    "713924856" "961537284" "287419635" "345286179";
const char* kDeadCell =  // cell 8 has no candidates
    "12345678." "........9" "........." "........." "........."
    "........." "........." "........." ".........";

CandidateGrid grid_of(const Board& b) {
  CandidateInitResult r = CandidateGrid::from_board(b);
  CHECK(r.status == InitStatus::Ok);
  return *r.grid;
}
CandidateGrid empty_grid() { return grid_of(Board{}); }

// Keep only `keep` as a candidate of `cell`.
void keep_only(CandidateGrid& g, int cell, int keep) {
  for (int d = 1; d <= 9; ++d) {
    if (d != keep) CHECK(g.eliminate(cell, d) != EliminateOutcome::WouldEmptyCell);
  }
}
// Remove `digit` from every cell of `unit` except `except` (-1 = none).
void strip(CandidateGrid& g, const Unit& unit, int digit, int except) {
  for (int cell : unit_cells(unit)) {
    if (cell != except) CHECK(g.eliminate(cell, digit) == EliminateOutcome::Eliminated);
  }
}

bool is_place(const Deduction& d, Technique t, int cell, int digit) {
  return d.technique() == t && d.action() == DeductionAction::PlaceDigit &&
         d.targets().size() == 1 && d.targets()[0] == Elimination{cell, digit};
}

// ---- independent expectations (naive, pairwise) ----
struct Expect { Technique t; int cell, digit; std::optional<Unit> unit; };

std::vector<Expect> reference_singles(const Board& b) {
  ref::Grid g{};
  for (int i = 0; i < 81; ++i) g[static_cast<std::size_t>(i)] = b.get(i);
  auto cands = [&](int cell) {
    std::vector<int> out;
    if (g[static_cast<std::size_t>(cell)] == 0)
      for (int d = 1; d <= 9; ++d) if (ref::placeable(g, cell, d)) out.push_back(d);
    return out;
  };
  std::vector<Expect> out;
  for (int c = 0; c < 81; ++c) {
    const auto cs = cands(c);
    if (cs.size() == 1) out.push_back({Technique::NakedSingle, c, cs[0], std::nullopt});
  }
  for (int kind = 0; kind < 3; ++kind) {
    for (int u = 0; u < 9; ++u) {
      for (int d = 1; d <= 9; ++d) {
        bool placed = false;
        std::vector<int> where;
        for (int c = 0; c < 81; ++c) {
          const int r = c / 9, col = c % 9, bx = (r / 3) * 3 + col / 3;
          const int mine = kind == 0 ? r : kind == 1 ? col : bx;
          if (mine != u) continue;
          if (g[static_cast<std::size_t>(c)] == d) placed = true;
          const auto cs = cands(c);
          if (std::find(cs.begin(), cs.end(), d) != cs.end()) where.push_back(c);
        }
        if (!placed && where.size() == 1) {
          const UnitKind k = kind == 0 ? UnitKind::Row : kind == 1 ? UnitKind::Column : UnitKind::Box;
          out.push_back({Technique::HiddenSingle, where[0], d, Unit{k, u}});
        }
      }
    }
  }
  return out;
}

void check_matches_reference(const Board& b, const std::string& ctx) {
  const DetectResult r = find_singles(b);
  CHECK_CTX(r.status == DetectStatus::Ok, ctx);
  const auto want = reference_singles(b);
  CHECK_CTX(r.deductions.size() == want.size(), ctx);
  for (std::size_t i = 0; i < std::min(want.size(), r.deductions.size()); ++i) {
    const Deduction& d = r.deductions[i];
    CHECK_CTX(is_place(d, want[i].t, want[i].cell, want[i].digit), ctx);
    CHECK_CTX(d.evidence().unit == want[i].unit, ctx);
    CHECK_CTX(d.evidence().cells == std::vector<int>{want[i].cell}, ctx);
    CHECK_CTX(d.evidence().digits.digits() == (want[i].t == Technique::HiddenSingle
                  ? std::vector<int>{want[i].digit}
                  : d.evidence().digits.digits()), ctx);
    CHECK_CTX(!d.explanation().empty(), ctx);
  }
}

// ---- tests ----
void test_naked_handcrafted() {
  CandidateGrid g = empty_grid();
  keep_only(g, 10, 7);
  DetectResult r = find_naked_singles(g);
  CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 1);
  if (r.deductions.size() == 1) {
    const Deduction& d = r.deductions[0];
    CHECK(is_place(d, Technique::NakedSingle, 10, 7));
    CHECK(!d.evidence().unit.has_value());
    CHECK(d.evidence().cells == std::vector<int>{10});
    CHECK(d.evidence().digits.digits() == std::vector<int>{7});
  }
  // Two and three candidates are not naked singles.
  CandidateGrid h = empty_grid();
  for (int d = 3; d <= 9; ++d) h.eliminate(4, d);   // cell 4 -> {1,2}
  for (int d = 4; d <= 9; ++d) h.eliminate(5, d);   // cell 5 -> {1,2,3}
  CHECK(h.candidate_count(4) == 2 && h.candidate_count(5) == 3);
  CHECK(find_naked_singles(h).deductions.empty());
  CHECK(find_naked_singles(h).status == DetectStatus::Ok);
  // Filled cells are never reported, even though they have a single value.
  const CandidateGrid solved = grid_of(parse(kSolved));
  const DetectResult s = find_singles(solved);
  CHECK(s.status == DetectStatus::Ok && s.deductions.empty());
  // Several, in ascending cell order regardless of the order they were made.
  CandidateGrid m = empty_grid();
  keep_only(m, 50, 2);
  keep_only(m, 3, 9);
  keep_only(m, 77, 1);
  const auto many = find_naked_singles(m).deductions;
  CHECK(many.size() == 3);
  if (many.size() == 3) {
    CHECK(is_place(many[0], Technique::NakedSingle, 3, 9));
    CHECK(is_place(many[1], Technique::NakedSingle, 50, 2));
    CHECK(is_place(many[2], Technique::NakedSingle, 77, 1));
  }
}

void test_hidden_every_unit() {
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < 9; ++u) {
      const Unit unit{kind, u};
      const auto cells = unit_cells(unit);
      for (int k : {0, 4, 8}) {  // first, middle and last cell of the unit
        const int target = cells[static_cast<std::size_t>(k)];
        CandidateGrid g = empty_grid();
        strip(g, unit, 5, target);
        const std::string ctx = "unit " + std::to_string(static_cast<int>(kind)) + "/" +
                                std::to_string(u) + " k=" + std::to_string(k);
        CHECK_CTX(find_naked_singles(g).deductions.empty(), ctx);
        const DetectResult r = find_hidden_singles(g);
        CHECK_CTX(r.status == DetectStatus::Ok && r.deductions.size() == 1, ctx);
        if (r.deductions.size() != 1) continue;
        const Deduction& d = r.deductions[0];
        CHECK_CTX(is_place(d, Technique::HiddenSingle, target, 5), ctx);
        CHECK_CTX(d.evidence().unit == std::optional<Unit>(unit), ctx);
        CHECK_CTX(d.evidence().cells == std::vector<int>{target}, ctx);
        CHECK_CTX(d.evidence().digits.digits() == std::vector<int>{5}, ctx);
        CHECK_CTX(find_singles(g).deductions == r.deductions, ctx);
      }
    }
  }
}

void test_hidden_negative_cases() {
  const Unit row0{UnitKind::Row, 0};
  // Zero locations: contradiction, never a single.
  {
    CandidateGrid g = empty_grid();
    strip(g, row0, 5, -1);
    for (const DetectResult& r : {find_hidden_singles(g), find_singles(g), find_naked_singles(g)}) {
      CHECK(r.status == DetectStatus::Contradiction && r.deductions.empty());
      CHECK(r.contradictions.size() == 1);
      if (r.contradictions.size() == 1) {
        CHECK(r.contradictions[0].kind == ContradictionKind::DigitHasNoLocation);
        CHECK((r.contradictions[0].unit == row0) && r.contradictions[0].digit == 5);
      }
    }
  }
  // Two locations: no deduction, healthy state.
  {
    CandidateGrid g = empty_grid();
    const auto cells = unit_cells(row0);
    for (int i = 2; i < 9; ++i) g.eliminate(cells[static_cast<std::size_t>(i)], 5);
    const DetectResult r = find_singles(g);
    CHECK(r.status == DetectStatus::Ok && r.deductions.empty());
    CHECK(g.cells_with_candidate(row0, 5).size() == 2);
  }
  // Digit already placed in the unit: never a hidden single there, even if
  // only one empty cell is left in the unit that could hold it.
  {
    Board b;
    b.set(0, 0, 5);  // row 0, column 0 and box 0 now contain 5
    const DetectResult r = find_hidden_singles(grid_of(b));
    for (const Deduction& d : r.deductions) {
      const Unit u = *d.evidence().unit;
      const auto members = unit_cells(u);
      for (int m : members) CHECK(!(b.get(m) == d.targets()[0].digit));
    }
  }
}

void test_overlap_policy_and_multiple() {
  // Naked AND hidden for the same placement: both are reported, naked first,
  // each with its own technique and evidence.
  CandidateGrid g = empty_grid();
  keep_only(g, 0, 5);
  strip(g, Unit{UnitKind::Row, 0}, 5, 0);
  const DetectResult r = find_singles(g);
  CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 2);
  if (r.deductions.size() == 2) {
    CHECK(is_place(r.deductions[0], Technique::NakedSingle, 0, 5));
    CHECK(!r.deductions[0].evidence().unit.has_value());
    CHECK(is_place(r.deductions[1], Technique::HiddenSingle, 0, 5));
    CHECK((r.deductions[1].evidence().unit == std::optional<Unit>(Unit{UnitKind::Row, 0})));
  }
  CHECK(find_naked_singles(g).deductions.size() == 1);
  CHECK(find_hidden_singles(g).deductions.size() == 1);

  // Two hidden singles in one unit, ordered by digit.
  CandidateGrid m = empty_grid();
  const Unit row0{UnitKind::Row, 0};
  strip(m, row0, 7, 6);
  strip(m, row0, 3, 1);
  const auto hs = find_hidden_singles(m).deductions;
  CHECK(hs.size() == 2);
  if (hs.size() == 2) {
    CHECK(is_place(hs[0], Technique::HiddenSingle, 1, 3));
    CHECK(is_place(hs[1], Technique::HiddenSingle, 6, 7));
    CHECK((hs[0].evidence().unit == std::optional<Unit>(row0)));
  }
  // Same placement hidden in row AND column: two distinct deductions, row first.
  CandidateGrid x = empty_grid();
  strip(x, Unit{UnitKind::Row, 0}, 4, 0);
  strip(x, Unit{UnitKind::Column, 0}, 4, 0);
  const auto both = find_hidden_singles(x).deductions;
  CHECK(both.size() == 2);
  if (both.size() == 2) {
    CHECK((both[0].evidence().unit == std::optional<Unit>(Unit{UnitKind::Row, 0})));
    CHECK((both[1].evidence().unit == std::optional<Unit>(Unit{UnitKind::Column, 0})));
  }
}

void test_differential_and_soundness() {
  check_matches_reference(parse(kPuzzle), "wikipedia puzzle");
  CHECK(!find_singles(parse(kPuzzle)).deductions.empty());
  // Cell 40 (row 4, column 4) is the classic naked single 5 of this puzzle.
  const auto wiki = find_naked_singles(grid_of(parse(kPuzzle))).deductions;
  CHECK(std::any_of(wiki.begin(), wiki.end(), [](const Deduction& d) {
    return is_place(d, Technique::NakedSingle, 40, 5); }));

  // Boards blanked from a known solution: results equal the naive reference,
  // and every single is SOUND (its digit equals the known solution's digit).
  const Board solution = parse(kSolved);
  ref::Rng rng(99);
  int deductions_seen = 0;
  for (int trial = 0; trial < 120; ++trial) {
    Board b = solution;
    const int blanks = 15 + rng.below(45);
    for (int k = 0; k < blanks; ++k) b.set(rng.below(81), 0);
    const std::string ctx = "trial " + std::to_string(trial);
    check_matches_reference(b, ctx);
    for (const Deduction& d : find_singles(b).deductions) {
      ++deductions_seen;
      CHECK_CTX(d.targets()[0].digit == solution.get(d.targets()[0].cell), ctx);
      CHECK_CTX(b.get(d.targets()[0].cell) == 0, ctx);
    }
  }
  CHECK(deductions_seen > 200);
}

void test_detection_is_pure() {
  const Board board = parse(kPuzzle);
  const CandidateGrid grid = grid_of(board);
  const CandidateGrid grid_before = grid;
  const Board board_before = board;
  const DetectResult a = find_singles(grid);
  const DetectResult b = find_singles(grid);
  const DetectResult c = find_singles(board);
  CHECK(a.deductions == b.deductions && a.deductions == c.deductions);
  CHECK(grid == grid_before && board == board_before);
  (void)find_naked_singles(grid);
  (void)find_hidden_singles(grid);
  CHECK(grid == grid_before);
}

void test_error_statuses() {
  const DetectResult dead = find_singles(parse(kDeadCell));
  CHECK(dead.status == DetectStatus::Contradiction && dead.deductions.empty());
  CHECK(dead.contradictions.size() == 2);
  CHECK(dead.contradictions[0].kind == ContradictionKind::EmptyCellNoCandidates);
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  const DetectResult inv = find_singles(bad);
  CHECK(inv.status == DetectStatus::InvalidBoard && inv.deductions.empty());
  CHECK(inv.conflicts == bad.find_conflicts() && !inv.conflicts.empty());
  CHECK(inv.contradictions.empty());
  // A healthy state with nothing to find is Ok + empty, not an error.
  const DetectResult none = find_singles(Board{});
  CHECK(none.status == DetectStatus::Ok && none.deductions.empty());
}

// Compare a grid before/after placing `digit` at `cell`.
void check_placement_effect(const CandidateGrid& before, const CandidateGrid& after,
                            int cell, int digit) {
  CHECK(after.board().get(cell) == digit);
  for (int i = 0; i < 81; ++i) {
    if (i != cell) CHECK(after.board().get(i) == before.board().get(i));
  }
  CHECK(after.candidates(cell).empty());
  for (int i = 0; i < 81; ++i) {
    if (i == cell) continue;
    const bool peer = cell_row(i) == cell_row(cell) || cell_column(i) == cell_column(cell) ||
                      cell_box(i) == cell_box(cell);
    DigitSet want = before.candidates(i);
    if (peer && before.board().get(i) == 0) want.erase(digit);
    CHECK(after.candidates(i) == want);
  }
  CHECK(after.invariants_hold());
}

void test_apply_naked_single() {
  CandidateGrid grid = grid_of(parse(kPuzzle));
  // Make an earlier elimination in a peer, which must not be undone.
  // An empty peer in row 4 with at least three candidates, so dropping one
  // non-5 candidate leaves it comfortably non-empty.
  int peer = -1, dropped = 0;
  for (int cell : unit_cells(Unit{UnitKind::Row, 4})) {
    if (cell != 40 && grid.board().get(cell) == 0 && grid.candidate_count(cell) >= 3) {
      peer = cell;
      for (int d : grid.candidates(cell).digits()) if (d != 5) { dropped = d; break; }
      break;
    }
  }
  CHECK(peer >= 0 && dropped != 0);
  CHECK(grid.eliminate(peer, dropped) == EliminateOutcome::Eliminated);

  const std::vector<Deduction> nakeds = find_naked_singles(grid).deductions;
  const auto it = std::find_if(nakeds.begin(), nakeds.end(), [](const Deduction& x) {
    return is_place(x, Technique::NakedSingle, 40, 5); });
  CHECK(it != nakeds.end());
  if (it == nakeds.end()) return;
  const Deduction d = *it;
  const CandidateGrid before = grid;
  const AppliedDeduction a = apply(grid, d);
  CHECK(a.status == ApplyStatus::Applied && a.deduction == d);
  check_placement_effect(before, grid, 40, 5);
  CHECK(!grid.is_candidate(peer, dropped));  // still eliminated
  CHECK(!grid.is_candidate(peer, 5));
  // `effective` lists exactly the removed candidates, cell then digit ascending.
  CHECK(!a.effective.empty());
  CHECK(std::is_sorted(a.effective.begin(), a.effective.end(), [](auto x, auto y) {
    return x.cell != y.cell ? x.cell < y.cell : x.digit < y.digit; }));
  CHECK(std::find(a.effective.begin(), a.effective.end(), Elimination{40, 5}) != a.effective.end());
  // The caller's own Board was never touched: grid.board() is its own copy.
  CHECK(parse(kPuzzle).get(40) == 0);
}

void test_apply_hidden_single() {
  CandidateGrid grid = empty_grid();
  const Unit col3{UnitKind::Column, 3};
  strip(grid, col3, 8, 30);  // digit 8 only at cell 30 (row 3, col 3, box 4)
  const DetectResult r = find_hidden_singles(grid);
  CHECK(r.deductions.size() == 1);
  if (r.deductions.size() != 1) return;
  const CandidateGrid before = grid;
  const AppliedDeduction a = apply(grid, r.deductions[0]);
  CHECK(a.status == ApplyStatus::Applied);
  check_placement_effect(before, grid, 30, 8);
  // After the placement the hidden single is gone and the state is healthy.
  CHECK(find_singles(grid).status == DetectStatus::Ok);
  CHECK(find_hidden_singles(grid).deductions.empty());
}

void test_apply_rejections() {
  const CandidateGrid base = grid_of(parse(kPuzzle));
  auto run = [&](const Deduction& d, ApplyStatus want, const char* what) {
    CandidateGrid g = base;
    const AppliedDeduction a = apply(g, d);
    CHECK_CTX(a.status == want, what);
    CHECK_CTX(a.effective.empty(), what);
    CHECK_CTX(g == base, what);  // nothing changed on failure
  };
  Evidence ev;  ev.cells = {40};  ev.digits.insert(5);
  // Digit not a candidate of the (empty) cell.
  run(Deduction::place(Technique::NakedSingle, 40, 4, ev, "x"), ApplyStatus::Rejected, "not candidate");
  // Filled cell (cell 0 holds 5).
  run(Deduction::place(Technique::NakedSingle, 0, 5, ev, "x"), ApplyStatus::Rejected, "filled");
  // A technique that is not allowed to place.
  run(Deduction::place(Technique::XWing, 40, 5, ev, "x"), ApplyStatus::Rejected, "technique");
  // Naked single claimed for a cell with several candidates (cell 2: {1,2,4}).
  run(Deduction::place(Technique::NakedSingle, 2, 4, ev, "x"), ApplyStatus::Unsupported, "not naked");
  // Hidden single without a unit, with a unit not containing the cell, and
  // with a unit where the digit has two locations or is already placed.
  run(Deduction::place(Technique::HiddenSingle, 2, 4, Evidence{}, "x"), ApplyStatus::Unsupported, "no unit");
  Evidence wrong = ev;  wrong.unit = Unit{UnitKind::Row, 8};
  run(Deduction::place(Technique::HiddenSingle, 2, 4, wrong, "x"), ApplyStatus::Unsupported, "unit lacks cell");
  Evidence loose = ev;  loose.unit = Unit{UnitKind::Row, 0};
  run(Deduction::place(Technique::HiddenSingle, 2, 4, loose, "x"), ApplyStatus::Unsupported, "two locations");

  // Foreign/stale: cell 2 is a naked single {4} in a different state, but in
  // this one it still has {1,2,4}: the digit is a candidate, the evidence is
  // false.
  CandidateGrid other = *CandidateGrid::from_board(Board{}).grid;
  keep_only(other, 2, 4);
  const DetectResult foreign = find_naked_singles(other);
  CHECK(foreign.deductions.size() == 1);
  if (foreign.deductions.size() == 1) {
    CHECK(is_place(foreign.deductions[0], Technique::NakedSingle, 2, 4));
    run(foreign.deductions[0], ApplyStatus::Unsupported, "foreign naked single");
  }
}

void test_apply_after_state_change() {
  CandidateGrid grid = grid_of(parse(kPuzzle));
  const auto found = find_singles(grid).deductions;
  CHECK(found.size() >= 2);
  // Apply the first; re-applying it must now be refused (cell filled).
  CHECK(apply(grid, found[0]).status == ApplyStatus::Applied);
  const CandidateGrid after_first = grid;
  const AppliedDeduction again = apply(grid, found[0]);
  CHECK(again.status == ApplyStatus::Rejected);
  CHECK(grid == after_first);
  // Every other stale entry is either still valid or refused; whichever, the
  // grid invariants hold and refusals leave it untouched.
  for (std::size_t i = 1; i < found.size(); ++i) {
    const CandidateGrid before = grid;
    const AppliedDeduction a = apply(grid, found[i]);
    if (a.status != ApplyStatus::Applied) CHECK(grid == before);
    CHECK(grid.invariants_hold());
  }
}

void test_contradicting_placement() {
  // Cells 0 and 1 (same row) both have 5 as their only candidate: each is a
  // naked single, but placing either would empty the other.
  CandidateGrid g = empty_grid();
  keep_only(g, 0, 5);
  keep_only(g, 1, 5);
  const DetectResult r = find_naked_singles(g);
  CHECK(r.status == DetectStatus::Ok && r.deductions.size() == 2);
  CHECK(g.place(0, 5) == PlaceOutcome::WouldEmptyPeer);
  const CandidateGrid before = g;
  const AppliedDeduction a = apply(g, r.deductions[0]);
  CHECK(a.status == ApplyStatus::WouldContradict && a.effective.empty());
  CHECK(g == before);
}

void test_place_direct() {
  CandidateGrid g = grid_of(parse(kPuzzle));
  CHECK_THROWS_AS(g.place(-1, 5), std::out_of_range);
  CHECK_THROWS_AS(g.place(81, 5), std::out_of_range);
  CHECK_THROWS_AS(g.place(40, 0), std::invalid_argument);
  CHECK_THROWS_AS(g.place(40, 10), std::invalid_argument);
  const CandidateGrid before = g;
  CHECK(g.place(0, 5) == PlaceOutcome::FilledCell);
  CHECK(g.place(40, 4) == PlaceOutcome::NotACandidate);
  CHECK(g == before);
  CHECK(g.place(40, 5) == PlaceOutcome::Placed);
  CHECK(g.place(40, 5) == PlaceOutcome::FilledCell);
  CHECK(g.board().get(40) == 5);
  // Input Board object used to build the grid is unaffected.
  const Board original = parse(kPuzzle);
  CandidateGrid h = grid_of(original);
  h.place(40, 5);
  CHECK(original.get(40) == 0 && !h.matches(original));
}

// Soundness + progress: repeatedly applying detected singles (test-side loop;
// not a production solver) solves this easy puzzle, and every placement is
// the true digit of the known solution.
void test_singles_solve_wikipedia_puzzle() {
  const Board solution = parse(kSolved);
  CandidateGrid g = grid_of(parse(kPuzzle));
  int steps = 0;
  while (true) {
    const DetectResult r = find_singles(g);
    CHECK(r.status == DetectStatus::Ok);
    if (r.deductions.empty()) break;
    const Deduction& d = r.deductions.front();
    CHECK(d.targets()[0].digit == solution.get(d.targets()[0].cell));
    CHECK(apply(g, d).status == ApplyStatus::Applied);
    CHECK(g.invariants_hold() && !g.has_contradiction());
    CHECK(++steps <= 81);
  }
  CHECK(g.board() == solution);
  CHECK(g.board().is_valid_solution());
}

}  // namespace

int main() {
  test_naked_handcrafted();
  test_hidden_every_unit();
  test_hidden_negative_cases();
  test_overlap_policy_and_multiple();
  test_differential_and_soundness();
  test_detection_is_pure();
  test_error_statuses();
  test_apply_naked_single();
  test_apply_hidden_single();
  test_apply_rejections();
  test_apply_after_state_change();
  test_contradicting_placement();
  test_place_direct();
  test_singles_solve_wikipedia_puzzle();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
