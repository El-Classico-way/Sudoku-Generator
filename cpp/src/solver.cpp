#include "sudoku/solver.hpp"

#include <stdexcept>

#include "search.hpp"

namespace sudoku {
namespace {

using detail::Search;
using detail::check_options;

}  // namespace

CountResult count_solutions(const Board& puzzle, std::uint64_t limit,
                            const SearchOptions& options) {
  if (limit == 0) throw std::invalid_argument("solution limit must be >= 1");
  check_options(options);
  if (!puzzle.is_consistent()) {
    CountResult invalid;
    invalid.status = SearchStatus::InvalidInput;
    return invalid;
  }
  return Search(puzzle, limit, options).run();
}

SolveResult find_solution(const Board& puzzle, const SearchOptions& options) {
  CountResult r = count_solutions(puzzle, 1, options);
  SolveResult out;
  out.stats = r.stats;
  switch (r.status) {
    case SearchStatus::InvalidInput:
      out.status = SolveStatus::InvalidInput;
      break;
    case SearchStatus::SolutionLimitReached:
      out.status = SolveStatus::Solved;
      out.solution = std::move(r.first_solution);
      break;
    case SearchStatus::Exhausted:
      out.status = SolveStatus::Unsolvable;  // limit 1, none found
      break;
    case SearchStatus::BudgetExhausted:
      out.status = SolveStatus::BudgetExhausted;
      break;
  }
  return out;
}

SolutionClass classify(const CountResult& r) noexcept {
  if (r.status == SearchStatus::InvalidInput) return SolutionClass::InvalidInput;
  if (r.count >= 2) return SolutionClass::Multiple;
  if (r.status == SearchStatus::Exhausted) {
    return r.count == 0 ? SolutionClass::NoSolution : SolutionClass::Unique;
  }
  return SolutionClass::Unknown;
}

SolutionClass classify_uniqueness(const Board& puzzle,
                                  const SearchOptions& options) {
  return classify(count_solutions(puzzle, 2, options));
}

}  // namespace sudoku
