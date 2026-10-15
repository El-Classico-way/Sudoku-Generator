// Placeholder entry point. It exists to validate the build system only;
// the Sudoku engine and the JSON protocol arrive in later milestones.
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << "sudoku_cli " << SUDOKU_VERSION << '\n';
    return 0;
  }
  std::cerr << "usage: sudoku_cli --version\n";
  return 2;
}
