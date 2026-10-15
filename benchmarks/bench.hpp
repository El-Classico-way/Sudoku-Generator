// Benchmark corpus, runner, aggregation and report for the Sudoku engine.
//
// Everything here measures from OUTSIDE the engine: a case is a function that
// calls the existing public API, and the runner times that call with a
// monotonic clock (std::chrono::steady_clock). Timing never feeds back into the
// engine, so it cannot change a result, an RNG sequence or an attempt count.
//
// Terminology
//   counters   deterministic algorithmic work/outcome numbers (attempts, search
//              nodes, deductions, ...). For a fixed corpus version, implementation
//              and compiler-independent algorithm they are identical on every run.
//   timings    wall-clock nanoseconds (elapsed_ns). Nondeterministic.
//   record     one execution of one case (one repetition).
//   summary    aggregates of ONE case over its repetitions (cases/operations are
//              never mixed in a statistic).
//
// Report schema "sudoku.benchmark/1": see the README ("Benchmarks") for the
// field list; report_json() is the reference implementation.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sudoku::bench {

inline constexpr std::string_view kSchema = "sudoku.benchmark/1";
// Bump when cases, seeds, options or fixtures change, so results stay comparable.
inline constexpr std::string_view kCorpusVersion = "m16.1";

// A case belongs to a tier and to every larger one: smoke < standard < extended.
enum class Tier { Smoke = 0, Standard = 1, Extended = 2 };
std::string_view to_string(Tier t);
std::optional<Tier> parse_tier(std::string_view name);

struct CaseOutcome {
  std::string status;       // operation-specific status identifier
  bool success = false;     // did the operation produce what it was asked for?
  std::map<std::string, std::int64_t> counters;  // deterministic work/outcome numbers
  std::string detail_json = "{}";                // deterministic JSON object with more data
};

struct BenchCase {
  std::string id;           // stable identifier
  std::string operation;    // complete_grid | puzzle_generation | logical_solve |
                            // classification | targeted_generation
  std::string description;
  Tier tier = Tier::Standard;
  std::string expected_status = "*";  // the status this case must produce; "*" = not pinned
  std::string params_json = "{}";     // the inputs (seed, options), as a JSON object
  std::function<CaseOutcome()> run;
};

// ---- aggregation ----
struct TimingSummary {
  std::size_t count = 0;                // samples (0 => every statistic is absent)
  std::optional<std::int64_t> min_ns;
  std::optional<std::int64_t> max_ns;
  std::optional<double> median_ns;      // mean of the two middle samples for an even count
  std::optional<double> mean_ns;
};
// Statistics over ALL given samples (the caller decides which runs are included).
TimingSummary summarize_times(std::vector<std::int64_t> elapsed_ns);

struct BenchConfig {
  Tier tier = Tier::Standard;
  std::vector<std::string> case_ids;  // if non-empty: exactly these cases (tier ignored)
  int repetitions = 1;                // measured executions per case, 1..1000
  int warmup = 0;                     // unrecorded executions before measuring, 0..100
};

struct BenchRecord {
  std::string case_id;
  std::string operation;
  std::string params_json;
  int repetition = 0;                 // 0-based
  std::string status;                 // "exception" if the case threw
  bool success = false;
  std::string expected_status;
  bool matches_expected = false;
  std::int64_t elapsed_ns = 0;        // measured (also for failures and exceptions)
  std::map<std::string, std::int64_t> counters;
  std::string detail_json = "{}";
  std::string error;                  // exception text; empty otherwise
};

struct CaseSummary {
  std::string case_id;
  std::string operation;
  std::size_t runs = 0;
  std::map<std::string, std::size_t> outcomes;  // status -> number of runs
  std::size_t successes = 0;
  std::size_t unexpected = 0;         // runs whose status differed from expected (exceptions included)
  bool results_stable = true;         // every run produced identical status, counters and detail
  TimingSummary timing;               // over ALL runs, successful or not
  std::map<std::string, std::int64_t> counter_totals;  // sums over all runs
  // attempts_total / successes when the case reports an "attempts" counter and at
  // least one success; absent otherwise. attempts_total includes failed requests.
  std::optional<double> attempts_per_success;
};

struct BenchReport {
  BenchConfig config;
  std::vector<BenchRecord> records;
  std::vector<CaseSummary> summaries;
  bool ok = true;                     // no unexpected outcome, exception or unstable result
};

struct Environment {
  std::string compiler;               // __VERSION__ or "unknown"
  std::string build_type;             // CMAKE_BUILD_TYPE at configure time, or "unspecified"
  bool optimization_enabled = false;  // whether the compiler defined __OPTIMIZE__
  long cxx_standard = 0;              // __cplusplus
  int pointer_bits = 0;
  std::string os;                     // linux | macos | windows | unknown
  std::string timer;                  // "steady_clock"
  bool timer_is_steady = false;
};
Environment current_environment();

// The fixed corpus (all tiers, stable order).
std::vector<BenchCase> corpus();

// Returns an error message, or nullopt when `config` is valid for `cases`
// (unknown case id, repetitions/warmup out of range, nothing selected).
std::optional<std::string> validate_config(const BenchConfig& config, const std::vector<BenchCase>& cases);
// The cases `config` selects, in corpus order. Precondition: validate_config() passed.
std::vector<const BenchCase*> select_cases(const BenchConfig& config, const std::vector<BenchCase>& cases);

// Runs the selected cases. Throws std::invalid_argument if the config is invalid.
// A case that throws is recorded (status "exception") and the run continues.
BenchReport run(const std::vector<BenchCase>& cases, const BenchConfig& config);

std::string report_json(const BenchReport& report, const Environment& env);

}  // namespace sudoku::bench
