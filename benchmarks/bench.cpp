#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <set>
#include <stdexcept>

#include "sudoku/diagnostics.hpp"
#include "sudoku/difficulty.hpp"
#include "sudoku/generator.hpp"
#include "sudoku/json_writer.hpp"
#include "sudoku/logical.hpp"
#include "sudoku/puzzle.hpp"
#include "sudoku/targeted.hpp"

#ifndef SUDOKU_BUILD_TYPE
#define SUDOKU_BUILD_TYPE "unspecified"
#endif

namespace sudoku::bench {

std::string_view to_string(Tier t) {
  switch (t) {
    case Tier::Smoke: return "smoke";
    case Tier::Standard: return "standard";
    case Tier::Extended: return "extended";
  }
  return "unknown";
}

std::optional<Tier> parse_tier(std::string_view name) {
  if (name == "smoke") return Tier::Smoke;
  if (name == "standard") return Tier::Standard;
  if (name == "extended") return Tier::Extended;
  return std::nullopt;
}

// ---------------------------------------------------------------- aggregation

TimingSummary summarize_times(std::vector<std::int64_t> v) {
  TimingSummary s;
  s.count = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.min_ns = v.front();
  s.max_ns = v.back();
  const std::size_t n = v.size();
  s.median_ns = n % 2 == 1 ? static_cast<double>(v[n / 2])
                           : (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2])) / 2.0;
  long double sum = 0;
  for (std::int64_t x : v) sum += static_cast<long double>(x);
  s.mean_ns = static_cast<double>(sum / static_cast<long double>(n));
  return s;
}

// ---------------------------------------------------------------- environment

Environment current_environment() {
  Environment e;
#if defined(__clang__)
  e.compiler = std::string("clang ") + __VERSION__;
#elif defined(__GNUC__)
  e.compiler = std::string("gcc ") + __VERSION__;
#else
  e.compiler = "unknown";
#endif
  e.build_type = SUDOKU_BUILD_TYPE;
#if defined(__OPTIMIZE__)
  e.optimization_enabled = true;
#endif
  e.cxx_standard = static_cast<long>(__cplusplus);
  e.pointer_bits = static_cast<int>(sizeof(void*) * 8);
#if defined(__linux__)
  e.os = "linux";
#elif defined(__APPLE__)
  e.os = "macos";
#elif defined(_WIN32)
  e.os = "windows";
#else
  e.os = "unknown";
#endif
  e.timer = "steady_clock";
  e.timer_is_steady = std::chrono::steady_clock::is_steady;
  return e;
}

// ------------------------------------------------------------------- corpus

namespace {

// Puzzle fixtures (each has exactly one solution, confirmed with an independent
// brute-force solver) and the profile the project classifies them as.
struct Fixture { const char* id; const char* puzzle; };
constexpr Fixture kFixtures[] = {
    {"easy", "53..7...." "6..195..." ".98....6." "8...6...3" "4..8.3..1" "7...2...6" ".6....28." "...419..5" "....8..79"},
    {"medium", "5...2.8671...........45..1......7....6......443....2..81.6........179.5..79..5..1"},
    {"hard", "6.........278..56..9......15......9.8..1.6.2....4......5.....4..6..3...737..2...9"},
    {"expert", "...7....3.569.2..88..36.9..6...7.....17.....248.....7.....3..5.9.4.28............"},
    {"master", ".9......2.8.......1.2..3.89....4.8..4..9.8.25...6.5.....8.6.24.3.5....17.1......."},
    {"stalled", ".2.78...48....37.9..6..5.....73.6...34.2.......5....2..7.594.6.......8.........4."},
};

Board parse_fixture(const char* text) {
  std::array<int, kCellCount> v{};
  int n = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == ' ') continue;
    v[static_cast<std::size_t>(n++)] = *p == '.' ? 0 : *p - '0';
  }
  if (n != kCellCount) throw std::logic_error("benchmark fixture has wrong length");
  return Board(v);
}

std::string params(std::initializer_list<std::pair<const char*, std::uint64_t>> numbers,
                   std::initializer_list<std::pair<const char*, const char*>> strings = {}) {
  JsonWriter w;
  w.begin_object();
  for (const auto& [k, v] : numbers) w.field(k, v);
  for (const auto& [k, v] : strings) w.field(k, v);
  w.end_object();
  return w.str();
}

