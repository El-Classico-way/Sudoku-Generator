#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/logical.hpp"
#include "sudoku/singles.hpp"
#include "sudoku/verify.hpp"

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

// Wikipedia example: solvable with singles alone.
const char* kWikiPuzzle =
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1"
    "7...2...6" ".6....28." "...419..5" "....8..79";
const char* kWikiSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791"
    "713924856" "961537284" "287419635" "345286179";
const char* kWikiAlt =  // a different valid grid (differs in four cells)
    "534768912" "672195348" "198342567" "859671423" "426853791"
    "713924856" "961537284" "287419635" "345286179";

// Generated puzzles. Each has exactly one solution, confirmed with an
// independent brute-force solver; `solution` is that solution. "Needs" means the
// default-order solver solves it, uses the technique, and STALLS when only that
// technique is switched off.
struct Fixture {
  const char* name;
  const char* puzzle;
  const char* solution;
  Technique technique;
  bool solved_by_default;  // otherwise the default order stalls
};
const Fixture kFixtures[] = {
    {"pointing",
     "........15..8......841.562.........7..86.9....3...1....2....1.9.1.53.28........3.",
     "293764851561823794784195623642358917158679342937241568325486179419537286876912435",
     Technique::Pointing, true},
    {"claiming",
     "2.9.6..4.4..3.1.....6.......9....5..8....94.1.3.6....9..3.92.....7....8.....5..1.",
     "219768345475321896386945172794213568862579431531684729143892657957136284628457913",
     Technique::Claiming, true},
    {"x-wing a",
     ".9......2.8.......1.2..3.89....4.8..4..9.8.25...6.5.....8.6.24.3.5....17.1.......",
     "593816472687429351142753689759142836436978125821635794978561243365284917214397568",
     Technique::XWing, true},
    {"x-wing b",
     "..7.........95.6..3..1...4.....4.......6.9..8....8547...14..75.5..8...19.9.....2.",
     "687234591142957683359168247238741965475629138916385472821493756564872319793516824",
     Technique::XWing, true},
    {"hidden pair",
     "6.........278..56..9......15......9.8..1.6.2....4......5.....4..6..3...737..2...9",
     "685741932127893564493265871516372498849156723732489615258917346961534287374628159",
     Technique::HiddenPair, true},
    {"hidden triple",
     "...7....3.569.2..88..36.9..6...7.....17.....248.....7.....3..5.9.4.28............",
     "249785163356912748871364925692871534517643892483259671728136459964528317135497286",
     Technique::HiddenTriple, true},
    // Used (not necessary: a hidden form covers the same situation):
    {"naked pair",
     "5..3.2.96...7...4.......3....9.....3.16.7.2..3.......1...5.4..9..2.1....65..8....",
     "581342796263791548497856312749125863816973254325468971178534629932617485654289137",
     Technique::NakedPair, true},
    {"naked triple",
     "..1...8..7394..2.6...76....8...46....76..1..3.45.3....2.....791.......6.....2....",
     "651392874739418256482765139823546917976281543145937628264853791598174362317629485",
     Technique::NakedTriple, false},
};
// Puzzles (unique solution each) on which the full technique set stalls.
const char* kStalled[][2] = {
    {".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4.",
     "529781634814623759736945218297316485341258976685479123178594362453162897962837541"},
    {"....5...4..823..1..7.8...2.9......32.659...4.2...7..6...1.4.....9......8.2.6.5...",
     "132756984648239715579814623987461532365982147214573869751348296496127358823695471"},
};
// Locally healthy start; one legal hidden single then leaves a digit with no
// location; the puzzle has no solution (independently confirmed).
const char* kContradictsAfterStep =
    "..............2..1.....12.....1....2..1.2.....2.....1....21.....1.....2...2...1..";
// Row 0 has empty cells 0,1,2 missing {5,7,8}; cells 0 and 1 can only be 5.
const char* kTwoSinglesCollide =
    "...123469" "........." "........." "7........" "8........"
    "........." ".7......." ".8......." "..5......";

