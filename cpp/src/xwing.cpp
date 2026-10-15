#include "sudoku/xwing.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace sudoku {
namespace {

bool contradiction_guard(const CandidateGrid& grid, DetectResult& out) {
  auto found = grid.find_contradictions();
  if (found.empty()) return false;
  out.status = DetectStatus::Contradiction;
  out.contradictions = std::move(found);
  return true;
}

struct Base {
  int index;                 // row or column index of the base unit
  std::array<int, 2> cross;  // its two candidate positions (ascending)
};

void collect(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (int digit = kMinDigit; digit <= kMaxDigit; ++digit) {
    for (UnitKind kind : {UnitKind::Row, UnitKind::Column}) {
      const bool rows = kind == UnitKind::Row;
      // Base units with exactly two candidate positions for the digit.
      std::vector<Base> bases;
      for (int u = 0; u < kBoardSize; ++u) {
        const Unit unit{kind, u};
        bool placed = false;
        for (int m : unit_cells(unit)) placed = placed || grid.board().get(m) == digit;
        if (placed) continue;
        const std::vector<int> where = grid.cells_with_candidate(unit, digit);
        if (where.size() != 2) continue;
        const auto cross = [rows](int cell) { return rows ? cell_column(cell) : cell_row(cell); };
        bases.push_back({u, {cross(where[0]), cross(where[1])}});
      }
      for (std::size_t i = 0; i < bases.size(); ++i) {
        for (std::size_t j = i + 1; j < bases.size(); ++j) {
          if (bases[i].cross != bases[j].cross) continue;
          // Ambiguous if a third base unit shares the same positions.
          int sharing = 0;
          for (const Base& b : bases) sharing += b.cross == bases[i].cross ? 1 : 0;
          if (sharing != 2) continue;
          const int u1 = bases[i].index, u2 = bases[j].index;
          const int p1 = bases[i].cross[0], p2 = bases[i].cross[1];
          const auto cell_at = [rows](int base, int cross) {
            return rows ? base * kBoardSize + cross : cross * kBoardSize + base;
          };
          std::vector<int> corners = {cell_at(u1, p1), cell_at(u1, p2), cell_at(u2, p1), cell_at(u2, p2)};
          std::sort(corners.begin(), corners.end());
          // Targets: the cross units' other cells that still have the digit.
          std::vector<Elimination> targets;
          for (int p : {p1, p2}) {
            for (int cell : grid.cells_with_candidate(Unit{rows ? UnitKind::Column : UnitKind::Row, p}, digit)) {
              const int base = rows ? cell_row(cell) : cell_column(cell);
              if (base != u1 && base != u2) targets.push_back({cell, digit});
            }
          }
          if (targets.empty()) continue;
          std::sort(targets.begin(), targets.end(), [](const Elimination& x, const Elimination& y) {
            return x.cell < y.cell; });
          Evidence ev;
          ev.unit = Unit{kind, u1};
          ev.second_unit = Unit{kind, u2};
          ev.cells = corners;
          ev.digits.insert(digit);
          const std::string base_name = rows ? "rows " : "columns ";
          const std::string cross_name = rows ? "columns " : "rows ";
          out.push_back(Deduction::eliminate(
              Technique::XWing, std::move(targets), std::move(ev),
              "digit " + std::to_string(digit) + " has only two positions in " + base_name +
                  std::to_string(u1) + " and " + std::to_string(u2) + ", both in " + cross_name +
                  std::to_string(p1) + " and " + std::to_string(p2) +
                  ", so it cannot appear elsewhere in those " + cross_name));
        }
      }
    }
  }
}

}  // namespace

DetectResult find_x_wings(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect(grid, out.deductions);
  return out;
}

DetectResult find_x_wings(const Board& board) {
  CandidateInitResult init = CandidateGrid::from_board(board);
  if (init.status != InitStatus::Ok) {
    DetectResult out;
    out.status = DetectStatus::InvalidBoard;
    out.conflicts = std::move(init.conflicts);
    return out;
  }
  return find_x_wings(*init.grid);
}

}  // namespace sudoku