BenchCase grid_case(const char* id, Tier tier, std::uint64_t seed) {
  BenchCase c;
  c.id = id;
  c.operation = "complete_grid";
  c.description = "generate_complete_grid(seed)";
  c.tier = tier;
  c.expected_status = "generated";
  c.params_json = params({{"seed", seed}});
  c.run = [seed] {
    const GenerateResult r = generate_complete_grid(seed);
    CaseOutcome o;
    o.success = r.status == GenerateStatus::Generated && r.grid.has_value();
    o.status = o.success ? "generated" : "budget_exhausted";
    o.counters = {{"nodes", static_cast<std::int64_t>(r.stats.nodes)},
                  {"assignments", static_cast<std::int64_t>(r.stats.assignments)},
                  {"backtracks", static_cast<std::int64_t>(r.stats.backtracks)}};
    JsonWriter w;
    w.begin_object();
    if (r.grid) w.field("grid", board_string(*r.grid));
    w.end_object();
    o.detail_json = w.str();
    return o;
  };
  return c;
}

BenchCase puzzle_case(const char* id, Tier tier, std::uint64_t seed, int target_clues, const char* expected) {
  BenchCase c;
  c.id = id;
  c.operation = "puzzle_generation";
  c.description = "generate_puzzle(seed, {target_clues}): grid + uniqueness-preserving clue removal";
  c.tier = tier;
  c.expected_status = expected;
  c.params_json = params({{"seed", seed}, {"target_clues", static_cast<std::uint64_t>(target_clues)}});
  c.run = [seed, target_clues] {
    PuzzleOptions options;
    options.target_clues = target_clues;
    const PuzzleResult r = generate_puzzle(seed, options);
    CaseOutcome o;
    o.status = std::string(to_string(r.status));
    o.success = r.puzzle.has_value();
    o.counters = {{"clue_count", r.clue_count},
                  {"cells_considered", r.stats.cells_considered},
                  {"removed", r.stats.removed},
                  {"rejected_not_unique", r.stats.rejected_not_unique},
                  {"rejected_inconclusive", r.stats.rejected_inconclusive}};
    JsonWriter w;
    w.begin_object();
    if (r.puzzle) w.field("puzzle", board_string(*r.puzzle));
    w.end_object();
    o.detail_json = w.str();
    return o;
  };
  return c;
}

BenchCase logical_case(const Fixture& f, const char* expected) {
  BenchCase c;
  c.id = std::string("logical.") + f.id;
  c.operation = "logical_solve";
  c.description = "solve_logically(fixture) with the default technique order";
  c.tier = std::string(f.id) == "easy" ? Tier::Smoke : Tier::Standard;
  c.expected_status = expected;
  c.params_json = params({}, {{"fixture", f.id}});
  const Board board = parse_fixture(f.puzzle);   // setup outside the measured call
  c.run = [board] {
    const LogicalResult r = solve_logically(board);
    CaseOutcome o;
    o.status = std::string(to_string(r.status));
    o.success = r.status == LogicalStatus::Solved;
    o.counters = {{"deductions", r.stats.deductions_applied},
                  {"placements", r.stats.placements},
                  {"eliminated_candidates", r.stats.eliminated_candidates}};
    for (Technique t : default_technique_order()) {
      o.counters["technique." + std::string(technique_id(t))] = r.stats.count(t);
    }
    JsonWriter w;
    w.begin_object();
    w.field("reason", to_string(r.reason));
    w.end_object();
    o.detail_json = w.str();
    return o;
  };
  return c;
}

