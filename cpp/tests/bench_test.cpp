#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "bench.hpp"
#include "check.hpp"
#include "json_parse.hpp"
#include "sudoku/diagnostics.hpp"
#include "sudoku/json_writer.hpp"
#include "sudoku/targeted.hpp"

using namespace sudoku;
using jsontest::Value;

namespace {

Value must_parse(const std::string& text, const std::string& ctx) {
  const auto v = jsontest::parse(text);
  CHECK_CTX(v.has_value(), ctx + ": " + text.substr(0, 200));
  return v ? *v : Value{};
}

// ---------- JSON writer and the independent test parser ----------
void test_parser_is_strict() {
  for (const char* good : {"{}", "[]", "[1,2,3]", "{\"a\":[true,false,null,-1.5e+3,0]}", " \"x\" ", "0", "-0.5"}) {
    CHECK_CTX(jsontest::parse(good).has_value(), good);
  }
  for (const char* bad : {"", "{", "{\"a\":1,}", "[1,]", "[1 2]", "01", "1.", ".5", "+1", "\"a\nb\"", "\"\\q\"",
                          "{a:1}", "[1] x", "tru", "nul", "\"unterminated", "{\"a\" 1}", "1e", "--1"}) {
    CHECK_CTX(!jsontest::parse(bad).has_value(), bad);
  }
}

void test_writer() {
  // Strings: quotes, backslashes, every control character, non-ASCII bytes.
  std::string nasty = "q\"b\\s/ \b\f\n\r\t";
  for (int c = 1; c < 0x20; ++c) nasty += static_cast<char>(c);
  nasty += "\xc3\xa9\xe2\x82\xac end";
  JsonWriter w;
  w.begin_object().field("s", nasty).end_object();
  Value v = must_parse(w.str(), "escapes");
  CHECK(v.has("s") && v.at("s").text == nasty);   // round-trips exactly
  CHECK(w.str().find('\n') == std::string::npos);  // no raw newline in the output

  // Numbers stay numbers; extremes are exact.
  JsonWriter n;
  n.begin_array();
  n.value(std::uint64_t{18446744073709551615ull}).value(std::numeric_limits<std::int64_t>::min());
  n.value(0).value(-7).value(0.5).value(1e21).value(-0.0).value(123456789.125);
  n.end_array();
  v = must_parse(n.str(), "numbers");
  CHECK(v.items.size() == 8);
  for (const Value& item : v.items) CHECK(item.type == Value::Type::Number);
  CHECK(v.items[0].text == "18446744073709551615" && v.items[0].unsigned_integer() == 18446744073709551615ull);
  CHECK(v.items[1].text == "-9223372036854775808");
  CHECK(v.items[4].number() == 0.5 && v.items[5].number() == 1e21 && v.items[7].number() == 123456789.125);

  // No JSON form for NaN/infinity: null, never 0.
  JsonWriter nf;
  nf.begin_array().value(std::nan("")).value(std::numeric_limits<double>::infinity())
      .value(-std::numeric_limits<double>::infinity()).end_array();
  v = must_parse(nf.str(), "non-finite");
  for (const Value& item : v.items) CHECK(item.type == Value::Type::Null);

  // Structure, booleans, null, raw embedding.
  JsonWriter s;
  s.begin_object();
  s.key("a").begin_array().value(true).value(false).null().begin_object().end_object().begin_array().end_array().end_array();
  s.key("raw").raw("{\"x\":1}");
  s.field("empty", "");
  s.end_object();
  v = must_parse(s.str(), "structure");
  CHECK(v.at("a").items.size() == 5 && v.at("a").items[0].boolean && v.at("raw").at("x").integer() == 1);
  CHECK(v.at("empty").text.empty());

  // Misuse throws instead of producing broken JSON.
  { JsonWriter m; m.begin_object(); CHECK_THROWS_AS(m.value(1), std::logic_error); }
  { JsonWriter m; CHECK_THROWS_AS(m.key("a"), std::logic_error); }
  { JsonWriter m; m.begin_array(); CHECK_THROWS_AS(m.key("a"), std::logic_error); }
  { JsonWriter m; m.begin_object(); m.key("a"); CHECK_THROWS_AS(m.key("b"), std::logic_error); }
  { JsonWriter m; m.begin_object(); m.key("a"); CHECK_THROWS_AS(m.end_object(), std::logic_error); }
  { JsonWriter m; m.begin_object(); CHECK_THROWS_AS(m.end_array(), std::logic_error); }
  { JsonWriter m; CHECK_THROWS_AS(m.end_object(), std::logic_error); }
  { JsonWriter m; m.begin_object(); CHECK_THROWS_AS(m.str(), std::logic_error); }
  { JsonWriter m; CHECK_THROWS_AS(m.str(), std::logic_error); }
  { JsonWriter m; m.value(1); CHECK_THROWS_AS(m.value(2), std::logic_error); }
}

// ---------- diagnostics ----------
std::uint64_t sum(const Value& rejections) {
  std::uint64_t t = 0;
  for (const auto& [k, v] : rejections.members) t += v.unsigned_integer();
  return t;
}

void check_diag_common(const Value& d, const TargetedResult& r, const TargetedOptions& o, const std::string& ctx) {
  CHECK_CTX(d.at("schema").text == "sudoku.targeted-diagnostics/1", ctx);
  CHECK_CTX(d.at("target").text == r.target && d.at("seed").unsigned_integer() == r.seed, ctx);
  CHECK_CTX(d.at("max_attempts").unsigned_integer() == o.max_attempts, ctx);
  CHECK_CTX(d.at("attempts").unsigned_integer() == r.attempts && r.attempts <= o.max_attempts, ctx);
  CHECK_CTX(d.at("attempts").is_integer_text() && d.at("seed").is_integer_text(), ctx);
  CHECK_CTX(d.at("rejections").members.size() == 5, ctx);
  CHECK_CTX(r.attempts == 0 || d.at("attempt_seed").unsigned_integer() == r.attempt_seed, ctx);
  CHECK_CTX(d.has("attempt_seed") == (r.attempts > 0), ctx);          // absent, not 0, when nothing ran
  CHECK_CTX(d.has("last_rejection") == r.last_rejection.has_value(), ctx);
  CHECK_CTX(d.has("detail") == !r.detail.empty(), ctx);
  CHECK_CTX(d.has("fault") == (r.status == TargetedStatus::InvalidRequest), ctx);
  CHECK_CTX(d.has("result") == (r.status == TargetedStatus::Generated), ctx);
  CHECK_CTX(!d.has("classification"), ctx);                            // only inside "result"
  CHECK_CTX(!d.has("elapsed_ns"), ctx);                                // none supplied
}

void test_diagnostics() {
  // A real success, a real exhaustion, a real invalid request.
  TargetedOptions easy;
  easy.target = "Easy";
  easy.max_attempts = 60;
  const TargetedResult ok = generate_targeted(1, easy);
  CHECK(ok.status == TargetedStatus::Generated);
  Value d = must_parse(targeted_diagnostics_json(ok, easy), "generated");
  check_diag_common(d, ok, easy, "generated");
  CHECK(d.at("status").text == "generated");
  CHECK(sum(d.at("rejections")) + 1 == ok.attempts);                    // earlier attempts were rejected
  const Value& res = d.at("result");
  CHECK(res.at("puzzle").text == board_string(*ok.puzzle) && res.at("puzzle").text.size() == 81);
  CHECK(res.at("solution").text == board_string(*ok.solution) && res.at("solution").text.find('.') == std::string::npos);
  CHECK(res.at("clue_count").integer() == ok.clue_count);
  const Value& cls = res.at("classification");
  CHECK(cls.at("profile").text == "Easy" && cls.at("profile_index").integer() == 0);
  const Value& metrics = cls.at("metrics");
  CHECK(metrics.at("deductions").integer() == ok.classification.metrics.deductions);
  CHECK(metrics.at("by_technique").members.size() == 9);
  CHECK(metrics.has("hardest_technique") && metrics.at("hardest_level").integer() == 1);

  TargetedOptions master;
  master.target = "Master";
  master.max_attempts = 3;
  const TargetedResult ex = generate_targeted(1, master);
  CHECK(ex.status == TargetedStatus::Exhausted);
  d = must_parse(targeted_diagnostics_json(ex, master), "exhausted");
  check_diag_common(d, ex, master, "exhausted");
  CHECK(d.at("status").text == "exhausted" && sum(d.at("rejections")) == 3 && d.at("attempts").integer() == 3);
  CHECK(!d.has("result"));                                              // nothing is presented as a result
  CHECK(d.at("last_rejection").type == Value::Type::String);

  TargetedOptions bad;
  bad.target = "Nope";
  const TargetedResult inv = generate_targeted(std::numeric_limits<std::uint64_t>::max(), bad);
  d = must_parse(targeted_diagnostics_json(inv, bad), "invalid");
  check_diag_common(d, inv, bad, "invalid");
  CHECK(d.at("status").text == "invalid_request" && d.at("fault").text == "unknown_target");
  CHECK(d.at("attempts").integer() == 0 && !d.has("attempt_seed"));
  CHECK(d.at("seed").text == "18446744073709551615");                   // exact 64-bit seed

  // Fatal outcomes are reported as such and are not folded into the rejection counts.
  TargetedResult fatal;
  fatal.status = TargetedStatus::Error;
  fatal.target = "Hard";
  fatal.seed = 5;
  fatal.attempts = 4;
  fatal.attempt_seed = 99;
  fatal.rejections.easier_than_target = 3;
  fatal.detail = "boom \"quoted\"\nline2";
  fatal.last_rejection = CandidateVerdict::WrongDifficulty;
  TargetedOptions hard;
  hard.target = "Hard";
  hard.max_attempts = 10;
  d = must_parse(targeted_diagnostics_json(fatal, hard), "fatal");
  check_diag_common(d, fatal, hard, "fatal");
  CHECK(d.at("status").text == "error" && d.at("detail").text == fatal.detail);
  CHECK(sum(d.at("rejections")) == 3 && d.at("attempts").integer() == 4);   // 3 rejected + the fatal attempt
  CHECK(!d.has("result"));
  fatal.status = TargetedStatus::Contradiction;
  CHECK(must_parse(targeted_diagnostics_json(fatal, hard), "contradiction").at("status").text == "contradiction");

  // A non-Generated result never leaks its (unassigned) classification as if measured.
  fatal.classification.profile = "Hard";
  fatal.classification.status = DifficultyStatus::Classified;
  CHECK(!must_parse(targeted_diagnostics_json(fatal, hard), "no leak").has("result"));

  // elapsed_ns appears only when supplied.
  d = must_parse(targeted_diagnostics_json(ok, easy, std::int64_t{1234}), "elapsed");
  CHECK(d.at("elapsed_ns").integer() == 1234);
  // Timing does not change anything else.
  Value without = must_parse(targeted_diagnostics_json(ok, easy), "no elapsed");
  CHECK(without.members.size() + 1 == d.members.size());
}

// ---------- aggregation ----------
void test_summarize_times() {
  using bench::summarize_times;
  bench::TimingSummary s = summarize_times({});
  CHECK(s.count == 0 && !s.min_ns && !s.max_ns && !s.median_ns && !s.mean_ns);   // absent, not zero
  s = summarize_times({42});
  CHECK(s.count == 1 && *s.min_ns == 42 && *s.max_ns == 42 && *s.median_ns == 42.0 && *s.mean_ns == 42.0);
  s = summarize_times({9, 1, 5});                                                // odd, unsorted
  CHECK(s.count == 3 && *s.min_ns == 1 && *s.max_ns == 9 && *s.median_ns == 5.0 && *s.mean_ns == 5.0);
  s = summarize_times({10, 2, 8, 4});                                            // even
  CHECK(*s.median_ns == 6.0 && *s.mean_ns == 6.0 && *s.min_ns == 2 && *s.max_ns == 10);
  s = summarize_times({1, 2, 3, 100});                                           // outlier: median != mean
  CHECK(*s.median_ns == 2.5 && *s.mean_ns == 26.5);
  s = summarize_times({0, 0, 0});                                                // valid zero durations are present
  CHECK(s.count == 3 && s.min_ns && *s.min_ns == 0 && s.median_ns && *s.median_ns == 0.0 && *s.mean_ns == 0.0);
  s = summarize_times({7, 7, 7, 7});
  CHECK(*s.median_ns == 7.0 && *s.mean_ns == 7.0);
  const std::int64_t big = std::numeric_limits<std::int64_t>::max();
  s = summarize_times({big, big});                                               // no overflow
  CHECK(*s.min_ns == big && *s.mean_ns > 9.0e18);
}

bench::BenchCase make_case(const std::string& id, const std::string& expected,
                           std::function<bench::CaseOutcome()> fn, bench::Tier tier = bench::Tier::Standard) {
  bench::BenchCase c;
  c.id = id;
  c.operation = "test_op";
  c.tier = tier;
  c.expected_status = expected;
  c.params_json = "{\"k\":1}";
  c.run = std::move(fn);
  return c;
}

bench::CaseOutcome outcome(const std::string& status, bool success, std::int64_t attempts = -1) {
  bench::CaseOutcome o;
  o.status = status;
  o.success = success;
  if (attempts >= 0) o.counters["attempts"] = attempts;
  return o;
}

void test_runner_aggregation() {
  // A request mix: two successes and one failure, with attempts 3 / 5 / 2.
  int call = 0;
  const int attempts[] = {3, 5, 2};
  const bool ok_flags[] = {true, false, true};
  auto mixed = make_case("mixed", "*", [&] {
    const int i = call++ % 3;
    return outcome(ok_flags[i] ? "generated" : "exhausted", ok_flags[i], attempts[i]);
  });
  auto never = make_case("never", "*", [] { return outcome("exhausted", false, 7); });
  bench::BenchConfig cfg;
  cfg.repetitions = 3;
  const bench::BenchReport rep = bench::run({mixed, never}, cfg);
  CHECK(rep.records.size() == 6 && rep.summaries.size() == 2);
  const bench::CaseSummary& m = rep.summaries[0];
  CHECK(m.runs == 3 && m.successes == 2 && m.outcomes.at("generated") == 2 && m.outcomes.at("exhausted") == 1);
  CHECK(m.counter_totals.at("attempts") == 10);
  CHECK(m.attempts_per_success && *m.attempts_per_success == 5.0);     // 10 attempts / 2 successes, failures included
  CHECK(m.timing.count == 3);                                          // every run, failed or not
  CHECK(!m.results_stable && !rep.ok);                                 // outcomes differed between repetitions
  const bench::CaseSummary& n = rep.summaries[1];
  CHECK(n.successes == 0 && !n.attempts_per_success && n.counter_totals.at("attempts") == 21);
  CHECK(n.results_stable && n.unexpected == 0);
  CHECK(n.timing.count == 3);

  // Failures remain in the output and in the JSON.
  const Value j = must_parse(bench::report_json(rep, bench::current_environment()), "aggregation json");
  CHECK(j.at("summary").items.size() == 2 && j.at("records").items.size() == 6);
  CHECK(j.at("summary").items[0].has("attempts_per_success") && !j.at("summary").items[1].has("attempts_per_success"));
  CHECK(j.at("summary").items[1].at("timing").at("distribution_reliable").boolean == false);   // 3 < 5 samples
  CHECK(j.at("ok").boolean == false);
}

void test_failures_are_recorded() {
  int later_calls = 0;
  auto thrower = make_case("thrower", "generated", []() -> bench::CaseOutcome { throw std::runtime_error("boom \"x\""); });
  auto mismatch = make_case("mismatch", "generated", [] { return outcome("exhausted", false); });
  auto fine = make_case("fine", "generated", [&] { ++later_calls; return outcome("generated", true); });
  bench::BenchConfig cfg;
  cfg.repetitions = 2;
  const bench::BenchReport rep = bench::run({thrower, mismatch, fine}, cfg);
  CHECK(rep.records.size() == 6);
  CHECK(rep.records[0].status == "exception" && rep.records[0].error == "boom \"x\"" && !rep.records[0].matches_expected);
  CHECK(rep.records[0].elapsed_ns >= 0 && !rep.records[0].success);
  CHECK(rep.records[2].status == "exhausted" && !rep.records[2].matches_expected);
  CHECK(rep.records[4].matches_expected && later_calls == 2);          // the run continued
  CHECK(!rep.ok);
  CHECK(rep.summaries[0].unexpected == 2 && rep.summaries[1].unexpected == 2 && rep.summaries[2].unexpected == 0);
  const Value j = must_parse(bench::report_json(rep, bench::current_environment()), "failures json");
  CHECK(j.at("records").items[0].at("error").text == "boom \"x\"");
  CHECK(j.at("records").items[0].at("status").text == "exception");
  CHECK(j.at("ok").boolean == false);
  // warm-up executions are not recorded.
  int calls = 0;
  auto counted = make_case("counted", "*", [&] { ++calls; return outcome("generated", true); });
  bench::BenchConfig warm;
  warm.warmup = 3;
  warm.repetitions = 2;
  const bench::BenchReport w = bench::run({counted}, warm);
  CHECK(calls == 5 && w.records.size() == 2 && w.ok);
}

// ---------- configuration and corpus ----------
void test_config_validation() {
  const auto cases = bench::corpus();
  bench::BenchConfig c;
  CHECK(!bench::validate_config(c, cases).has_value());
  c.repetitions = 0;    CHECK(bench::validate_config(c, cases).has_value());
  c.repetitions = 1001; CHECK(bench::validate_config(c, cases).has_value());
  c.repetitions = 1000; CHECK(!bench::validate_config(c, cases).has_value());
  c.repetitions = 1;
  c.warmup = -1;        CHECK(bench::validate_config(c, cases).has_value());
  c.warmup = 101;       CHECK(bench::validate_config(c, cases).has_value());
  c.warmup = 100;       CHECK(!bench::validate_config(c, cases).has_value());
  c.warmup = 0;
  c.case_ids = {"no.such.case"};
  CHECK(bench::validate_config(c, cases).has_value());
  CHECK_THROWS_AS(bench::run(cases, c), std::invalid_argument);
  c.case_ids = {"grid.seed1", "grid.seed1"};
  CHECK(!bench::validate_config(c, cases).has_value());
  CHECK(bench::select_cases(c, cases).size() == 1);                    // selected once, in corpus order
  CHECK(!bench::validate_config(bench::BenchConfig{}, {}).has_value() == false);   // nothing to run
  CHECK(bench::parse_tier("smoke") && bench::parse_tier("extended") && !bench::parse_tier("huge"));
}

const char* kExpectedIds[] = {
    "grid.seed1", "puzzle.default.seed1", "logical.easy", "classify.easy", "targeted.easy.seed1",
    "targeted.master.exhaust.seed1", "grid.seed2", "puzzle.clues30.seed2", "logical.medium", "classify.medium",
    "logical.hard", "classify.hard", "logical.expert", "classify.expert", "logical.master", "classify.master",
    "logical.stalled", "classify.stalled", "targeted.medium.seed1", "targeted.hard.seed0",
    "targeted.expert.seed13", "targeted.master.seed8", "targeted.expert.exhaust.seed1",
    "targeted.expert.ext.seed0", "targeted.master.ext.seed0", "targeted.expert.ext.seed1", "targeted.master.ext.seed1",
    "targeted.expert.ext.seed2", "targeted.master.ext.seed2", "targeted.expert.ext.seed3", "targeted.master.ext.seed3",
    "targeted.expert.ext.seed4", "targeted.master.ext.seed4", "targeted.expert.ext.seed5", "targeted.master.ext.seed5"};

void test_corpus() {
  CHECK(bench::kCorpusVersion == "m16.1" && bench::kSchema == "sudoku.benchmark/1");
  const auto a = bench::corpus();
  const auto b = bench::corpus();
  CHECK(a.size() == std::size(kExpectedIds) && a.size() == b.size());
  std::set<std::string> ids;
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK_CTX(a[i].id == kExpectedIds[i], a[i].id);                    // stable identifiers and order
    CHECK_CTX(b[i].id == a[i].id && b[i].params_json == a[i].params_json && b[i].tier == a[i].tier &&
                  b[i].expected_status == a[i].expected_status && b[i].operation == a[i].operation, a[i].id);
    ids.insert(a[i].id);
    const Value params = must_parse(a[i].params_json, a[i].id);        // inputs are explicit JSON objects
    CHECK_CTX(params.is_object() && !params.members.empty(), a[i].id);
    CHECK_CTX(static_cast<bool>(a[i].run), a[i].id);
    CHECK_CTX(a[i].expected_status != "@PUZZLE_DEFAULT@" && a[i].expected_status.find('@') == std::string::npos, a[i].id);
  }
  CHECK(ids.size() == a.size());                                       // unique
  std::size_t smoke = 0, standard = 0;
  for (const auto& c : a) {
    smoke += c.tier == bench::Tier::Smoke;
    standard += c.tier != bench::Tier::Extended;
  }
  bench::BenchConfig cfg;
  cfg.tier = bench::Tier::Smoke;
  CHECK(bench::select_cases(cfg, a).size() == smoke && smoke == 6);
  cfg.tier = bench::Tier::Standard;
  CHECK(bench::select_cases(cfg, a).size() == standard && standard == 23);
  cfg.tier = bench::Tier::Extended;
  CHECK(bench::select_cases(cfg, a).size() == a.size());
  cfg.case_ids = {"targeted.expert.ext.seed3"};                        // explicit ids override the tier
  cfg.tier = bench::Tier::Smoke;
  CHECK(bench::select_cases(cfg, a).size() == 1);
  // The five operations are all represented in the smoke tier.
  std::set<std::string> ops;
  for (const auto& c : a) if (c.tier == bench::Tier::Smoke) ops.insert(c.operation);
  CHECK(ops == (std::set<std::string>{"complete_grid", "puzzle_generation", "logical_solve", "classification", "targeted_generation"}));
}

