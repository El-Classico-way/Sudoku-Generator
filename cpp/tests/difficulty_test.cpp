#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "reference.hpp"
#include "sudoku/difficulty.hpp"

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

// Every puzzle below has exactly one solution, confirmed with an independent
// brute-force solver. Expected classes were observed from the solver and each is
// backed by explicit checks that the easier profiles stall.
const char* kWiki =  // solved by singles
    "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1" "7...2...6" ".6....28." "...419..5" "....8..79";
const char* kWikiSolved =
    "534678912" "672195348" "198342567" "859761423" "426853791" "713924856" "961537284" "287419635" "345286179";
const char* kNakedOnly =  // solved by naked singles alone (no hidden single is ever needed)
    "9...7.3......1.......4.....36...4...7...9.8.1.15.......43.....6...6.1..4.5.3..9..";
const char* kMedium1 =   // singles stall; one pointing elimination unlocks it
    "5...2.8671...........45..1......7....6......443....2..81.6........179.5..79..5..1";
const char* kMedium2 =
    "3....5..7.8.2....5.51.47.6.4.....3...9.8..5.2.1......8..9..6......51...6.........";
const char* kHardNakedPair =
    "5..3.2.96...7...4.......3....9.....3.16.7.2..3.......1...5.4..9..2.1....65..8....";
const char* kHardHiddenPair =
    "6.........278..56..9......15......9.8..1.6.2....4......5.....4..6..3...737..2...9";
const char* kHardPointing =  // needs pointing AND pairs, so it is Hard, not Medium
    "........15..8......841.562.........7..86.9....3...1....2....1.9.1.53.28........3.";
const char* kExpertHiddenTriple =
    "...7....3.569.2..88..36.9..6...7.....17.....248.....7.....3..5.9.4.28............";
const char* kMasterXWingA =
    ".9......2.8.......1.2..3.89....4.8..4..9.8.25...6.5.....8.6.24.3.5....17.1.......";
const char* kMasterXWingB =
    "..7.........95.6..3..1...4.....4.......6.9..8....8547...14..75.5..8...19.9.....2.";
const char* kStalled[] = {  // valid, uniquely solvable, but no profile solves them
    "..1...8..7394..2.6...76....8...46....76..1..3.45.3....2.....791.......6.....2....",
    ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4.",
    "....5...4..823..1..7.8...2.9......32.659...4.2...7..6...1.4.....9......8.2.6.5...",
};
const char* kContradictsAfterStep =
    "..............2..1.....12.....1....2..1.2.....2.....1....21.....1.....2...2...1..";
const char* kTwoSinglesCollide =
    "...123469" "........." "........." "7........" "8........" "........." ".7......." ".8......." "..5......";
const char* kDeadCell =
    "12345678." "........9" "........." "........." "........." "........." "........." "........." ".........";

struct Expected { const char* name; const char* puzzle; const char* profile; int index; };
const Expected kClassified[] = {
    {"wiki", kWiki, "Easy", 0},           {"naked only", kNakedOnly, "Easy", 0},
    {"medium 1", kMedium1, "Medium", 1},  {"medium 2", kMedium2, "Medium", 1},
    {"naked pair", kHardNakedPair, "Hard", 2},
    {"hidden pair", kHardHiddenPair, "Hard", 2},
    {"pointing+pairs", kHardPointing, "Hard", 2},
    {"hidden triple", kExpertHiddenTriple, "Expert", 3},
    {"x-wing a", kMasterXWingA, "Master", 4}, {"x-wing b", kMasterXWingB, "Master", 4},
};

// A trace step with the given deduction and effective eliminations.
AppliedDeduction step(Deduction d, int effective, ApplyStatus status = ApplyStatus::Applied) {
  AppliedDeduction a{std::move(d), status, {}};
  for (int i = 0; i < effective; ++i) a.effective.push_back({i, 1});
  return a;
}
Deduction place(Technique t, int cell, int digit) { return Deduction::place(t, cell, digit, Evidence{}, "x"); }
Deduction eliminate(Technique t, std::vector<Elimination> e) { return Deduction::eliminate(t, std::move(e), Evidence{}, "x"); }