LogicalOptions only(std::initializer_list<Technique> t) { LogicalOptions o; o.techniques = t; return o; }
LogicalOptions without(Technique skip) {
  LogicalOptions o;
  for (Technique t : default_technique_order()) if (t != skip) o.techniques.push_back(t);
  return o;
}
int givens_kept(const Board& puzzle, const Board& result) {
  int bad = 0;
  for (int i = 0; i < 81; ++i) if (puzzle.get(i) != 0 && puzzle.get(i) != result.get(i)) ++bad;
  return bad;
}

// Re-applies a recorded trace to a fresh grid: it must reproduce the result.
void check_trace(const Board& puzzle, const LogicalResult& r, const std::string& ctx) {
  CHECK_CTX(r.stats.deductions_applied == static_cast<int>(r.steps.size()), ctx);
  int by_sum = 0, placements = 0, eliminated = 0;
  for (int n : r.stats.by_technique) by_sum += n;
  CandidateGrid replay = *CandidateGrid::from_board(puzzle).grid;
  int previous = 0;
  for (int c = 0; c < 81; ++c) previous += replay.candidate_count(c);
  for (const AppliedDeduction& s : r.steps) {
    CHECK_CTX(s.status == ApplyStatus::Applied && !s.effective.empty(), ctx);
    if (s.deduction.action() == DeductionAction::PlaceDigit) ++placements;
    else eliminated += static_cast<int>(s.effective.size());
    const AppliedDeduction again = apply(replay, s.deduction);
    CHECK_CTX(again.status == ApplyStatus::Applied && again.effective == s.effective, ctx);
    int now = 0;
    for (int c = 0; c < 81; ++c) now += replay.candidate_count(c);
    CHECK_CTX(now < previous, ctx);  // strict progress at every step
    previous = now;
  }
  CHECK_CTX(by_sum == r.stats.deductions_applied, ctx);
  CHECK_CTX(placements == r.stats.placements && eliminated == r.stats.eliminated_candidates, ctx);
  CHECK_CTX(r.candidates && *r.candidates == replay, ctx);
  CHECK_CTX(r.board && replay.board() == *r.board, ctx);
}

// ---------- verifier ----------
void test_verifier() {
  const Board solved = parse(kWikiSolved);
  const Board none;
  CHECK(verify_solution(solved, none).valid());
  CHECK(verify_solution(solved, solved).valid());
  CHECK(verify_solution(solved, parse(kWikiPuzzle)).valid());

  Board b = solved;
  b.set(4, 4, 0);
  SolutionVerdict v = verify_solution(b, none);
  CHECK((v == SolutionVerdict{SolutionFault::EmptyCell, 40, 0}));
  CHECK(!verify_solution(Board{}, none).valid());

  b = solved;  b.set(0, 0, solved.get(0, 1));   // (0,0) copies (0,1)=3
  v = verify_solution(b, none);
  CHECK((v == SolutionVerdict{SolutionFault::RowViolation, 0, 3}));

  b = solved;  b.set(0, 0, 3);  b.set(0, 1, 5);  // swap inside row 0 and box 0
  v = verify_solution(b, none);
  CHECK((v == SolutionVerdict{SolutionFault::ColumnViolation, 0, 3}));

  b = solved;  // swap whole rows 0 and 3: rows and columns stay valid, boxes do not
  for (int c = 0; c < 9; ++c) { b.set(0, c, solved.get(3, c)); b.set(3, c, solved.get(0, c)); }
  v = verify_solution(b, none);
  CHECK((v == SolutionVerdict{SolutionFault::BoxViolation, 0, 3}));

  // A valid grid that disagrees with a given.
  const Board alt = parse(kWikiAlt);
  CHECK(alt.is_valid_solution());
  v = verify_solution(alt, solved);
  CHECK((v == SolutionVerdict{SolutionFault::GivenChanged, 3, 6}));
  CHECK(verify_solution(alt, none).valid());  // fine without that clue

  // The verifier agrees with an independent pairwise definition on perturbed grids.
  ref::Rng rng(5);
  int valid = 0, invalid = 0;
  for (int trial = 0; trial < 400; ++trial) {
    Board g = solved;
    const int edits = trial % 3;  // 0 edits keeps it valid
    for (int e = 0; e < edits; ++e) g.set(rng.below(81), 1 + rng.below(9));
    ref::Grid grid{};
    for (int i = 0; i < 81; ++i) grid[static_cast<std::size_t>(i)] = g.get(i);
    const bool expected = ref::consistent(grid);  // complete + no repeats == valid
    const SolutionVerdict got = verify_solution(g, none);
    CHECK_CTX(got.valid() == expected, ref::to_string(grid));
    (expected ? valid : invalid)++;
  }
  CHECK(valid > 50 && invalid > 50);
}