BenchCase classification_case(const Fixture& f, const char* expected) {
  BenchCase c;
  c.id = std::string("classify.") + f.id;
  c.operation = "classification";
  c.description = "classify_difficulty(fixture) with the standard profile ladder";
  c.tier = std::string(f.id) == "easy" ? Tier::Smoke : Tier::Standard;
  c.expected_status = expected;
  c.params_json = params({}, {{"fixture", f.id}});
  const Board board = parse_fixture(f.puzzle);
  c.run = [board] {
    const DifficultyResult r = classify_difficulty(board);
    CaseOutcome o;
    o.status = std::string(to_string(r.status));
    o.success = r.status == DifficultyStatus::Classified;
    o.counters = {{"deductions", r.metrics.deductions},
                  {"placements", r.metrics.placements},
                  {"eliminated_candidates", r.metrics.eliminated_candidates},
                  {"hardest_level", r.metrics.hardest_level}};
    JsonWriter w;
    w.begin_object();
    if (r.status == DifficultyStatus::Classified) {
      w.field("profile", r.profile);
      w.field("profile_index", r.profile_index);
    }
    w.key("metrics");
    write_metrics(w, r.metrics);
    w.end_object();
    o.detail_json = w.str();
    return o;
  };
  return c;
}

BenchCase targeted_case(const char* id, Tier tier, const char* target, std::uint64_t seed,
                        std::uint64_t max_attempts, const char* expected) {
  BenchCase c;
  c.id = id;
  c.operation = "targeted_generation";
  c.description = "generate_targeted(seed, {target, max_attempts})";
  c.tier = tier;
  c.expected_status = expected;
  c.params_json = params({{"seed", seed}, {"max_attempts", max_attempts}}, {{"target", target}});
  const std::string target_name = target;
  c.run = [target_name, seed, max_attempts] {
    TargetedOptions options;
    options.target = target_name;
    options.max_attempts = max_attempts;
    const TargetedResult r = generate_targeted(seed, options);
    CaseOutcome o;
    o.status = std::string(to_string(r.status));
    o.success = r.status == TargetedStatus::Generated;
    const auto i64 = [](std::uint64_t v) { return static_cast<std::int64_t>(v); };
    o.counters = {{"attempts", i64(r.attempts)},
                  {"max_attempts", i64(options.max_attempts)},
                  {"rejected_easier", i64(r.rejections.easier_than_target)},
                  {"rejected_harder", i64(r.rejections.harder_than_target)},
                  {"rejected_unsolved", i64(r.rejections.unsolved)},
                  {"rejected_degenerate", i64(r.rejections.degenerate)},
                  {"rejected_inconclusive", i64(r.rejections.inconclusive)}};
    o.detail_json = targeted_diagnostics_json(r, options);   // no timing inside
    return o;
  };
  return c;
}

}  // namespace

std::vector<BenchCase> corpus() {
  std::vector<BenchCase> cases;
  // smoke: one of every operation, cheap
  cases.push_back(grid_case("grid.seed1", Tier::Smoke, 1));
  cases.push_back(puzzle_case("puzzle.default.seed1", Tier::Smoke, 1, kMinPuzzleClues, "target_not_reached"));
  cases.push_back(logical_case(kFixtures[0], "solved"));
  cases.push_back(classification_case(kFixtures[0], "classified"));
  cases.push_back(targeted_case("targeted.easy.seed1", Tier::Smoke, "Easy", 1, 60, "generated"));
  cases.push_back(targeted_case("targeted.master.exhaust.seed1", Tier::Smoke, "Master", 1, 3, "exhausted"));
  // standard
  cases.push_back(grid_case("grid.seed2", Tier::Standard, 2));
  cases.push_back(puzzle_case("puzzle.clues30.seed2", Tier::Standard, 2, 30, "target_reached"));
  for (std::size_t i = 1; i < std::size(kFixtures); ++i) {
    const bool stalled = std::string(kFixtures[i].id) == "stalled";
    cases.push_back(logical_case(kFixtures[i], stalled ? "stalled" : "solved"));
    cases.push_back(classification_case(kFixtures[i], stalled ? "unsolved" : "classified"));
  }
  cases.push_back(targeted_case("targeted.medium.seed1", Tier::Standard, "Medium", 1, 60, "generated"));
  cases.push_back(targeted_case("targeted.hard.seed0", Tier::Standard, "Hard", 0, 60, "generated"));
  cases.push_back(targeted_case("targeted.expert.seed13", Tier::Standard, "Expert", 13, 60, "generated"));
  cases.push_back(targeted_case("targeted.master.seed8", Tier::Standard, "Master", 8, 60, "generated"));
  cases.push_back(targeted_case("targeted.expert.exhaust.seed1", Tier::Standard, "Expert", 1, 5, "exhausted"));
  // extended: larger budgets, outcome not pinned (Generated or Exhausted are both legitimate)
  for (std::uint64_t seed = 0; seed < 6; ++seed) {
    cases.push_back(targeted_case(("targeted.expert.ext.seed" + std::to_string(seed)).c_str(), Tier::Extended,
                                  "Expert", seed, 400, "*"));
    cases.push_back(targeted_case(("targeted.master.ext.seed" + std::to_string(seed)).c_str(), Tier::Extended,
                                  "Master", seed, 400, "*"));
  }
  return cases;
}