// ---------- A. metrics ----------
void test_levels() {
  const std::vector<std::pair<Technique, int>> table = {
      {Technique::NakedSingle, 1}, {Technique::HiddenSingle, 1}, {Technique::Pointing, 2},
      {Technique::Claiming, 2},    {Technique::NakedPair, 3},    {Technique::HiddenPair, 3},
      {Technique::NakedTriple, 4}, {Technique::HiddenTriple, 4}, {Technique::XWing, 5}};
  CHECK(table.size() == static_cast<std::size_t>(kTechniqueCount));
  for (auto [t, level] : table) CHECK(technique_level(t) == level);
  CHECK(technique_level(static_cast<Technique>(99)) == 0);
}

void test_metrics_known_trace() {
  const std::vector<AppliedDeduction> trace = {
      step(place(Technique::NakedSingle, 0, 1), 5),                       // placement: 5 side-effect removals
      step(eliminate(Technique::Pointing, {{1, 2}, {2, 2}, {3, 2}}), 3),  // 3 eliminations
      step(eliminate(Technique::XWing, {{4, 3}, {5, 3}}), 2),             // 2 eliminations
      step(eliminate(Technique::Pointing, {{6, 4}}), 1),                  // 1 elimination
      step(place(Technique::HiddenSingle, 7, 5), 4),                      // placement
  };
  const SolveMetrics m = compute_metrics(trace);
  CHECK(m.deductions == 5);
  CHECK(m.placements == 2);
  CHECK(m.eliminated_candidates == 6);  // 3 + 2 + 1; the placements' 5 + 4 are NOT included
  CHECK(m.count(Technique::NakedSingle) == 1 && m.count(Technique::HiddenSingle) == 1);
  CHECK(m.count(Technique::Pointing) == 2 && m.count(Technique::XWing) == 1);
  CHECK(m.count(Technique::Claiming) == 0 && m.count(Technique::NakedPair) == 0);
  CHECK(m.hardest_technique == std::optional<Technique>(Technique::XWing) && m.hardest_level == 5);
  int by_sum = 0;
  for (int n : m.by_technique) by_sum += n;
  CHECK(by_sum == m.deductions);  // every deduction counted exactly once
  CHECK(m.deductions == m.placements + 3);  // 3 elimination deductions
  // Deterministic and a pure function of the trace.
  CHECK(compute_metrics(trace) == m);
}

void test_metrics_edge_cases() {
  const SolveMetrics empty = compute_metrics({});
  CHECK(empty == SolveMetrics{});
  CHECK(empty.deductions == 0 && empty.placements == 0 && empty.eliminated_candidates == 0);
  CHECK(!empty.hardest_technique && empty.hardest_level == 0);

  // A placement's side-effect removals never leak into eliminated_candidates.
  const SolveMetrics only_placement = compute_metrics({step(place(Technique::NakedSingle, 0, 1), 30)});
  CHECK(only_placement.placements == 1 && only_placement.eliminated_candidates == 0);

  // Steps that did not change state are not counted.
  const SolveMetrics ignoring = compute_metrics({
      step(eliminate(Technique::NakedPair, {{1, 2}}), 1),
      step(eliminate(Technique::XWing, {{2, 3}}), 0, ApplyStatus::NoChange),
      step(eliminate(Technique::HiddenTriple, {{3, 4}}), 0, ApplyStatus::Unsupported)});
  CHECK(ignoring.deductions == 1 && ignoring.hardest_level == 3);
  CHECK(ignoring.hardest_technique == std::optional<Technique>(Technique::NakedPair));

  // Ties within a level go to the later technique; level beats order of appearance.
  CHECK(compute_metrics({step(place(Technique::HiddenSingle, 0, 1), 1),
                         step(place(Technique::NakedSingle, 1, 2), 1)}).hardest_technique ==
        std::optional<Technique>(Technique::HiddenSingle));
  CHECK(compute_metrics({step(eliminate(Technique::HiddenPair, {{1, 1}}), 1),
                         step(eliminate(Technique::NakedPair, {{2, 1}}), 1)}).hardest_technique ==
        std::optional<Technique>(Technique::HiddenPair));
  CHECK(compute_metrics({step(eliminate(Technique::XWing, {{1, 1}}), 1),
                         step(place(Technique::NakedSingle, 2, 3), 1)}).hardest_level == 5);
}

