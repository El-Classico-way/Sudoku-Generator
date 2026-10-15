#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "sudoku/solver.hpp"
#include "sudoku/targeted.hpp"
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

// Puzzles/solutions (unique solution each, confirmed with an independent
// brute-force solver) with their M14 classification.
struct Fixture { const char* name; const char* puzzle; const char* solution; const char* profile; };
const Fixture kFixtures[] = {
    {"easy", "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1" "7...2...6" ".6....28." "...419..5" "....8..79",
     "534678912" "672195348" "198342567" "859761423" "426853791" "713924856" "961537284" "287419635" "345286179", "Easy"},
    {"medium", "5...2.8671...........45..1......7....6......443....2..81.6........179.5..79..5..1",
     "543921867192768543786453912258347196967512384431896275815634729324179658679285431", "Medium"},
    {"hard", "6.........278..56..9......15......9.8..1.6.2....4......5.....4..6..3...737..2...9",
     "685741932127893564493265871516372498849156723732489615258917346961534287374628159", "Hard"},
    {"expert", "...7....3.569.2..88..36.9..6...7.....17.....248.....7.....3..5.9.4.28............",
     "249785163356912748871364925692871534517643892483259671728136459964528317135497286", "Expert"},
    {"master", ".9......2.8.......1.2..3.89....4.8..4..9.8.25...6.5.....8.6.24.3.5....17.1.......",
     "593816472687429351142753689759142836436978125821635794978561243365284917214397568", "Master"},
};
// Valid, uniquely solvable, but every profile stalls.
const char* kStalledPuzzle = ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4.";
const char* kStalledSolution = "529781634814623759736945218297316485341258976685479123178594362453162897962837541";
// Two solutions (a "deadly rectangle" cleared from the easy fixture's solution).
const char* kTwoSolutionsPuzzle =
    "534008912" "672195348" "198342567" "859001423" "426853791" "713924856" "961537284" "287419635" "345286179";

// Request seeds (with max 60 attempts) demonstrated to succeed for each profile.
struct Request { const char* target; std::uint64_t seed; };
const Request kRequests[] = {{"Easy", 1}, {"Medium", 1}, {"Hard", 0}, {"Expert", 13}, {"Master", 8}};

std::uint64_t total(const RejectionCounts& r) {
  return r.easier_than_target + r.harder_than_target + r.unsolved + r.degenerate + r.inconclusive;
}

TargetedOptions options_for(const std::string& target, std::uint64_t attempts = 60) {
  TargetedOptions o;
  o.target = target;
  o.max_attempts = attempts;
  return o;
}

// Everything a Generated result promises, checked independently of the generator.
void check_generated(const TargetedResult& r, const std::string& target, const TargetedOptions& options,
                     const std::string& ctx) {
  CHECK_CTX(r.status == TargetedStatus::Generated && r.fault == RequestFault::None, ctx);
  CHECK_CTX(r.puzzle && r.solution, ctx);
  if (!r.puzzle || !r.solution) return;
  CHECK_CTX(r.attempts >= 1 && r.attempts <= options.max_attempts, ctx);
  CHECK_CTX(total(r.rejections) + 1 == r.attempts, ctx);  // every earlier attempt was rejected
  CHECK_CTX(r.target == target, ctx);
  // A real puzzle, not a complete grid, and the reported clue count is right.
  int clues = 0;
  for (int i = 0; i < kCellCount; ++i) clues += r.puzzle->get(i) != 0;
  CHECK_CTX(clues == r.clue_count && clues >= kMinPuzzleClues && clues < kCellCount, ctx);
  // Exactly one solution, established exhaustively, equal to the reported one.
  const CountResult count = count_solutions(*r.puzzle, 2);
  CHECK_CTX(count.status == SearchStatus::Exhausted && count.count == 1, ctx);
  CHECK_CTX(count.first_solution && *count.first_solution == *r.solution, ctx);
  CHECK_CTX(classify_uniqueness(*r.puzzle) == SolutionClass::Unique, ctx);
  CHECK_CTX(verify_solution(*r.solution, *r.puzzle).valid(), ctx);
  // Exactly the requested difficulty, never an adjacent one.
  const DifficultyResult cls = classify_difficulty(*r.puzzle, options.profiles);
  CHECK_CTX(cls.status == DifficultyStatus::Classified && cls.profile == target, ctx);
  CHECK_CTX(r.classification.profile == target && r.classification.metrics == cls.metrics, ctx);
  const int index = cls.profile_index;
  for (int j = 0; j < static_cast<int>(options.profiles.size()); ++j) {
    const DifficultyResult at = evaluate_profile(*r.puzzle, options.profiles[static_cast<std::size_t>(j)]);
    if (j < index) CHECK_CTX(at.status == DifficultyStatus::Unsolved, ctx);   // no easier profile solves it
    else CHECK_CTX(at.status == DifficultyStatus::Classified, ctx);
  }
  // The reported attempt seed replays the candidate exactly.
  const PuzzleResult replay = generate_puzzle(r.attempt_seed, options.puzzle);
  CHECK_CTX(replay.puzzle && *replay.puzzle == *r.puzzle && replay.solution && *replay.solution == *r.solution, ctx);
}

