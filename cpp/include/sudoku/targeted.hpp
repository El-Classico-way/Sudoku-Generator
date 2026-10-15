// Difficulty-targeted puzzle generation.
//
// generate_targeted() asks for a puzzle of ONE named difficulty profile and
// returns a puzzle only when that exact profile has been established. It never
// substitutes another difficulty: a candidate classified easier OR harder than
// the target is rejected, and if the attempt budget runs out the result is
// Exhausted, with no puzzle. Exhaustion says nothing about whether the target
// is possible: it only means these attempts did not find one.
//
// Each attempt (one iteration of the loop):
//   1. derive attempt_seed = mix(seed, attempt index)         [documented below]
//   2. generate_puzzle(attempt_seed, options.puzzle): a complete grid, then
//      uniqueness-preserving clue removal (unchanged M04/M05 behavior)
//   3. check_candidate(): validity, independent uniqueness, independent
//      solution verification, then classify_difficulty() and an exact match
// Everything reuses the existing components; nothing is relaxed.
//
// check_candidate() verdicts for one candidate:
//   Matches         exactly one solution (exhaustively established), the supplied
//                   solution verifies against the clues, and the puzzle is
//                   classified as the target profile.
//   WrongDifficulty classified as some other profile (easier or harder).
//   Unsolved        valid but stalls under every profile: never a match.
//   NotUnique       zero or several solutions.
//   Inconclusive    the solution count hit its node budget, so uniqueness is NOT
//                   established; never accepted.
//   Degenerate      no clue was removed (a complete grid is not a puzzle).
//   Invalid         inconsistent clues, a supplied solution that is not a valid
//                   solution, disagrees with a clue, or differs from the unique
//                   solution.
//   Contradiction   the logical solver found the (supposedly valid) puzzle
//                   contradictory.
//   Error           an internal solver/classifier inconsistency.
//
// In generate_targeted(): Matches ends the search; WrongDifficulty, Unsolved,
// Degenerate and Inconclusive are ordinary rejections (counted, then retry).
// The other verdicts cannot happen for a puzzle that clue removal just
// verified, so they mean a defect and end the request at once: Contradiction ->
// status Contradiction; NotUnique, Invalid, Error, or any exception ->
// status Error.
//
// Statuses:
//   Generated       `puzzle`/`solution` hold a verified exact match.
//   Exhausted       max_attempts attempts were made, none matched.
//   InvalidRequest  nothing was attempted; `fault` says why.
//   Contradiction   a candidate was found contradictory (defect), see above.
//   Error           internal failure, `detail` says what.
//
// Attempt: one pass through steps 1-3. attempts <= max_attempts always; the
// count includes the successful or fatal attempt. A request that fails
// validation makes 0 attempts.
//
// Reproducibility: the result (status, puzzle, solution, attempts, attempt
// seed, classification, metrics, rejection counts) is a pure function of
// (seed, options) for this implementation. All randomness flows from `seed`
// through std::mt19937_64 with an explicit mixing function; there is no global
// state, clock or unordered iteration. Identical output across compilers or
// library versions is not promised beyond what generate_puzzle() documents, and
// may change when the algorithm changes.
//
// Seed mixing: attempt i (0-based) uses
//   attempt_seed = splitmix64(seed + (i + 1) * 0x9E3779B97F4A7C15)
// so `generate_puzzle(result.attempt_seed, options.puzzle)` reproduces the
// reported candidate exactly.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sudoku/board.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/puzzle.hpp"

namespace sudoku {

enum class CandidateVerdict {
  Matches,
  WrongDifficulty,
  Unsolved,
  NotUnique,
  Inconclusive,
  Degenerate,
  Invalid,
  Contradiction,
  Error,
};

struct CandidateReport {
  CandidateVerdict verdict = CandidateVerdict::Invalid;
  // The classification that decided WrongDifficulty/Matches/Unsolved/
  // Contradiction/Error; default-constructed if classification was not reached.
  DifficultyResult classification;
  bool classified = false;  // whether `classification` was produced
};

// Full per-candidate check against `target` (a profile name in `profiles`).
// `solution` is the complete grid the puzzle was derived from.
// `count_node_budget` bounds the uniqueness search (nullopt = exhaustive; zero
// is invalid). Throws std::invalid_argument for an unknown target, an invalid
// profile list or a zero budget; every other outcome is a verdict.
CandidateReport check_candidate(const Board& puzzle, const Board& solution,
                                const std::string& target,
                                const std::vector<DifficultyProfile>& profiles = standard_profiles(),
                                std::optional<std::uint64_t> count_node_budget = std::nullopt);

struct TargetedOptions {
  std::string target;                                       // profile name; required
  std::vector<DifficultyProfile> profiles = standard_profiles();  // the ladder used
  std::uint64_t max_attempts = 100;                         // >= 1
  PuzzleOptions puzzle;                                     // clue removal per attempt
};

enum class TargetedStatus { Generated, Exhausted, InvalidRequest, Contradiction, Error };

enum class RequestFault {
  None,
  EmptyTarget,
  UnknownTarget,     // not a name in `profiles`
  InvalidProfiles,   // `profiles` is not a valid ladder
  ZeroAttempts,
  InvalidClueTarget, // puzzle.target_clues outside [17, 81]
  ZeroNodeBudget,    // puzzle.node_budget == 0
};

struct RejectionCounts {
  std::uint64_t easier_than_target = 0;   // classified at a lower profile
  std::uint64_t harder_than_target = 0;   // classified at a higher profile
  std::uint64_t unsolved = 0;             // stalled under every profile
  std::uint64_t degenerate = 0;           // no clue removed
  std::uint64_t inconclusive = 0;         // uniqueness not established
  friend bool operator==(const RejectionCounts&, const RejectionCounts&) = default;
};

struct TargetedResult {
  TargetedStatus status = TargetedStatus::InvalidRequest;
  RequestFault fault = RequestFault::None;
  std::string target;               // the requested profile
  std::uint64_t seed = 0;           // the request seed
  std::uint64_t attempts = 0;
  std::uint64_t attempt_seed = 0;   // of the matching (or last/fatal) attempt; 0 if none
  // Present only when status == Generated.
  std::optional<Board> puzzle;
  std::optional<Board> solution;
  int clue_count = 0;
  DifficultyResult classification;  // of the match, or of the fatal candidate
  RejectionCounts rejections;       // all rejected candidates so far
  std::optional<CandidateVerdict> last_rejection;
  std::string detail;               // Error: what failed
};

TargetedResult generate_targeted(std::uint64_t seed, const TargetedOptions& options);

// The documented seed derivation (exposed so a reported attempt can be replayed).
std::uint64_t attempt_seed(std::uint64_t seed, std::uint64_t attempt_index);

}  // namespace sudoku