// Metrics from a real run agree with the solver's own statistics.
void test_metrics_match_solver() {
  for (const Expected& e : kClassified) {
    const LogicalResult run = solve_logically(parse(e.puzzle));
    const SolveMetrics m = compute_metrics(run.steps);
    CHECK_CTX(m.deductions == run.stats.deductions_applied, e.name);
    CHECK_CTX(m.placements == run.stats.placements, e.name);
    CHECK_CTX(m.eliminated_candidates == run.stats.eliminated_candidates, e.name);
    CHECK_CTX(m.by_technique == run.stats.by_technique, e.name);
    CHECK_CTX(m.deductions == static_cast<int>(run.steps.size()), e.name);
    CHECK_CTX(compute_metrics(run.steps) == m, e.name);
  }
}

// ---------- B. classification ----------
void test_standard_profiles() {
  const auto profiles = standard_profiles();
  CHECK(profiles.size() == 5);
  const char* names[] = {"Easy", "Medium", "Hard", "Expert", "Master"};
  for (std::size_t i = 0; i < 5; ++i) CHECK(profiles[i].name == names[i]);
  CHECK(profiles[0].techniques == (std::vector<Technique>{Technique::NakedSingle, Technique::HiddenSingle}));
  CHECK(profiles[4].techniques == default_technique_order());
  for (std::size_t i = 1; i < profiles.size(); ++i) {   // each adds to the previous
    CHECK(profiles[i].techniques.size() > profiles[i - 1].techniques.size());
    CHECK(std::equal(profiles[i - 1].techniques.begin(), profiles[i - 1].techniques.end(),
                     profiles[i].techniques.begin()));
  }
  // Level of every technique in profile i is at most i + 1.
  for (std::size_t i = 0; i < profiles.size(); ++i)
    for (Technique t : profiles[i].techniques) CHECK(technique_level(t) <= static_cast<int>(i) + 1);
}

void test_classified_fixtures() {
  const auto profiles = standard_profiles();
  for (const Expected& e : kClassified) {
    const std::string ctx = e.name;
    const Board puzzle = parse(e.puzzle);
    const DifficultyResult r = classify_difficulty(puzzle);
    CHECK_CTX(r.status == DifficultyStatus::Classified, ctx);
    CHECK_CTX(r.profile == e.profile && r.profile_index == e.index, ctx);
    CHECK_CTX(r.solver_status == LogicalStatus::Solved && r.solver_reason == LogicalReason::PuzzleSolved, ctx);
    // The hardest technique needed is exactly the profile's level.
    CHECK_CTX(r.metrics.hardest_level == e.index + 1, ctx);
    // Every easier profile genuinely fails on it (stalls); it is never mislabelled.
    for (int j = 0; j < e.index; ++j) {
      const DifficultyResult easier = evaluate_profile(puzzle, profiles[static_cast<std::size_t>(j)]);
      CHECK_CTX(easier.status == DifficultyStatus::Unsolved && easier.profile.empty() &&
                    easier.solver_status == LogicalStatus::Stalled,
                ctx + " under " + profiles[static_cast<std::size_t>(j)].name);
    }
    // Every profile from the classified one up solves it the same way.
    for (std::size_t j = static_cast<std::size_t>(e.index); j < profiles.size(); ++j) {
      const DifficultyResult same = evaluate_profile(puzzle, profiles[j]);
      CHECK_CTX(same.status == DifficultyStatus::Classified && same.metrics == r.metrics, ctx);
    }
    // Totals agree with an independent run of the same profile.
    const LogicalResult run = solve_logically(puzzle, [&] {
      LogicalOptions o; o.techniques = profiles[static_cast<std::size_t>(e.index)].techniques; return o; }());
    CHECK_CTX(r.metrics == compute_metrics(run.steps), ctx);
    CHECK_CTX(r.metrics.deductions == run.stats.deductions_applied, ctx);
    CHECK_CTX(r.metrics.placements == run.stats.placements, ctx);
    CHECK_CTX(r.metrics.eliminated_candidates == run.stats.eliminated_candidates, ctx);
    // Deterministic.
    const DifficultyResult again = classify_difficulty(puzzle);
    CHECK_CTX(again.profile == r.profile && again.metrics == r.metrics && again.status == r.status, ctx);
  }
  // Specific metric values (boundary cases).
  const DifficultyResult w = classify_difficulty(parse(kWiki));
  CHECK(w.metrics.deductions == 51 && w.metrics.placements == 51 && w.metrics.eliminated_candidates == 0);
  CHECK(w.metrics.hardest_level == 1);
  const DifficultyResult m1 = classify_difficulty(parse(kMedium1));
  CHECK(m1.metrics.eliminated_candidates == 1 && m1.metrics.count(Technique::Pointing) == 1);
  CHECK(m1.metrics.hardest_technique == std::optional<Technique>(Technique::Pointing));
  CHECK(m1.metrics.placements + 1 == m1.metrics.deductions);
  const DifficultyResult n = classify_difficulty(parse(kNakedOnly));
  CHECK(n.metrics.count(Technique::HiddenSingle) == 0 && n.metrics.count(Technique::NakedSingle) == 58);
}