// ---------- configuration and inputs ----------
void test_inputs() {
  const Board puzzle = parse(kWikiPuzzle);
  const Board before = puzzle;
  CHECK(solve_logically(puzzle).status == LogicalStatus::Solved);
  CHECK(puzzle == before);  // never modified

  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  const LogicalResult inv = solve_logically(bad);
  CHECK(inv.status == LogicalStatus::InvalidInput && inv.reason == LogicalReason::InconsistentBoard);
  CHECK(!inv.board && !inv.candidates && inv.steps.empty() && !inv.conflicts.empty());

  // A complete board with a repeated digit is invalid input, never "solved".
  Board broken = parse(kWikiSolved);
  broken.set(0, 0, broken.get(0, 1));
  const LogicalResult br = solve_logically(broken);
  CHECK(br.status == LogicalStatus::InvalidInput && br.status != LogicalStatus::Solved);

  LogicalOptions dup = only({Technique::NakedSingle, Technique::NakedSingle});
  LogicalResult r = solve_logically(puzzle, dup);
  CHECK(r.status == LogicalStatus::InvalidInput && r.reason == LogicalReason::InvalidConfiguration);
  LogicalOptions unknown;
  unknown.techniques = {Technique::NakedSingle, static_cast<Technique>(99)};
  r = solve_logically(puzzle, unknown);
  CHECK(r.status == LogicalStatus::InvalidInput && r.reason == LogicalReason::InvalidConfiguration);
  CHECK(r.steps.empty() && !r.board);
  // Empty list == default order.
  CHECK(solve_logically(puzzle, LogicalOptions{}).stats.deductions_applied ==
        solve_logically(puzzle, LogicalOptions{default_technique_order()}).stats.deductions_applied);
  CHECK(default_technique_order().size() == static_cast<std::size_t>(kTechniqueCount));
}

// ---------- solved / singles ----------
void test_already_solved() {
  const Board solved = parse(kWikiSolved);
  const LogicalResult r = solve_logically(solved);
  CHECK(r.status == LogicalStatus::Solved && r.reason == LogicalReason::PuzzleSolved);
  CHECK(r.board && *r.board == solved && r.steps.empty() && r.stats.deductions_applied == 0);
  CHECK(verify_solution(*r.board, solved).valid());
}

void test_singles_puzzle() {
  const Board puzzle = parse(kWikiPuzzle);
  const Board solution = parse(kWikiSolved);
  const LogicalResult r = solve_logically(puzzle, only({Technique::NakedSingle, Technique::HiddenSingle}));
  CHECK(r.status == LogicalStatus::Solved && r.reason == LogicalReason::PuzzleSolved);
  CHECK(r.board && *r.board == solution && givens_kept(puzzle, *r.board) == 0);
  CHECK(verify_solution(*r.board, puzzle).valid());
  CHECK(r.stats.placements == 51 && r.stats.deductions_applied == 51 && r.stats.eliminated_candidates == 0);
  CHECK(r.stats.count(Technique::NakedSingle) + r.stats.count(Technique::HiddenSingle) == 51);
  CHECK(r.stats.count(Technique::XWing) == 0 && r.stats.count(Technique::Pointing) == 0);
  check_trace(puzzle, r, "singles");

  // The default order also solves it, preferring naked singles.
  const LogicalResult full = solve_logically(puzzle);
  CHECK(full.status == LogicalStatus::Solved && *full.board == solution);
  CHECK(full.stats.count(Technique::NakedSingle) > 0);
  check_trace(puzzle, full, "default");
  // Determinism.
  const LogicalResult again = solve_logically(puzzle);
  CHECK(again.steps.size() == full.steps.size());
  for (std::size_t i = 0; i < full.steps.size(); ++i) {
    CHECK(again.steps[i].deduction == full.steps[i].deduction && again.steps[i].effective == full.steps[i].effective);
  }
}

