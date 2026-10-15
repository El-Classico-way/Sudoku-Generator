// sudoku_cli: the engine's command-line interface. One JSON request per process:
// the request is read from standard input (or --input FILE), exactly one JSON
// response is written to standard output (or --output FILE) followed by a
// newline. Standard output carries nothing else; diagnostics go to standard
// error. See sudoku/protocol.hpp and the README ("JSON protocol and CLI").
//
//   sudoku_cli                      read a request from stdin, write the response to stdout
//   sudoku_cli --input F --output G read/write files instead
//   sudoku_cli --help | --version   plain text, exit 0
//
// Exit codes: 0 success; 1 valid request, non-success engine outcome;
// 2 invalid request or invalid command line; 3 internal/operational failure.
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include "sudoku/protocol.hpp"

namespace {

void usage(std::FILE* to) {
  std::fputs(
      "usage: sudoku_cli [--input FILE] [--output FILE]\n"
      "       sudoku_cli --help | --version\n"
      "\n"
      "Reads one JSON request (schema sudoku.request/1) from standard input or FILE and\n"
      "writes one JSON response (sudoku.response/1) plus a newline to standard output or\n"
      "FILE. Standard error carries diagnostics only.\n"
      "\n"
      "exit codes: 0 success; 1 valid request, non-success outcome (for example\n"
      "exhausted or stalled); 2 invalid request or command line; 3 internal failure\n",
      to);
}

// Reads at most kMaxRequestBytes + 1 bytes; `too_large` is set if the input is longer.
bool read_all(std::FILE* in, std::string& out, bool& too_large) {
  too_large = false;
  char buffer[65536];
  const std::size_t limit = sudoku::kMaxRequestBytes;
  for (;;) {
    const std::size_t n = std::fread(buffer, 1, sizeof buffer, in);
    if (n > 0) {
      if (out.size() + n > limit) {
        too_large = true;
        return true;   // stop reading; the request is rejected unparsed
      }
      out.append(buffer, n);
    }
    if (n < sizeof buffer) return std::ferror(in) == 0;
  }
}

int emit(const sudoku::ProtocolOutcome& outcome, const std::string& output_path) {
  if (!outcome.internal_detail.empty()) std::fprintf(stderr, "sudoku_cli: %s\n", outcome.internal_detail.c_str());
  const std::string text = outcome.response_json + "\n";
  if (output_path.empty()) {
    if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() || std::fflush(stdout) != 0) {
      std::fputs("sudoku_cli: could not write the response to standard output\n", stderr);
      return sudoku::kExitInternalError;
    }
  } else {
    std::ofstream out(output_path, std::ios::binary);
    out << text;
    out.flush();
    if (!out) {
      std::fprintf(stderr, "sudoku_cli: could not write %s\n", output_path.c_str());
      return sudoku::kExitInternalError;
    }
  }
  return outcome.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path, output_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version" && argc == 2) {
      std::printf("sudoku_cli %s\n", SUDOKU_VERSION);
      return 0;
    }
    if (arg == "--help" && argc == 2) {
      usage(stdout);
      return 0;
    }
    if ((arg == "--input" || arg == "--output") && i + 1 < argc) {
      (arg == "--input" ? input_path : output_path) = argv[++i];
      continue;
    }
    std::fprintf(stderr, "sudoku_cli: unknown or incomplete argument: %s\n", arg.c_str());
    usage(stderr);
    return sudoku::kExitProtocolError;   // no request was read, so no response is written
  }

  try {
    std::string text;
    bool too_large = false;
    bool ok = true;
    if (input_path.empty()) {
      ok = read_all(stdin, text, too_large);
    } else {
      std::FILE* f = std::fopen(input_path.c_str(), "rb");
      if (f == nullptr) {
        return emit(sudoku::internal_error_outcome("cannot open input file: " + input_path), output_path);
      }
      ok = read_all(f, text, too_large);
      std::fclose(f);
    }
    if (!ok) return emit(sudoku::internal_error_outcome("error while reading the request"), output_path);
    if (too_large) return emit(sudoku::request_too_large_outcome(), output_path);
    return emit(sudoku::handle_request(text), output_path);
  } catch (const std::exception& e) {
    return emit(sudoku::internal_error_outcome(e.what()), output_path);
  } catch (...) {
    return emit(sudoku::internal_error_outcome("unknown exception"), output_path);
  }
}