void test_unsolved() {
  for (const char* s : kStalled) {
    const Board puzzle = parse(s);
    const DifficultyResult r = classify_difficulty(puzzle);
    CHECK(r.status == DifficultyStatus::Unsolved);
    CHECK(r.status != DifficultyStatus::Classified && r.profile.empty() && r.profile_index == -1);
    CHECK(r.solver_status == LogicalStatus::Stalled && r.solver_reason == LogicalReason::NoApplicableDeduction);
    CHECK(r.metrics.deductions > 0);   // partial progress of the strongest profile
    for (const DifficultyProfile& p : standard_profiles()) {
      CHECK(evaluate_profile(puzzle, p).status == DifficultyStatus::Unsolved);
    }
  }
  // The empty board is valid and stalls: no label.
  const DifficultyResult empty = classify_difficulty(Board{});
  CHECK(empty.status == DifficultyStatus::Unsolved && empty.metrics.deductions == 0);
}

void test_invalid_and_contradiction() {
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  DifficultyResult r = classify_difficulty(bad);
  CHECK(r.status == DifficultyStatus::InvalidInput && r.solver_reason == LogicalReason::InconsistentBoard);
  CHECK(r.profile.empty() && r.profile_index == -1 && r.metrics == SolveMetrics{});
  Board broken = parse(kWikiSolved);   // complete but with a repeated digit
  broken.set(0, 0, broken.get(0, 1));
  CHECK(classify_difficulty(broken).status == DifficultyStatus::InvalidInput);

  for (const char* s : {kDeadCell, kContradictsAfterStep, kTwoSinglesCollide}) {
    r = classify_difficulty(parse(s));
    CHECK(r.status == DifficultyStatus::Contradiction);
    CHECK(r.profile.empty() && r.profile_index == -1 && r.solver_status == LogicalStatus::Contradiction);
    CHECK(evaluate_profile(parse(s), standard_profiles()[0]).status == DifficultyStatus::Contradiction);
  }
  CHECK(classify_difficulty(parse(kDeadCell)).solver_reason == LogicalReason::ContradictoryInitialState);
  CHECK(classify_difficulty(parse(kContradictsAfterStep)).solver_reason == LogicalReason::ContradictionAfterDeduction);
  CHECK(classify_difficulty(parse(kTwoSinglesCollide)).solver_reason == LogicalReason::DeductionWouldContradict);

  // A complete valid board needs no deductions: first profile, zero metrics.
  r = classify_difficulty(parse(kWikiSolved));
  CHECK(r.status == DifficultyStatus::Classified && r.profile == "Easy" && r.metrics == SolveMetrics{});
}