void test_technique_priority() {
  const Board puzzle = parse(kWikiPuzzle);
  const CandidateGrid grid = *CandidateGrid::from_board(puzzle).grid;
  const Deduction naked = find_naked_singles(grid).deductions.front();
  const Deduction hidden = find_hidden_singles(grid).deductions.front();
  CHECK(solve_logically(puzzle).steps.front().deduction == naked);
  CHECK(solve_logically(puzzle, only({Technique::HiddenSingle, Technique::NakedSingle})).steps.front().deduction == hidden);
  CHECK(solve_logically(puzzle, only({Technique::HiddenSingle})).steps.front().deduction == hidden);
}

// ---------- the techniques through the orchestration layer ----------
void test_techniques_through_solver() {
  for (const Fixture& f : kFixtures) {
    const std::string ctx = f.name;
    const Board puzzle = parse(f.puzzle);
    const Board solution = parse(f.solution);
    const LogicalResult r = solve_logically(puzzle);
    CHECK_CTX(r.stats.count(f.technique) > 0, ctx);
    check_trace(puzzle, r, ctx);
    CHECK_CTX(r.candidates && r.candidates->invariants_hold(), ctx);
    // Everything the solver did is sound w.r.t. the true solution.
    for (int c = 0; c < 81; ++c) {
      if (r.board->get(c) != 0) CHECK_CTX(r.board->get(c) == solution.get(c), ctx);
      else CHECK_CTX(r.candidates->is_candidate(c, solution.get(c)), ctx);
    }
    if (f.solved_by_default) {
      CHECK_CTX(r.status == LogicalStatus::Solved && *r.board == solution, ctx);
      CHECK_CTX(verify_solution(*r.board, puzzle).valid(), ctx);
      CHECK_CTX(givens_kept(puzzle, *r.board) == 0, ctx);
    } else {
      CHECK_CTX(r.status == LogicalStatus::Stalled && !r.board->is_complete(), ctx);
    }
    // The techniques that are necessary really are: without them it stalls.
    if (f.technique != Technique::NakedPair && f.technique != Technique::NakedTriple) {
      const LogicalResult lacking = solve_logically(puzzle, without(f.technique));
      CHECK_CTX(lacking.status == LogicalStatus::Stalled &&
                    lacking.reason == LogicalReason::NoApplicableDeduction, ctx);
      CHECK_CTX(lacking.stats.count(f.technique) == 0, ctx);
    }
  }
}

// ---------- stalled ----------
void test_stalled() {
  for (auto& s : kStalled) {
    const Board puzzle = parse(s[0]);
    const Board solution = parse(s[1]);
    const LogicalResult r = solve_logically(puzzle);
    CHECK(r.status == LogicalStatus::Stalled && r.reason == LogicalReason::NoApplicableDeduction);
    CHECK(r.status != LogicalStatus::Contradiction && r.status != LogicalStatus::Solved);
    CHECK(r.board && !r.board->is_complete() && r.stats.deductions_applied > 0);
    CHECK(r.contradictions.empty() && !r.refused);
    check_trace(puzzle, r, "stalled");
    // Not guessing: every placed digit is the true one, the true digit stays a
    // candidate everywhere, and the puzzle is still uniquely solvable.
    for (int c = 0; c < 81; ++c) {
      if (r.board->get(c) != 0) CHECK(r.board->get(c) == solution.get(c));
      else CHECK(r.candidates->is_candidate(c, solution.get(c)));
    }
  }
  // The empty board: valid, nothing to deduce.
  const LogicalResult empty = solve_logically(Board{});
  CHECK(empty.status == LogicalStatus::Stalled && empty.steps.empty());
  // A subset of techniques stalls where the full set does not.
  const Board hard = parse(kFixtures[0].puzzle);
  const LogicalResult singles = solve_logically(hard, only({Technique::NakedSingle, Technique::HiddenSingle}));
  CHECK(singles.status == LogicalStatus::Stalled && singles.reason == LogicalReason::NoApplicableDeduction);
}