// ------------------------------------------------------------------- config

std::optional<std::string> validate_config(const BenchConfig& config, const std::vector<BenchCase>& cases) {
  if (config.repetitions < 1 || config.repetitions > 1000) return "repetitions must be in 1..1000";
  if (config.warmup < 0 || config.warmup > 100) return "warmup must be in 0..100";
  std::set<std::string> known;
  for (const BenchCase& c : cases) known.insert(c.id);
  for (const std::string& id : config.case_ids) {
    if (!known.count(id)) return "unknown case id: " + id;
  }
  if (select_cases(config, cases).empty()) return "no cases selected";
  return std::nullopt;
}

std::vector<const BenchCase*> select_cases(const BenchConfig& config, const std::vector<BenchCase>& cases) {
  std::vector<const BenchCase*> out;
  for (const BenchCase& c : cases) {
    const bool wanted = config.case_ids.empty()
                            ? static_cast<int>(c.tier) <= static_cast<int>(config.tier)
                            : std::find(config.case_ids.begin(), config.case_ids.end(), c.id) != config.case_ids.end();
    if (wanted) out.push_back(&c);
  }
  return out;
}

// ---------------------------------------------------------------------- run

namespace {

struct Timed {
  CaseOutcome outcome;
  std::string error;
  bool threw = false;
  std::int64_t elapsed_ns = 0;
};

Timed timed_run(const BenchCase& c) {
  Timed t;
  const auto start = std::chrono::steady_clock::now();
  try {
    t.outcome = c.run();
  } catch (const std::exception& e) {
    t.threw = true;
    t.error = e.what();
  } catch (...) {
    t.threw = true;
    t.error = "unknown exception";
  }
  t.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  return t;
}

}  // namespace

BenchReport run(const std::vector<BenchCase>& cases, const BenchConfig& config) {
  if (const auto error = validate_config(config, cases)) throw std::invalid_argument(*error);
  BenchReport report;
  report.config = config;
  for (const BenchCase* c : select_cases(config, cases)) {
    for (int i = 0; i < config.warmup; ++i) (void)timed_run(*c);   // unrecorded
    CaseSummary summary;
    summary.case_id = c->id;
    summary.operation = c->operation;
    std::vector<std::int64_t> times;
    for (int rep = 0; rep < config.repetitions; ++rep) {
      const Timed t = timed_run(*c);
      BenchRecord r;
      r.case_id = c->id;
      r.operation = c->operation;
      r.params_json = c->params_json;
      r.repetition = rep;
      r.expected_status = c->expected_status;
      r.elapsed_ns = t.elapsed_ns;
      if (t.threw) {
        r.status = "exception";
        r.error = t.error;
        r.matches_expected = false;
      } else {
        r.status = t.outcome.status;
        r.success = t.outcome.success;
        r.counters = t.outcome.counters;
        r.detail_json = t.outcome.detail_json;
        r.matches_expected = c->expected_status == "*" || c->expected_status == r.status;
      }
      // Aggregate.
      ++summary.runs;
      ++summary.outcomes[r.status];
      summary.successes += r.success ? 1 : 0;
      summary.unexpected += r.matches_expected ? 0 : 1;
      times.push_back(r.elapsed_ns);
      for (const auto& [k, v] : r.counters) summary.counter_totals[k] += v;
      if (!report.records.empty() && report.records.back().case_id == c->id) {
        const BenchRecord& first = report.records[report.records.size() - static_cast<std::size_t>(rep)];
        if (first.status != r.status || first.counters != r.counters || first.detail_json != r.detail_json) {
          summary.results_stable = false;
        }
      }
      report.records.push_back(std::move(r));
    }
    summary.timing = summarize_times(times);
    const auto attempts = summary.counter_totals.find("attempts");
    if (attempts != summary.counter_totals.end() && summary.successes > 0) {
      summary.attempts_per_success = static_cast<double>(attempts->second) / static_cast<double>(summary.successes);
    }
    if (summary.unexpected > 0 || !summary.results_stable) report.ok = false;
    report.summaries.push_back(std::move(summary));
  }
  return report;
}

