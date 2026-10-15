#include "sudoku/generator.hpp"

#include <random>
#include <stdexcept>

#include "search.hpp"

namespace sudoku {

GenerateResult generate_complete_grid(std::uint64_t seed,
                                      const GenerateOptions& options) {
  SearchOptions search_options;
  search_options.node_budget = options.node_budget;
  detail::check_options(search_options);

  std::mt19937_64 rng(seed);
  CountResult r = detail::Search(Board{}, 1, search_options, &rng).run();

  GenerateResult out;
  out.stats = r.stats;
  switch (r.status) {
    case SearchStatus::SolutionLimitReached:  // limit 1 reached: one grid
      if (!r.first_solution || !r.first_solution->is_valid_solution()) {
        throw std::logic_error("generator produced an invalid grid");
      }
      out.status = GenerateStatus::Generated;
      out.grid = std::move(r.first_solution);
      break;
    case SearchStatus::BudgetExhausted:
      out.status = GenerateStatus::BudgetExhausted;
      break;
    default:  // an empty board always has a solution; anything else is a bug
      throw std::logic_error("search of an empty board found no grid");
  }
  return out;
}

}  // namespace sudoku