// ---------- contradictions ----------
void test_contradictions() {
  // Contradictory from the start.
  const Board dead = parse("12345678." "........9" "........." "........." "........."
                           "........." "........." "........." ".........");
  LogicalResult r = solve_logically(dead);
  CHECK(r.status == LogicalStatus::Contradiction && r.reason == LogicalReason::ContradictoryInitialState);
  CHECK(!r.contradictions.empty() && r.steps.empty() && r.board && r.board->get(8) == 0);
  CHECK(r.status != LogicalStatus::Stalled && r.status != LogicalStatus::InvalidInput);

  // Contradiction that only appears after a legal deduction.
  const Board puzzle = parse(kContradictsAfterStep);
  const CandidateGrid start = *CandidateGrid::from_board(puzzle).grid;
  CHECK(!start.has_contradiction());  // the start looked healthy
  r = solve_logically(puzzle);
  CHECK(r.status == LogicalStatus::Contradiction && r.reason == LogicalReason::ContradictionAfterDeduction);
  CHECK(r.steps.size() == 1 && !r.contradictions.empty());
  CHECK(r.candidates && r.candidates->has_contradiction());
  CHECK(!r.board->is_complete() && r.status != LogicalStatus::Solved);
  check_trace(puzzle, r, "after deduction");

  // A sound deduction that cannot be carried out: cells 0 and 1 both need a 5.
  const Board collide = parse(kTwoSinglesCollide);
  CHECK(!CandidateGrid::from_board(collide).grid->has_contradiction());
  r = solve_logically(collide);
  CHECK(r.status == LogicalStatus::Contradiction && r.reason == LogicalReason::DeductionWouldContradict);
  CHECK(r.refused && r.refused->status == ApplyStatus::WouldContradict);
  CHECK(r.steps.empty() && r.board && *r.board == collide);  // refused: nothing partially applied
  CHECK(r.candidates && *r.candidates == *CandidateGrid::from_board(collide).grid);

  // Contradictions are found regardless of the enabled techniques.
  CHECK(solve_logically(dead, only({Technique::XWing})).status == LogicalStatus::Contradiction);
}

// ---------- soundness / termination sweep ----------
void test_sweep() {
  const Board solution = parse(kWikiSolved);
  ref::Rng rng(2024);
  int solved = 0, stalled = 0, total_steps = 0;
  for (int trial = 0; trial < 60; ++trial) {
    Board puzzle = solution;
    const int blanks = 35 + rng.below(35);
    for (int k = 0; k < blanks; ++k) puzzle.set(rng.below(81), 0);
    const std::string ctx = "trial " + std::to_string(trial) + " " + ref::to_string([&] {
      ref::Grid g{}; for (int i = 0; i < 81; ++i) g[static_cast<std::size_t>(i)] = puzzle.get(i); return g; }());
    const LogicalResult r = solve_logically(puzzle);
    CHECK_CTX(r.status == LogicalStatus::Solved || r.status == LogicalStatus::Stalled, ctx);
    CHECK_CTX(r.steps.size() <= 729, ctx);  // the documented bound
    total_steps += static_cast<int>(r.steps.size());
    (r.status == LogicalStatus::Solved ? solved : stalled)++;
    if (r.status == LogicalStatus::Solved) {
      CHECK_CTX(*r.board == solution && verify_solution(*r.board, puzzle).valid(), ctx);
    }
    for (int c = 0; c < 81; ++c) {
      if (r.board->get(c) != 0) CHECK_CTX(r.board->get(c) == solution.get(c), ctx);
      else CHECK_CTX(r.candidates->is_candidate(c, solution.get(c)), ctx);
    }
    check_trace(puzzle, r, ctx);
    CHECK_CTX(solve_logically(puzzle).steps.size() == r.steps.size(), ctx);  // deterministic
  }
  CHECK(solved > 5 && stalled > 5 && total_steps > 500);
  std::printf("logical sweep: %d solved, %d stalled, %d steps\n", solved, stalled, total_steps);
}

}  // namespace

int main() {
  test_verifier();
  test_inputs();
  test_already_solved();
  test_singles_puzzle();
  test_technique_priority();
  test_techniques_through_solver();
  test_stalled();
  test_contradictions();
  test_sweep();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
