#include "sudoku/difficulty.hpp"

#include <algorithm>
#include <set>

namespace sudoku {
namespace {

DifficultyResult invalid_configuration() {
  DifficultyResult r;
  r.status = DifficultyStatus::InvalidInput;
  r.solver_status = LogicalStatus::InvalidInput;
  r.solver_reason = LogicalReason::InvalidConfiguration;
  return r;
}

bool valid_technique(Technique t) {
  const auto all = default_technique_order();
  return std::find(all.begin(), all.end(), t) != all.end();
}

// Name non-empty; techniques non-empty, known and without duplicates.
bool profile_ok(const DifficultyProfile& p) {
  if (p.name.empty() || p.techniques.empty()) return false;
  std::set<Technique> seen;
  for (Technique t : p.techniques) {
    if (!valid_technique(t) || !seen.insert(t).second) return false;
  }
  return true;
}

bool ladder_ok(const std::vector<DifficultyProfile>& profiles) {
  if (profiles.empty()) return false;
  std::set<std::string> names;
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    if (!profile_ok(profiles[i]) || !names.insert(profiles[i].name).second) return false;
    if (i == 0) continue;
    const std::set<Technique> previous(profiles[i - 1].techniques.begin(), profiles[i - 1].techniques.end());
    const std::set<Technique> current(profiles[i].techniques.begin(), profiles[i].techniques.end());
    if (!std::includes(current.begin(), current.end(), previous.begin(), previous.end())) return false;
  }
  return true;
}

DifficultyResult from_run(const LogicalResult& run) {
  DifficultyResult r;
  r.metrics = compute_metrics(run.steps);
  r.solver_status = run.status;
  r.solver_reason = run.reason;
  switch (run.status) {
    case LogicalStatus::Solved: r.status = DifficultyStatus::Classified; break;
    case LogicalStatus::Stalled: r.status = DifficultyStatus::Unsolved; break;
    case LogicalStatus::Contradiction: r.status = DifficultyStatus::Contradiction; break;
    case LogicalStatus::InvalidInput: r.status = DifficultyStatus::InvalidInput; break;
    case LogicalStatus::Error: r.status = DifficultyStatus::Error; break;
  }
  return r;
}

LogicalOptions options_for(const DifficultyProfile& p) {
  LogicalOptions o;
  o.techniques = p.techniques;
  return o;
}

}  // namespace

int technique_level(Technique t) {
  switch (t) {
    case Technique::NakedSingle:
    case Technique::HiddenSingle: return 1;
    case Technique::Pointing:
    case Technique::Claiming: return 2;
    case Technique::NakedPair:
    case Technique::HiddenPair: return 3;
    case Technique::NakedTriple:
    case Technique::HiddenTriple: return 4;
    case Technique::XWing: return 5;
  }
  return 0;
}

SolveMetrics compute_metrics(const std::vector<AppliedDeduction>& trace) {
  SolveMetrics m;
  for (const AppliedDeduction& step : trace) {
    if (step.status != ApplyStatus::Applied) continue;
    const Technique t = step.deduction.technique();
    ++m.deductions;
    ++m.by_technique[static_cast<std::size_t>(t)];
    if (step.deduction.action() == DeductionAction::PlaceDigit) {
      ++m.placements;
    } else {
      m.eliminated_candidates += static_cast<int>(step.effective.size());
    }
    const int level = technique_level(t);
    if (!m.hardest_technique || level > m.hardest_level ||
        (level == m.hardest_level && static_cast<int>(t) > static_cast<int>(*m.hardest_technique))) {
      m.hardest_technique = t;
      m.hardest_level = level;
    }
  }
  return m;
}

std::vector<DifficultyProfile> standard_profiles() {
  std::vector<DifficultyProfile> out;
  std::vector<Technique> acc;
  const auto add = [&](const char* name, std::initializer_list<Technique> more) {
    acc.insert(acc.end(), more.begin(), more.end());
    out.push_back({name, acc});
  };
  add("Easy", {Technique::NakedSingle, Technique::HiddenSingle});
  add("Medium", {Technique::Pointing, Technique::Claiming});
  add("Hard", {Technique::NakedPair, Technique::HiddenPair});
  add("Expert", {Technique::NakedTriple, Technique::HiddenTriple});
  add("Master", {Technique::XWing});
  return out;
}

DifficultyResult evaluate_profile(const Board& puzzle, const DifficultyProfile& profile) {
  if (!profile_ok(profile)) return invalid_configuration();
  DifficultyResult r = from_run(solve_logically(puzzle, options_for(profile)));
  if (r.status == DifficultyStatus::Classified) {
    r.profile = profile.name;
    r.profile_index = 0;
  }
  return r;
}

bool is_valid_ladder(const std::vector<DifficultyProfile>& profiles) { return ladder_ok(profiles); }

DifficultyResult classify_difficulty(const Board& puzzle, const std::vector<DifficultyProfile>& profiles) {
  if (!ladder_ok(profiles)) return invalid_configuration();
  DifficultyResult last;
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    last = from_run(solve_logically(puzzle, options_for(profiles[i])));
    if (last.status == DifficultyStatus::Classified) {
      last.profile = profiles[i].name;
      last.profile_index = static_cast<int>(i);
      return last;
    }
    if (last.status != DifficultyStatus::Unsolved) return last;  // contradiction, invalid, error
  }
  return last;  // every profile stalled: Unsolved, partial metrics of the last run
}

}  // namespace sudoku