// ---------- smoke run: determinism, timing isolation, schema ----------
void check_schema(const Value& j, const bench::BenchConfig& cfg, std::size_t expected_cases) {
  CHECK(j.at("schema").text == "sudoku.benchmark/1" && j.at("corpus_version").text == "m16.1");
  const Value& env = j.at("environment");
  for (const char* k : {"compiler", "build_type", "os", "timer"}) CHECK_CTX(env.at(k).type == Value::Type::String, k);
  for (const char* k : {"optimization_enabled", "timer_is_steady"}) CHECK_CTX(env.at(k).type == Value::Type::Bool, k);
  CHECK(env.at("cxx_standard").integer() >= 202002 && env.at("pointer_bits").integer() >= 32);
  CHECK(j.at("config").at("repetitions").integer() == cfg.repetitions && j.at("config").at("warmup").integer() == cfg.warmup);
  CHECK(j.at("records").items.size() == expected_cases * static_cast<std::size_t>(cfg.repetitions));
  CHECK(j.at("summary").items.size() == expected_cases);
  for (const Value& r : j.at("records").items) {
    const std::string ctx = r.at("case").text;
    CHECK_CTX(r.at("params").is_object() && r.at("detail").is_object() && r.at("counters").is_object(), ctx);
    CHECK_CTX(r.at("elapsed_ns").is_integer_text() && r.at("elapsed_ns").integer() >= 0, ctx);
    CHECK_CTX(r.at("repetition").is_integer_text() && r.at("success").type == Value::Type::Bool, ctx);
    CHECK_CTX(r.at("matches_expected").type == Value::Type::Bool && r.at("status").type == Value::Type::String, ctx);
    for (const auto& [k, v] : r.at("counters").members) CHECK_CTX(v.is_integer_text(), ctx + "." + k);
  }
  for (const Value& s : j.at("summary").items) {
    const std::string ctx = s.at("case").text;
    CHECK_CTX(s.at("runs").integer() == cfg.repetitions, ctx);
    CHECK_CTX(s.at("timing").at("samples").integer() == cfg.repetitions, ctx);
    CHECK_CTX(s.at("timing").has("median_ns") && s.at("timing").has("min_ns"), ctx);
    CHECK_CTX(s.at("results_stable").boolean, ctx);
  }
  CHECK(j.at("ok").type == Value::Type::Bool);
}