// ---------- custom profiles ----------
void test_custom_profiles() {
  const DifficultyProfile naked_only{"NakedOnly", {Technique::NakedSingle}};
  // Solved with the one technique it needs.
  DifficultyResult r = evaluate_profile(parse(kNakedOnly), naked_only);
  CHECK(r.status == DifficultyStatus::Classified && r.profile == "NakedOnly" && r.profile_index == 0);
  CHECK(r.metrics.count(Technique::NakedSingle) == 58 && r.metrics.hardest_level == 1);

  // Required technique unavailable: the puzzle is not classified, partial metrics.
  r = evaluate_profile(parse(kMedium1), DifficultyProfile{"Pointless", {Technique::NakedSingle, Technique::HiddenSingle}});
  CHECK(r.status == DifficultyStatus::Unsolved && r.profile.empty() && r.solver_status == LogicalStatus::Stalled);
  r = evaluate_profile(parse(kMedium1), DifficultyProfile{"PointingOnly", {Technique::Pointing}});
  CHECK(r.status == DifficultyStatus::Unsolved);   // eliminations alone cannot place digits
  r = evaluate_profile(parse(kMasterXWingA),
                       DifficultyProfile{"NoXWing", {Technique::NakedSingle, Technique::HiddenSingle,
                                                     Technique::Pointing, Technique::Claiming,
                                                     Technique::NakedPair, Technique::HiddenPair,
                                                     Technique::NakedTriple, Technique::HiddenTriple}});
  CHECK(r.status == DifficultyStatus::Unsolved && r.metrics.count(Technique::XWing) == 0);

  // A custom ladder.
  const std::vector<DifficultyProfile> ladder = {
      {"Naked", {Technique::NakedSingle}},
      {"Singles", {Technique::NakedSingle, Technique::HiddenSingle}},
      {"Locked", {Technique::NakedSingle, Technique::HiddenSingle, Technique::Pointing, Technique::Claiming}}};
  r = classify_difficulty(parse(kNakedOnly), ladder);
  CHECK(r.status == DifficultyStatus::Classified && r.profile == "Naked" && r.profile_index == 0);
  r = classify_difficulty(parse(kMedium1), ladder);
  CHECK(r.status == DifficultyStatus::Classified && r.profile == "Locked" && r.profile_index == 2);
  r = classify_difficulty(parse(kHardNakedPair), ladder);
  CHECK(r.status == DifficultyStatus::Unsolved);   // stalls under the strongest profile of this ladder
  // The profile's own priority order is respected (hidden before naked here).
  r = evaluate_profile(parse(kWiki), DifficultyProfile{"HiddenFirst", {Technique::HiddenSingle, Technique::NakedSingle}});
  CHECK(r.status == DifficultyStatus::Classified);
  const LogicalResult naked_first = solve_logically(parse(kWiki));
  CHECK(r.metrics.count(Technique::HiddenSingle) > naked_first.stats.count(Technique::HiddenSingle));
}