// ---------- A. exact matching, end to end ----------
void test_exact_matching() {
  for (const Request& req : kRequests) {
    const TargetedOptions options = options_for(req.target);
    const TargetedResult r = generate_targeted(req.seed, options);
    check_generated(r, req.target, options, std::string(req.target) + " seed " + std::to_string(req.seed));
  }
}

// ---------- check_candidate ----------
void test_candidate_verdicts() {
  // Each fixture matches only its own profile; every other target is a
  // WrongDifficulty (easier or harder), never a match.
  for (const Fixture& f : kFixtures) {
    for (const Fixture& other : kFixtures) {
      const CandidateReport r = check_candidate(parse(f.puzzle), parse(f.solution), other.profile);
      const bool same = std::string(f.profile) == other.profile;
      CHECK_CTX((r.verdict == CandidateVerdict::Matches) == same,
                std::string(f.name) + " as " + other.profile);
      CHECK_CTX(same || r.verdict == CandidateVerdict::WrongDifficulty, std::string(f.name) + " as " + other.profile);
      CHECK_CTX(r.classified && r.classification.profile == f.profile, f.name);
    }
  }

  // Stalls under every profile: valid and unique, but never a match.
  for (const char* target : {"Easy", "Master"}) {
    const CandidateReport r = check_candidate(parse(kStalledPuzzle), parse(kStalledSolution), target);
    CHECK(r.verdict == CandidateVerdict::Unsolved && r.classified);
    CHECK(r.classification.status == DifficultyStatus::Unsolved && r.classification.profile.empty());
  }

  // Not unique.
  const Board easy_solution = parse(kFixtures[0].solution);
  CandidateReport r = check_candidate(parse(kTwoSolutionsPuzzle), easy_solution, "Easy");
  CHECK(r.verdict == CandidateVerdict::NotUnique && !r.classified);

  // Inconclusive: a one-node budget cannot establish uniqueness.
  r = check_candidate(parse(kFixtures[1].puzzle), parse(kFixtures[1].solution), "Medium",
                      standard_profiles(), std::uint64_t{1});
  CHECK(r.verdict == CandidateVerdict::Inconclusive && r.verdict != CandidateVerdict::Matches && !r.classified);
  // With room to finish it is established and matches.
  r = check_candidate(parse(kFixtures[1].puzzle), parse(kFixtures[1].solution), "Medium",
                      standard_profiles(), std::uint64_t{100000});
  CHECK(r.verdict == CandidateVerdict::Matches);

  // Degenerate: a complete grid is not a puzzle.
  r = check_candidate(easy_solution, easy_solution, "Easy");
  CHECK(r.verdict == CandidateVerdict::Degenerate && !r.classified);

  // Invalid: repeated clue digit; solution with a hole; solution that disagrees with a clue;
  // solution that is not a Sudoku at all.
  Board bad = parse(kFixtures[0].puzzle);
  bad.set(0, 2, 5);  // duplicates the 5 at (0,0)
  CHECK(check_candidate(bad, easy_solution, "Easy").verdict == CandidateVerdict::Invalid);
  Board holey = easy_solution;
  holey.set(4, 4, 0);
  CHECK(check_candidate(parse(kFixtures[0].puzzle), holey, "Easy").verdict == CandidateVerdict::Invalid);
  CHECK(check_candidate(parse(kFixtures[0].puzzle), parse(kFixtures[1].solution), "Easy").verdict ==
        CandidateVerdict::Invalid);
  Board wrong = easy_solution;
  wrong.set(0, 0, 9);
  CHECK(check_candidate(parse(kFixtures[0].puzzle), wrong, "Easy").verdict == CandidateVerdict::Invalid);

  // Argument errors.
  const Board p = parse(kFixtures[0].puzzle);
  CHECK_THROWS_AS(check_candidate(p, easy_solution, "Nope"), std::invalid_argument);
  CHECK_THROWS_AS(check_candidate(p, easy_solution, ""), std::invalid_argument);
  CHECK_THROWS_AS(check_candidate(p, easy_solution, "Easy", standard_profiles(), std::uint64_t{0}),
                  std::invalid_argument);
  CHECK_THROWS_AS(check_candidate(p, easy_solution, "Easy", {}), std::invalid_argument);
  // A custom ladder: a profile is matched by name.
  const std::vector<DifficultyProfile> ladder = {{"A", {Technique::NakedSingle}},
                                                 {"B", {Technique::NakedSingle, Technique::HiddenSingle}}};
  CHECK(check_candidate(p, easy_solution, "B", ladder).verdict != CandidateVerdict::Error);
}