void test_smoke_run() {
  const auto cases = bench::corpus();
  bench::BenchConfig cfg;
  cfg.tier = bench::Tier::Smoke;
  cfg.repetitions = 2;
  const bench::BenchReport first = bench::run(cases, cfg);
  const bench::BenchReport second = bench::run(cases, cfg);
  CHECK(first.ok && second.ok);
  CHECK(first.records.size() == 12);
  for (const auto& r : first.records) CHECK_CTX(r.matches_expected && r.error.empty(), r.case_id);

  // Algorithmic content is identical across runs; only elapsed_ns may differ.
  CHECK(first.records.size() == second.records.size());
  for (std::size_t i = 0; i < first.records.size(); ++i) {
    const auto& a = first.records[i];
    const auto& b = second.records[i];
    CHECK_CTX(a.case_id == b.case_id && a.operation == b.operation && a.params_json == b.params_json &&
                  a.repetition == b.repetition && a.status == b.status && a.success == b.success &&
                  a.counters == b.counters && a.detail_json == b.detail_json, a.case_id);
  }
  // Repetitions of one case agree with each other too.
  for (const auto& s : first.summaries) CHECK_CTX(s.results_stable, s.case_id);

  // Machine-readable output parses and has the documented shape, failures included.
  const Value j = must_parse(bench::report_json(first, bench::current_environment()), "smoke report");
  check_schema(j, cfg, 6);
  CHECK(j.at("ok").boolean);
  bool saw_exhausted = false;
  for (const Value& r : j.at("records").items) saw_exhausted = saw_exhausted || r.at("status").text == "exhausted";
  CHECK(saw_exhausted);   // the exhausted request is represented, not dropped

  // The separately kept counters agree with the diagnostics object they summarize.
  int targeted_records = 0;
  for (const auto& r : first.records) {
    if (r.operation != "targeted_generation") continue;
    ++targeted_records;
    const Value diag = must_parse(r.detail_json, r.case_id);
    const Value& rej = diag.at("rejections");
    CHECK_CTX(r.counters.at("attempts") == diag.at("attempts").integer(), r.case_id);
    CHECK_CTX(r.counters.at("max_attempts") == diag.at("max_attempts").integer(), r.case_id);
    CHECK_CTX(r.counters.at("rejected_easier") == rej.at("easier_than_target").integer(), r.case_id);
    CHECK_CTX(r.counters.at("rejected_harder") == rej.at("harder_than_target").integer(), r.case_id);
    CHECK_CTX(r.counters.at("rejected_unsolved") == rej.at("unsolved").integer(), r.case_id);
    CHECK_CTX(r.counters.at("rejected_degenerate") == rej.at("degenerate").integer(), r.case_id);
    CHECK_CTX(r.counters.at("rejected_inconclusive") == rej.at("inconclusive").integer(), r.case_id);
    CHECK_CTX(r.success == (diag.at("status").text == "generated"), r.case_id);
    CHECK_CTX(r.status == diag.at("status").text, r.case_id);
  }
  CHECK(targeted_records == 4);   // 2 cases x 2 repetitions

  // Warm-up changes nothing algorithmic.
  bench::BenchConfig warm = cfg;
  warm.warmup = 1;
  const bench::BenchReport w = bench::run(cases, warm);
  CHECK(w.records.size() == first.records.size());
  for (std::size_t i = 0; i < w.records.size(); ++i) {
    CHECK_CTX(w.records[i].counters == first.records[i].counters && w.records[i].detail_json == first.records[i].detail_json,
              w.records[i].case_id);
  }

  // Timing isolation: the harness result equals a direct, untimed call.
  TargetedOptions easy;
  easy.target = "Easy";
  easy.max_attempts = 60;
  const TargetedResult direct = generate_targeted(1, easy);
  for (const auto& r : first.records) {
    if (r.case_id != "targeted.easy.seed1") continue;
    CHECK(r.detail_json == targeted_diagnostics_json(direct, easy));
    CHECK(r.counters.at("attempts") == static_cast<std::int64_t>(direct.attempts));
    CHECK(r.status == "generated" && r.success);
  }
  TargetedOptions master;
  master.target = "Master";
  master.max_attempts = 3;
  const TargetedResult direct_fail = generate_targeted(1, master);
  for (const auto& r : first.records) {
    if (r.case_id != "targeted.master.exhaust.seed1") continue;
    CHECK(r.detail_json == targeted_diagnostics_json(direct_fail, master));
    CHECK(r.counters.at("attempts") == 3 && r.counters.at("max_attempts") == 3 && !r.success);
  }
}

}  // namespace

int main() {
  test_parser_is_strict();
  test_writer();
  test_diagnostics();
  test_summarize_times();
  test_runner_aggregation();
  test_failures_are_recorded();
  test_config_validation();
  test_corpus();
  test_smoke_run();
  if (testing_detail::failures() != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", testing_detail::failures());
  }
  return TEST_MAIN_RESULT();
}
