// Difficulty metrics and profiles for the logical solver.
//
// These are THIS PROJECT'S OPERATIONAL classification, defined entirely by
// which of the implemented logical techniques (logical.hpp) are needed to
// solve a puzzle. They are not a universally accepted measure of human
// difficulty: real solvers differ in which patterns they see, and puzzles
// within one profile can feel very different. A label is only meaningful
// relative to the documented technique set below.
//
// ---- Metrics (compute_metrics) -------------------------------------------
// A pure function of a logical-solver trace (LogicalResult::steps). Only steps
// whose status is Applied are counted (solver traces contain nothing else).
//   deductions            applied deductions (each counted once).
//   placements            applied placement deductions (naked/hidden singles).
//   eliminated_candidates candidates removed by ELIMINATION deductions only
//                         (sum of their `effective` sizes). Candidates removed
//                         as a side effect of a placement are not counted, so
//                         nothing is counted twice. A deduction with several
//                         eliminations counts once in `deductions` and by its
//                         full size here. Hence deductions = placements +
//                         (number of elimination deductions).
//   by_technique          applied deductions per Technique.
//   hardest_technique     the applied technique with the highest level, ties
//                         broken by the later position in Technique order;
//                         absent for an empty trace.
//   hardest_level         technique_level(hardest_technique), 0 if none.
// Limits: counts reflect the solver's deterministic choices (easiest technique
// first, first deduction in each detector's order); a different but valid
// solving path could use more or fewer steps. They are measures of this
// solver's trace, not of the puzzle in the abstract. Deterministic for the same
// puzzle, technique list and implementation version.
//
// ---- Technique levels -----------------------------------------------------
//   1 singles (naked, hidden)     2 locked candidates (pointing, claiming)
//   3 pairs (naked, hidden)       4 triples (naked, hidden)      5 X-Wing
//
// ---- Profiles --------------------------------------------------------------
// A profile is a name plus the techniques (in priority order) the solver may
// use. The standard ladder, each profile adding to the previous one:
//   Easy    singles
//   Medium  + locked candidates
//   Hard    + pairs
//   Expert  + triples
//   Master  + X-Wing
// There are no numeric thresholds: a profile's only criterion is that the
// solver SOLVES the puzzle (board complete and independently verified) using
// just its techniques.
//
// ---- Classification --------------------------------------------------------
// classify_difficulty() runs the logical solver with each profile in order and
// stops at the first outcome that decides the result:
//   Solved        -> Classified as that profile (the easiest profile that
//                    solves the puzzle). Metrics come from that run.
//   Stalled       -> try the next profile.
//   Contradiction -> Contradiction (no difficulty label).
//   InvalidInput  -> InvalidInput.   Error -> Error.
// If every profile stalls the result is Unsolved: the puzzle may be valid and
// even have a unique solution, but none of the profiles can solve it; it is
// never labelled. Metrics then describe the partial progress of the last
// profile. A complete (already solved) board needs no deductions and is
// classified under the first profile with zero deductions.
// The profile list must be a ladder: non-empty, every profile with a distinct
// non-empty name and a valid non-empty technique list, and each profile's
// techniques a superset of the previous one's. Otherwise the result is
// InvalidInput (reason InvalidConfiguration) and the solver is not run.
// No search or guessing is used anywhere.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "sudoku/board.hpp"
#include "sudoku/deduction.hpp"
#include "sudoku/logical.hpp"

namespace sudoku {

// 1..5 as documented above.
int technique_level(Technique technique);

struct SolveMetrics {
  int deductions = 0;
  int placements = 0;
  int eliminated_candidates = 0;
  std::array<int, kTechniqueCount> by_technique{};
  std::optional<Technique> hardest_technique;
  int hardest_level = 0;
  int count(Technique t) const { return by_technique[static_cast<std::size_t>(t)]; }
  friend bool operator==(const SolveMetrics&, const SolveMetrics&) = default;
};

SolveMetrics compute_metrics(const std::vector<AppliedDeduction>& trace);

struct DifficultyProfile {
  std::string name;
  std::vector<Technique> techniques;  // priority order
};

// The standard ladder Easy, Medium, Hard, Expert, Master.
std::vector<DifficultyProfile> standard_profiles();

enum class DifficultyStatus {
  Classified,     // solved under `profile`
  Unsolved,       // valid, but no profile solves it (stalled)
  Contradiction,  // no label
  InvalidInput,   // bad board or bad profiles
  Error,          // internal inconsistency in the solver
};

struct DifficultyResult {
  DifficultyStatus status = DifficultyStatus::InvalidInput;
  std::string profile;        // name, only when Classified
  int profile_index = -1;     // index in the list used, only when Classified
  SolveMetrics metrics;       // see the classification notes above
  LogicalStatus solver_status = LogicalStatus::InvalidInput;  // of the deciding run
  LogicalReason solver_reason = LogicalReason::InvalidConfiguration;
};

// Runs a single profile: Classified if the solver solves the puzzle with just
// its techniques, Unsolved if it stalls, otherwise as above.
DifficultyResult evaluate_profile(const Board& puzzle, const DifficultyProfile& profile);

// True iff `profiles` is a valid ladder (the rules above). This is exactly the
// validation classify_difficulty performs before it runs the solver, without
// running anything: classify_difficulty reports InvalidConfiguration if and
// only if this returns false.
bool is_valid_ladder(const std::vector<DifficultyProfile>& profiles);

// Easiest profile of an ordered ladder that solves the puzzle.
DifficultyResult classify_difficulty(const Board& puzzle,
                                     const std::vector<DifficultyProfile>& profiles = standard_profiles());

}  // namespace sudoku