// ------------------------------------------------------------------- output

namespace {

void write_counters(JsonWriter& w, const std::map<std::string, std::int64_t>& counters) {
  w.begin_object();
  for (const auto& [k, v] : counters) w.field(k, v);
  w.end_object();
}

void write_timing(JsonWriter& w, const TimingSummary& t) {
  w.begin_object();
  w.field("samples", static_cast<std::uint64_t>(t.count));
  // Absent (not zero) when there are no samples.
  if (t.min_ns) w.field("min_ns", *t.min_ns);
  if (t.median_ns) w.field("median_ns", *t.median_ns);
  if (t.mean_ns) w.field("mean_ns", *t.mean_ns);
  if (t.max_ns) w.field("max_ns", *t.max_ns);
  // Few samples do not describe a distribution.
  w.field("distribution_reliable", t.count >= 5);
  w.end_object();
}

}  // namespace

std::string report_json(const BenchReport& report, const Environment& env) {
  JsonWriter w;
  w.begin_object();
  w.field("schema", kSchema);
  w.field("corpus_version", kCorpusVersion);
  w.key("environment").begin_object();
  w.field("compiler", env.compiler);
  w.field("build_type", env.build_type);
  w.field("optimization_enabled", env.optimization_enabled);
  w.field("cxx_standard", static_cast<std::int64_t>(env.cxx_standard));
  w.field("pointer_bits", env.pointer_bits);
  w.field("os", env.os);
  w.field("timer", env.timer);
  w.field("timer_is_steady", env.timer_is_steady);
  w.end_object();
  w.key("config").begin_object();
  w.field("tier", to_string(report.config.tier));
  w.key("case_ids").begin_array();
  for (const std::string& id : report.config.case_ids) w.value(id);
  w.end_array();
  w.field("repetitions", report.config.repetitions);
  w.field("warmup", report.config.warmup);
  w.end_object();

  w.key("records").begin_array();
  for (const BenchRecord& r : report.records) {
    w.begin_object();
    w.field("case", r.case_id);
    w.field("operation", r.operation);
    w.key("params").raw(r.params_json);
    w.field("repetition", r.repetition);
    w.field("status", r.status);
    w.field("success", r.success);
    w.field("expected_status", r.expected_status);
    w.field("matches_expected", r.matches_expected);
    w.field("elapsed_ns", r.elapsed_ns);
    w.key("counters");
    write_counters(w, r.counters);
    w.key("detail").raw(r.detail_json);
    if (!r.error.empty()) w.field("error", r.error);
    w.end_object();
  }
  w.end_array();

  w.key("summary").begin_array();
  for (const CaseSummary& s : report.summaries) {
    w.begin_object();
    w.field("case", s.case_id);
    w.field("operation", s.operation);
    w.field("runs", static_cast<std::uint64_t>(s.runs));
    w.key("outcomes").begin_object();
    for (const auto& [status, n] : s.outcomes) w.field(status, static_cast<std::uint64_t>(n));
    w.end_object();
    w.field("successes", static_cast<std::uint64_t>(s.successes));
    w.field("unexpected", static_cast<std::uint64_t>(s.unexpected));
    w.field("results_stable", s.results_stable);
    w.key("timing");
    write_timing(w, s.timing);
    w.key("counter_totals");
    write_counters(w, s.counter_totals);
    if (s.attempts_per_success) w.field("attempts_per_success", *s.attempts_per_success);
    w.end_object();
  }
  w.end_array();
  w.field("ok", report.ok);
  w.end_object();
  return w.str();
}

}  // namespace sudoku::bench