// ---------- B. no silent downgrade ----------
void test_no_silent_downgrade() {
  // A bounded search for a rare target fails honestly.
  for (const char* target : {"Expert", "Master"}) {
    const TargetedOptions options = options_for(target, 5);
    const TargetedResult r = generate_targeted(1, options);
    CHECK_CTX(r.status == TargetedStatus::Exhausted && r.fault == RequestFault::None, target);
    CHECK_CTX(!r.puzzle && !r.solution && r.clue_count == 0, target);   // nothing is returned
    CHECK_CTX(r.attempts == 5 && total(r.rejections) == 5, target);     // all five were rejected
    CHECK_CTX(r.rejections.easier_than_target + r.rejections.unsolved > 0, target);  // lower ones were refused
    CHECK_CTX(r.last_rejection.has_value(), target);
    CHECK_CTX(r.classification.status != DifficultyStatus::Classified, target);
    CHECK_CTX(r.status != TargetedStatus::Generated, target);
  }
  // The attempts that were rejected for being easier really were easier: replaying
  // each attempt seed and classifying it independently gives a lower profile or none.
  const TargetedOptions options = options_for("Master", 5);
  const TargetedResult r = generate_targeted(3, options);
  CHECK(r.status == TargetedStatus::Exhausted);
  RejectionCounts replayed;
  for (std::uint64_t i = 0; i < r.attempts; ++i) {
    const PuzzleResult p = generate_puzzle(attempt_seed(3, i), options.puzzle);
    const DifficultyResult c = classify_difficulty(*p.puzzle);
    if (c.status == DifficultyStatus::Unsolved) ++replayed.unsolved;
    else if (c.profile_index < 4) ++replayed.easier_than_target;
    else if (c.profile_index > 4) ++replayed.harder_than_target;
  }
  CHECK(replayed == r.rejections);
  // Whatever target is asked for, a Generated result is that target and no other.
  for (const char* target : {"Easy", "Medium", "Hard"}) {
    for (std::uint64_t seed = 1; seed < (std::string(target) == "Medium" ? 2u : 3u); ++seed) {
      const TargetedResult g = generate_targeted(seed, options_for(target, 25));
      if (g.status == TargetedStatus::Generated) {
        CHECK_CTX(g.classification.profile == target, target);
        CHECK_CTX(classify_difficulty(*g.puzzle).profile == target, target);
      } else {
        CHECK_CTX(g.status == TargetedStatus::Exhausted && !g.puzzle, target);
      }
    }
  }
}