void test_invalid_profiles() {
  const Board puzzle = parse(kWiki);
  auto invalid = [&](const std::vector<DifficultyProfile>& p, const char* what) {
    const DifficultyResult r = classify_difficulty(puzzle, p);
    CHECK_CTX(r.status == DifficultyStatus::InvalidInput && r.solver_reason == LogicalReason::InvalidConfiguration, what);
    CHECK_CTX(r.profile.empty() && r.profile_index == -1 && r.metrics == SolveMetrics{}, what);
  };
  const DifficultyProfile a{"A", {Technique::NakedSingle}};
  const DifficultyProfile b{"B", {Technique::NakedSingle, Technique::HiddenSingle}};
  invalid({}, "empty ladder");
  invalid({DifficultyProfile{"", {Technique::NakedSingle}}}, "empty name");
  invalid({DifficultyProfile{"A", {}}}, "no techniques");
  invalid({DifficultyProfile{"A", {Technique::NakedSingle, Technique::NakedSingle}}}, "duplicate technique");
  invalid({DifficultyProfile{"A", {static_cast<Technique>(99)}}}, "unknown technique");
  invalid({a, DifficultyProfile{"A", b.techniques}}, "duplicate names");
  invalid({b, a}, "later profile weaker than the earlier one");
  invalid({a, DifficultyProfile{"H", {Technique::HiddenSingle}}}, "not a superset");
  CHECK(classify_difficulty(puzzle, {a, b}).status == DifficultyStatus::Classified);
  // evaluate_profile validates a single profile the same way.
  CHECK(evaluate_profile(puzzle, DifficultyProfile{"", {Technique::NakedSingle}}).status == DifficultyStatus::InvalidInput);
  CHECK(evaluate_profile(puzzle, DifficultyProfile{"A", {}}).status == DifficultyStatus::InvalidInput);
  // The board is checked after the profiles, but neither is ever mistaken for the other.
  Board bad;
  bad.set(0, 0, 4);  bad.set(0, 5, 4);
  CHECK(classify_difficulty(bad, {a}).solver_reason == LogicalReason::InconsistentBoard);
  CHECK(classify_difficulty(bad, {}).solver_reason == LogicalReason::InvalidConfiguration);

  // is_valid_ladder is exactly the validation classify_difficulty applies (M22: generate_targeted uses it
  // instead of classifying an empty board). Compare on every ladder above plus valid ones.
  const std::vector<std::vector<DifficultyProfile>> ladders = {
      {}, {DifficultyProfile{"", {Technique::NakedSingle}}}, {DifficultyProfile{"A", {}}},
      {DifficultyProfile{"A", {Technique::NakedSingle, Technique::NakedSingle}}},
      {DifficultyProfile{"A", {static_cast<Technique>(99)}}}, {a, DifficultyProfile{"A", b.techniques}}, {b, a},
      {a, DifficultyProfile{"H", {Technique::HiddenSingle}}}, {a, b}, {a}, standard_profiles()};
  for (const auto& ladder : ladders) {
    const DifficultyResult r = classify_difficulty(Board{}, ladder);
    const bool rejected = r.status == DifficultyStatus::InvalidInput && r.solver_reason == LogicalReason::InvalidConfiguration;
    CHECK(is_valid_ladder(ladder) == !rejected);
  }
  CHECK(is_valid_ladder(standard_profiles()) && !is_valid_ladder({}));
}

// ---------- sweep ----------
void test_sweep() {
  const Board solution = parse(kWikiSolved);
  const auto profiles = standard_profiles();
  ref::Rng rng(909);
  int per_profile[5] = {0, 0, 0, 0, 0}, unsolved = 0;
  for (int trial = 0; trial < 50; ++trial) {
    Board puzzle = solution;
    const int blanks = 35 + rng.below(35);
    for (int k = 0; k < blanks; ++k) puzzle.set(rng.below(81), 0);
    const DifficultyResult r = classify_difficulty(puzzle);
    const std::string ctx = "trial " + std::to_string(trial);
    CHECK_CTX(r.status == DifficultyStatus::Classified || r.status == DifficultyStatus::Unsolved, ctx);
    if (r.status == DifficultyStatus::Classified) {
      ++per_profile[r.profile_index];
      CHECK_CTX(r.profile == profiles[static_cast<std::size_t>(r.profile_index)].name, ctx);
      CHECK_CTX(r.metrics.hardest_level == r.profile_index + 1, ctx);
      CHECK_CTX(r.solver_status == LogicalStatus::Solved, ctx);
    } else {
      ++unsolved;
      CHECK_CTX(r.profile.empty() && r.solver_status == LogicalStatus::Stalled, ctx);
    }
    const DifficultyResult again = classify_difficulty(puzzle);
    CHECK_CTX(again.status == r.status && again.profile == r.profile && again.metrics == r.metrics, ctx);
  }
  CHECK(per_profile[0] > 0 && unsolved > 0);
  std::printf("difficulty sweep: easy=%d medium=%d hard=%d expert=%d master=%d unsolved=%d\n",
              per_profile[0], per_profile[1], per_profile[2], per_profile[3], per_profile[4], unsolved);
}

}  // namespace

int main() {
  test_levels();
  test_metrics_known_trace();
  test_metrics_edge_cases();
  test_metrics_match_solver();
  test_standard_profiles();
  test_classified_fixtures();
  test_unsolved();
  test_invalid_and_contradiction();
  test_custom_profiles();
  test_invalid_profiles();
  test_sweep();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
