// Private search engine shared by solver.cpp and generator.cpp. Not part of
// the public interface (lives in cpp/src, not cpp/include).
#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

#include "sudoku/solver.hpp"

namespace sudoku::detail {

// Candidate sets are bit masks: bit d (1..9) set <=> digit d present/legal.
// Bit 0 is never used. All masks are unsigned.
constexpr unsigned kAllDigits = ((1u << (kMaxDigit + 1)) - 1u) & ~1u;  // 0x3FE

inline int box_of(int index) {
  return (index / kBoardSize / kBoxSize) * kBoxSize +
         (index % kBoardSize) / kBoxSize;
}

// Fisher-Yates driven directly by the engine's raw output (specified by the
// standard) rather than std::shuffle / uniform_int_distribution, whose
// results are implementation-defined. The modulo bias (n <= 81 against 2^64
// values) is negligible and no uniformity claim is made.
inline void shuffle_in_place(std::mt19937_64& rng, int* a, int n) {
  for (int i = n - 1; i > 0; --i) {
    const auto j = static_cast<int>(rng() % static_cast<unsigned>(i + 1));
    std::swap(a[i], a[j]);
  }
}

class Search {
 public:
  // If `rng` is non-null, the candidate digits of every node are tried in a
  // random order drawn from *rng (the caller owns it and keeps it alive);
  // otherwise they are tried in ascending order.
  Search(const Board& puzzle, std::uint64_t limit, const SearchOptions& opts,
         std::mt19937_64* rng = nullptr)
      : limit_(limit), budget_(opts.node_budget), rng_(rng) {
    for (int i = 0; i < kCellCount; ++i) {
      const int d = puzzle.get(i);
      grid_[i] = d;
      if (d != kEmptyCell) place_masks(i, d);
    }
  }

  CountResult run() {
    CountResult out;
    descend(out);
    out.stats = stats_;
    out.count = found_;
    if (stop_ == Stop::Limit) {
      out.status = SearchStatus::SolutionLimitReached;
    } else if (stop_ == Stop::Budget) {
      out.status = SearchStatus::BudgetExhausted;
    } else {
      out.status = SearchStatus::Exhausted;
    }
    return out;
  }

 private:
  enum class Stop { None, Limit, Budget };

  void place_masks(int i, int d) {
    const unsigned bit = 1u << d;
    rows_[i / kBoardSize] |= bit;
    cols_[i % kBoardSize] |= bit;
    boxes_[box_of(i)] |= bit;
  }
  void clear_masks(int i, int d) {
    const unsigned bit = 1u << d;
    rows_[i / kBoardSize] &= ~bit;
    cols_[i % kBoardSize] &= ~bit;
    boxes_[box_of(i)] &= ~bit;
  }
  unsigned candidates(int i) const {
    return kAllDigits &
           ~(rows_[i / kBoardSize] | cols_[i % kBoardSize] | boxes_[box_of(i)]);
  }

  void descend(CountResult& out) {
    if (budget_ && stats_.nodes >= *budget_) {
      stop_ = Stop::Budget;
      return;
    }
    ++stats_.nodes;

    // MRV: first empty cell with the fewest candidates (ties: lowest index).
    int best = -1;
    unsigned best_mask = 0;
    int best_count = kBoardSize + 1;
    for (int i = 0; i < kCellCount; ++i) {
      if (grid_[i] != kEmptyCell) continue;
      const unsigned mask = candidates(i);
      const int n = std::popcount(mask);
      if (n < best_count) {
        best = i;
        best_mask = mask;
        best_count = n;
        if (n == 0) return;  // dead branch
      }
    }

    if (best < 0) {  // grid full
      record_solution(out);
      return;
    }

    int order[kBoardSize];
    int n = 0;
    while (best_mask != 0) {  // ascending digit order
      order[n++] = std::countr_zero(best_mask);
      best_mask &= best_mask - 1;
    }
    if (rng_ != nullptr) shuffle_in_place(*rng_, order, n);

    for (int k = 0; k < n; ++k) {
      const int d = order[k];
      grid_[best] = d;
      place_masks(best, d);
      ++stats_.assignments;
      descend(out);
      clear_masks(best, d);
      grid_[best] = kEmptyCell;
      ++stats_.backtracks;
      if (stop_ != Stop::None) return;
    }
  }

  void record_solution(CountResult& out) {
    Board board;
    for (int i = 0; i < kCellCount; ++i) board.set(i, grid_[i]);
    if (!board.is_valid_solution()) {
      throw std::logic_error("solver produced an invalid grid");
    }
    ++found_;
    if (!out.first_solution) out.first_solution = board;
    if (found_ >= limit_) stop_ = Stop::Limit;
  }

  int grid_[kCellCount] = {};
  unsigned rows_[kBoardSize] = {};
  unsigned cols_[kBoardSize] = {};
  unsigned boxes_[kBoardSize] = {};
  std::uint64_t limit_;
  std::optional<std::uint64_t> budget_;
  std::uint64_t found_ = 0;
  SearchStats stats_;
  Stop stop_ = Stop::None;
  std::mt19937_64* rng_;
};

inline void check_options(const SearchOptions& options) {
  if (options.node_budget && *options.node_budget == 0) {
    throw std::invalid_argument("node_budget must be nonzero");
  }
}

}  // namespace sudoku::detail