// ---------- C. attempt budgets ----------
void test_attempt_budgets() {
  // A budget of one makes exactly one attempt, success or failure.
  for (const char* target : {"Easy", "Master"}) {
    for (std::uint64_t seed = 0; seed < 3; ++seed) {
      const TargetedOptions options = options_for(target, 1);
      const TargetedResult r = generate_targeted(seed, options);
      CHECK_CTX(r.attempts == 1, target);
      if (r.status == TargetedStatus::Generated) {
        check_generated(r, target, options, target);
      } else {
        CHECK_CTX(r.status == TargetedStatus::Exhausted && total(r.rejections) == 1, target);
      }
    }
  }
  // The limit is exact on failure and never exceeded.
  for (std::uint64_t budget : {2, 3}) {
    const TargetedResult r = generate_targeted(1, options_for("Master", budget));
    CHECK(r.status == TargetedStatus::Exhausted && r.attempts == budget && total(r.rejections) == budget);
  }
  // On success the count is where it stopped, within the budget.
  const Request& e = kRequests[0];
  const TargetedResult ok = generate_targeted(e.seed, options_for(e.target, 60));
  CHECK(ok.status == TargetedStatus::Generated && ok.attempts <= 60);
  if (ok.attempts > 1) {  // a smaller budget stops before reaching it
    const TargetedResult shorter = generate_targeted(e.seed, options_for(e.target, ok.attempts - 1));
    CHECK(shorter.status == TargetedStatus::Exhausted && shorter.attempts == ok.attempts - 1);
  }
  const TargetedResult exact = generate_targeted(e.seed, options_for(e.target, ok.attempts));
  CHECK(exact.status == TargetedStatus::Generated && exact.attempts == ok.attempts);

  // Invalid budgets and requests: nothing is attempted.
  auto invalid = [](TargetedOptions o, RequestFault fault, const char* what) {
    const TargetedResult r = generate_targeted(1, o);
    CHECK_CTX(r.status == TargetedStatus::InvalidRequest && r.fault == fault, what);
    CHECK_CTX(r.attempts == 0 && !r.puzzle && !r.solution && r.clue_count == 0, what);
    CHECK_CTX(r.status != TargetedStatus::Exhausted, what);
  };
  invalid(options_for("Easy", 0), RequestFault::ZeroAttempts, "zero attempts");
  invalid(options_for("", 5), RequestFault::EmptyTarget, "empty target");
  invalid(options_for("Legendary", 5), RequestFault::UnknownTarget, "unknown target");
  invalid(options_for("easy", 5), RequestFault::UnknownTarget, "names are case-sensitive");
  { TargetedOptions o = options_for("Easy", 5); o.puzzle.target_clues = 16;
    invalid(o, RequestFault::InvalidClueTarget, "clue target 16"); }
  { TargetedOptions o = options_for("Easy", 5); o.puzzle.target_clues = 82;
    invalid(o, RequestFault::InvalidClueTarget, "clue target 82"); }
  { TargetedOptions o = options_for("Easy", 5); o.puzzle.node_budget = 0;
    invalid(o, RequestFault::ZeroNodeBudget, "zero node budget"); }
  { TargetedOptions o = options_for("Easy", 5);
    o.profiles = {{"A", {Technique::NakedSingle}}, {"B", {Technique::HiddenSingle}}};  // not nested
    invalid(o, RequestFault::InvalidProfiles, "non-ladder profiles"); }
  { TargetedOptions o = options_for("Easy", 5); o.profiles.clear();
    invalid(o, RequestFault::InvalidProfiles, "no profiles"); }
  { TargetedOptions o = options_for("Easy", 5);
    o.profiles = {{"Only", {Technique::NakedSingle}}};
    CHECK(generate_targeted(1, o).fault == RequestFault::UnknownTarget); }  // "Easy" is not in this ladder
}

