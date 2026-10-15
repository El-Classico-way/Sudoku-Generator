// sudoku_bench: runs the fixed benchmark corpus and prints a JSON report.
// A benchmark tool, not the application CLI.
//
//   sudoku_bench [--tier smoke|standard|extended] [--case ID]... [--repetitions N]
//                [--warmup N] [--output FILE] [--list] [--help]
//
// Exit status: 0 all outcomes as expected; 1 the run completed but some outcome
// was unexpected, a case threw, or results were unstable (the report is still
// written); 2 invalid command line or configuration (nothing was run).
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "bench.hpp"

namespace {

void usage(std::ostream& os) {
  os << "usage: sudoku_bench [--tier smoke|standard|extended] [--case ID]... [--repetitions N]\n"
        "                    [--warmup N] [--output FILE] [--list] [--help]\n"
        "  --tier         cases to run: smoke < standard < extended (default standard)\n"
        "  --case         run exactly this case id (repeatable; overrides --tier)\n"
        "  --repetitions  measured runs per case, 1..1000 (default 1)\n"
        "  --warmup       unrecorded runs per case before measuring, 0..100 (default 0)\n"
        "  --output       write the JSON report to FILE instead of stdout\n"
        "  --list         list the cases (id, tier, operation) and exit\n";
}

bool parse_int(const std::string& text, int& out) {
  try {
    std::size_t used = 0;
    const long v = std::stol(text, &used);
    if (used != text.size() || v < -1000000 || v > 1000000) return false;
    out = static_cast<int>(v);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace sudoku::bench;
  BenchConfig config;
  std::string output;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) { std::cerr << "missing value for " << arg << "\n"; return false; }
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (arg == "--help") { usage(std::cout); return 0; }
    else if (arg == "--list") list = true;
    else if (arg == "--tier") {
      if (!value(v)) return 2;
      const auto tier = parse_tier(v);
      if (!tier) { std::cerr << "unknown tier: " << v << "\n"; return 2; }
      config.tier = *tier;
    } else if (arg == "--case") {
      if (!value(v)) return 2;
      config.case_ids.push_back(v);
    } else if (arg == "--repetitions") {
      if (!value(v) || !parse_int(v, config.repetitions)) { std::cerr << "invalid --repetitions\n"; return 2; }
    } else if (arg == "--warmup") {
      if (!value(v) || !parse_int(v, config.warmup)) { std::cerr << "invalid --warmup\n"; return 2; }
    } else if (arg == "--output") {
      if (!value(output)) return 2;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      usage(std::cerr);
      return 2;
    }
  }

  const std::vector<BenchCase> cases = corpus();
  if (list) {
    for (const BenchCase& c : cases) {
      std::cout << c.id << "\t" << to_string(c.tier) << "\t" << c.operation << "\n";
    }
    return 0;
  }
  if (const auto error = validate_config(config, cases)) {
    std::cerr << "invalid configuration: " << *error << "\n";
    return 2;
  }
  const BenchReport report = run(cases, config);
  const std::string json = report_json(report, current_environment());
  if (output.empty()) {
    std::cout << json << "\n";
  } else {
    std::ofstream out(output, std::ios::binary);
    out << json << "\n";
    if (!out) { std::cerr << "could not write " << output << "\n"; return 2; }
  }
  return report.ok ? 0 : 1;
}
