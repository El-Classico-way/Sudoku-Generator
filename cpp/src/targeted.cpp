#include "sudoku/targeted.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

#include "sudoku/solver.hpp"
#include "sudoku/verify.hpp"

namespace sudoku {
namespace {

std::uint64_t splitmix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

bool has_profile(const std::vector<DifficultyProfile>& profiles, const std::string& name) {
  return std::any_of(profiles.begin(), profiles.end(),
                     [&](const DifficultyProfile& p) { return p.name == name; });
}

int index_of(const std::vector<DifficultyProfile>& profiles, const std::string& name) {
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    if (profiles[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// The same validation classify_difficulty applies before it runs anything.
// (It used to classify an empty board, i.e. up to five full logical solves,
// for every candidate just to learn this.)
bool ladder_valid(const std::vector<DifficultyProfile>& profiles) { return is_valid_ladder(profiles); }

}  // namespace

std::uint64_t attempt_seed(std::uint64_t seed, std::uint64_t attempt_index) {
  return splitmix64(seed + (attempt_index + 1) * 0x9E3779B97F4A7C15ull);
}

CandidateReport check_candidate(const Board& puzzle, const Board& solution, const std::string& target,
                                const std::vector<DifficultyProfile>& profiles,
                                std::optional<std::uint64_t> count_node_budget) {
  if (count_node_budget && *count_node_budget == 0) {
    throw std::invalid_argument("count_node_budget must be nonzero");
  }
  if (!ladder_valid(profiles)) throw std::invalid_argument("invalid difficulty profiles");
  if (!has_profile(profiles, target)) throw std::invalid_argument("unknown target profile");

  CandidateReport report;
  auto done = [&](CandidateVerdict v) { report.verdict = v; return report; };

  // Structure: consistent clues, a genuinely complete valid solution (checked
  // independently), and every clue agreeing with it.
  if (!puzzle.is_consistent()) return done(CandidateVerdict::Invalid);
  if (!verify_solution(solution, puzzle).valid()) return done(CandidateVerdict::Invalid);
  if (puzzle.is_complete()) return done(CandidateVerdict::Degenerate);

  // Uniqueness must be established exhaustively; anything else is not proof.
  const CountResult count = count_solutions(puzzle, 2, [&] {
    SearchOptions o;
    o.node_budget = count_node_budget;
    return o;
  }());
  switch (classify(count)) {
    case SolutionClass::Unique: break;
    case SolutionClass::Unknown: return done(CandidateVerdict::Inconclusive);
    case SolutionClass::Multiple:
    case SolutionClass::NoSolution: return done(CandidateVerdict::NotUnique);
    case SolutionClass::InvalidInput: return done(CandidateVerdict::Invalid);
  }
  if (!count.first_solution || *count.first_solution != solution) return done(CandidateVerdict::Invalid);

  // Difficulty: exact match with the target profile only.
  report.classification = classify_difficulty(puzzle, profiles);
  report.classified = true;
  switch (report.classification.status) {
    case DifficultyStatus::Classified:
      return done(report.classification.profile == target ? CandidateVerdict::Matches
                                                          : CandidateVerdict::WrongDifficulty);
    case DifficultyStatus::Unsolved: return done(CandidateVerdict::Unsolved);
    case DifficultyStatus::Contradiction: return done(CandidateVerdict::Contradiction);
    case DifficultyStatus::InvalidInput: return done(CandidateVerdict::Invalid);
    case DifficultyStatus::Error: return done(CandidateVerdict::Error);
  }
  return done(CandidateVerdict::Error);
}

TargetedResult generate_targeted(std::uint64_t seed, const TargetedOptions& options) {
  TargetedResult result;
  result.target = options.target;
  result.seed = seed;

  // Validate the request before doing any work.
  auto reject = [&](RequestFault fault) {
    result.status = TargetedStatus::InvalidRequest;
    result.fault = fault;
    return result;
  };
  if (options.target.empty()) return reject(RequestFault::EmptyTarget);
  if (!ladder_valid(options.profiles)) return reject(RequestFault::InvalidProfiles);
  if (!has_profile(options.profiles, options.target)) return reject(RequestFault::UnknownTarget);
  if (options.max_attempts == 0) return reject(RequestFault::ZeroAttempts);
  if (options.puzzle.target_clues < kMinPuzzleClues || options.puzzle.target_clues > kCellCount) {
    return reject(RequestFault::InvalidClueTarget);
  }
  if (options.puzzle.node_budget && *options.puzzle.node_budget == 0) {
    return reject(RequestFault::ZeroNodeBudget);
  }
  const int target_index = index_of(options.profiles, options.target);

  auto fail = [&](TargetedStatus status, std::string detail) {
    result.status = status;
    result.detail = std::move(detail);
    return result;
  };

  for (std::uint64_t i = 0; i < options.max_attempts; ++i) {
    result.attempts = i + 1;
    result.attempt_seed = attempt_seed(seed, i);
    try {
      PuzzleResult candidate = generate_puzzle(result.attempt_seed, options.puzzle);
      if (candidate.status != PuzzleStatus::TargetReached &&
          candidate.status != PuzzleStatus::TargetNotReached) {
        return fail(TargetedStatus::Error, "clue removal rejected an already validated request");
      }
      if (!candidate.puzzle || !candidate.solution) {
        return fail(TargetedStatus::Error, "clue removal returned no puzzle");
      }
      const CandidateReport report = check_candidate(*candidate.puzzle, *candidate.solution,
                                                     options.target, options.profiles,
                                                     options.puzzle.node_budget);
      switch (report.verdict) {
        case CandidateVerdict::Matches:
          result.status = TargetedStatus::Generated;
          result.puzzle = *candidate.puzzle;
          result.solution = *candidate.solution;
          result.clue_count = candidate.clue_count;
          result.classification = report.classification;
          return result;
        case CandidateVerdict::WrongDifficulty:
          (report.classification.profile_index < target_index ? result.rejections.easier_than_target
                                                              : result.rejections.harder_than_target)++;
          break;
        case CandidateVerdict::Unsolved: ++result.rejections.unsolved; break;
        case CandidateVerdict::Degenerate: ++result.rejections.degenerate; break;
        case CandidateVerdict::Inconclusive: ++result.rejections.inconclusive; break;
        case CandidateVerdict::Contradiction:
          result.classification = report.classification;
          return fail(TargetedStatus::Contradiction, "a verified-unique candidate was found contradictory");
        case CandidateVerdict::NotUnique:
          return fail(TargetedStatus::Error, "candidate failed the independent uniqueness check");
        case CandidateVerdict::Invalid:
          return fail(TargetedStatus::Error, "candidate failed independent validation");
        case CandidateVerdict::Error:
          result.classification = report.classification;
          return fail(TargetedStatus::Error, "solver or classifier reported an internal error");
      }
      result.last_rejection = report.verdict;
    } catch (const std::exception& e) {
      return fail(TargetedStatus::Error, e.what());
    }
  }
  result.status = TargetedStatus::Exhausted;
  return result;
}

}  // namespace sudoku
