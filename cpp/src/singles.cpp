#include "sudoku/singles.hpp"

#include <string>

namespace sudoku {
namespace {

const char* unit_name(UnitKind kind) {
  switch (kind) {
    case UnitKind::Row: return "row";
    case UnitKind::Column: return "column";
    case UnitKind::Box: return "box";
  }
  return "unit";
}

std::string describe(int cell) {
  return "cell " + std::to_string(cell) + " (row " + std::to_string(cell_row(cell)) +
         ", column " + std::to_string(cell_column(cell)) + ")";
}

// Shared guard: refuse to produce deductions from a contradictory grid.
bool contradiction_guard(const CandidateGrid& grid, DetectResult& out) {
  auto found = grid.find_contradictions();
  if (found.empty()) return false;
  out.status = DetectStatus::Contradiction;
  out.contradictions = std::move(found);
  return true;
}

void collect_naked(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (int cell = 0; cell < kCellCount; ++cell) {
    if (grid.board().get(cell) != kEmptyCell) continue;
    const DigitSet set = grid.candidates(cell);
    if (set.size() != 1) continue;
    const int digit = set.digits().front();
    Evidence ev;
    ev.cells = {cell};
    ev.digits = set;
    out.push_back(Deduction::place(
        Technique::NakedSingle, cell, digit, std::move(ev),
        describe(cell) + " has only one candidate: " + std::to_string(digit)));
  }
}

void collect_hidden(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column, UnitKind::Box}) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit unit{kind, u};
      const auto members = unit_cells(unit);
      for (int digit = kMinDigit; digit <= kMaxDigit; ++digit) {
        bool placed = false;
        for (int m : members) placed = placed || grid.board().get(m) == digit;
        if (placed) continue;  // already in the unit: nothing to find
        const std::vector<int> where = grid.cells_with_candidate(unit, digit);
        if (where.size() != 1) continue;  // 0 is a contradiction (guarded)
        Evidence ev;
        ev.unit = unit;
        ev.cells = where;
        ev.digits.insert(digit);
        out.push_back(Deduction::place(
            Technique::HiddenSingle, where[0], digit, std::move(ev),
            "in " + std::string(unit_name(kind)) + " " + std::to_string(u) + ", digit " +
                std::to_string(digit) + " can only go in " + describe(where[0])));
      }
    }
  }
}

}  // namespace

DetectResult find_naked_singles(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_naked(grid, out.deductions);
  return out;
}

DetectResult find_hidden_singles(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_hidden(grid, out.deductions);
  return out;
}

DetectResult find_singles(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) {
    collect_naked(grid, out.deductions);
    collect_hidden(grid, out.deductions);
  }
  return out;
}

DetectResult find_singles(const Board& board) {
  CandidateInitResult init = CandidateGrid::from_board(board);
  if (init.status != InitStatus::Ok) {
    DetectResult out;
    out.status = DetectStatus::InvalidBoard;
    out.conflicts = std::move(init.conflicts);
    return out;
  }
  return find_singles(*init.grid);
}

}  // namespace sudoku