// ---------- D. reproducibility ----------
void expect_same(const TargetedResult& a, const TargetedResult& b, const std::string& ctx) {
  CHECK_CTX(a.status == b.status && a.fault == b.fault, ctx);
  CHECK_CTX(a.target == b.target && a.seed == b.seed, ctx);
  CHECK_CTX(a.attempts == b.attempts && a.attempt_seed == b.attempt_seed, ctx);
  CHECK_CTX(a.puzzle == b.puzzle && a.solution == b.solution && a.clue_count == b.clue_count, ctx);
  CHECK_CTX(a.classification.status == b.classification.status && a.classification.profile == b.classification.profile &&
                a.classification.profile_index == b.classification.profile_index &&
                a.classification.metrics == b.classification.metrics, ctx);
  CHECK_CTX(a.rejections == b.rejections && a.last_rejection == b.last_rejection && a.detail == b.detail, ctx);
}

void test_reproducibility() {
  // Successful searches repeat exactly (the cheap profiles plus one expensive one;
  // every profile's success is already covered by the exact-matching test).
  for (const Request& req : kRequests) {
    const std::string target = req.target;
    if (target == "Medium" || target == "Master") continue;
    const TargetedOptions options = options_for(req.target);
    expect_same(generate_targeted(req.seed, options), generate_targeted(req.seed, options), req.target);
  }
  // A failed bounded search repeats exactly, too.
  const TargetedOptions failing = options_for("Master", 4);
  const TargetedResult a = generate_targeted(11, failing);
  CHECK(a.status == TargetedStatus::Exhausted);
  expect_same(a, generate_targeted(11, failing), "failed search");
  // Other requests in between do not matter (no shared state).
  (void)generate_targeted(12, options_for("Easy", 3));
  expect_same(a, generate_targeted(11, failing), "failed search after other requests");
  // The seed matters; the derivation is documented and stable.
  CHECK(attempt_seed(11, 0) == attempt_seed(11, 0));
  CHECK(attempt_seed(11, 0) != attempt_seed(11, 1));
  CHECK(attempt_seed(11, 0) != attempt_seed(12, 0));
  const TargetedResult b = generate_targeted(12, failing);
  CHECK(b.attempt_seed != a.attempt_seed);
  // The budget does not change which candidates are examined.
  const TargetedResult shorter = generate_targeted(11, options_for("Master", 2));
  CHECK(shorter.attempts == 2 && shorter.rejections.easier_than_target <= a.rejections.easier_than_target);
}

// ---------- E. failure paths ----------
void test_failure_paths() {
  // With a one-node uniqueness budget clue removal keeps every clue (nothing it
  // tries can be shown unique), so every attempt is a complete grid: refused.
  TargetedOptions options = options_for("Easy", 4);
  options.puzzle.node_budget = 1;
  const TargetedResult r = generate_targeted(5, options);
  CHECK(r.status == TargetedStatus::Exhausted && r.attempts == 4);
  CHECK(r.rejections.degenerate == 4 && total(r.rejections) == 4);
  CHECK(!r.puzzle && !r.solution && r.clue_count == 0);   // no default/complete board as success
  CHECK(r.last_rejection == std::optional<CandidateVerdict>(CandidateVerdict::Degenerate));

  // Stalled candidates are rejected and counted; they never match any target.
  const TargetedResult master = generate_targeted(1, options_for("Master", 8));
  CHECK(master.status == TargetedStatus::Exhausted);
  CHECK(master.rejections.unsolved > 0);

  // A non-Generated result never carries a puzzle.
  for (const TargetedResult* x : {&r, &master}) {
    CHECK(!x->puzzle && !x->solution && x->clue_count == 0);
    CHECK(x->status != TargetedStatus::Generated);
  }
}

}  // namespace

int main() {
  test_exact_matching();
  test_candidate_verdicts();
  test_no_silent_downgrade();
  test_attempt_budgets();
  test_reproducibility();
  test_failure_paths();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
