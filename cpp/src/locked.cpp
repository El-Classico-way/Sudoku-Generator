#include "sudoku/locked.hpp"

#include <algorithm>
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

std::string name_of(const Unit& u) { return std::string(unit_name(u.kind)) + " " + std::to_string(u.index); }

bool holds_digit(const CandidateGrid& grid, const Unit& unit, int digit) {
  for (int c : unit_cells(unit)) {
    if (grid.board().get(c) == digit) return true;
  }
  return false;
}

bool contradiction_guard(const CandidateGrid& grid, DetectResult& out) {
  auto found = grid.find_contradictions();
  if (found.empty()) return false;
  out.status = DetectStatus::Contradiction;
  out.contradictions = std::move(found);
  return true;
}

// Cells of `target` that have `digit` as a candidate and are not in `source`.
std::vector<Elimination> targets_of(const CandidateGrid& grid, const Unit& target,
                                    const Unit& source, int digit) {
  const auto source_cells = unit_cells(source);
  std::vector<Elimination> out;
  for (int c : grid.cells_with_candidate(target, digit)) {
    if (std::find(source_cells.begin(), source_cells.end(), c) == source_cells.end()) {
      out.push_back({c, digit});
    }
  }
  return out;
}

void emit(std::vector<Deduction>& out, Technique tech, const Unit& source,
          const Unit& target, const std::vector<int>& positions, int digit,
          std::vector<Elimination> eliminations, const std::string& why) {
  Evidence ev;
  ev.unit = source;
  ev.target_unit = target;
  ev.cells = positions;
  ev.digits.insert(digit);
  out.push_back(Deduction::eliminate(tech, std::move(eliminations), std::move(ev), why));
}

void collect_pointing(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (int b = 0; b < kBoardSize; ++b) {
    const Unit box{UnitKind::Box, b};
    for (int d = kMinDigit; d <= kMaxDigit; ++d) {
      if (holds_digit(grid, box, d)) continue;
      const std::vector<int> where = grid.cells_with_candidate(box, d);
      if (where.empty()) continue;  // contradiction; excluded by the guard
      for (UnitKind kind : {UnitKind::Row, UnitKind::Column}) {
        const Unit line = unit_of(kind, where.front());
        const bool aligned = std::all_of(where.begin(), where.end(), [&](int c) {
          return unit_of(kind, c) == line; });
        if (!aligned) continue;
        auto targets = targets_of(grid, line, box, d);
        if (targets.empty()) continue;
        emit(out, Technique::Pointing, box, line, where, d, std::move(targets),
             "in " + name_of(box) + ", digit " + std::to_string(d) + " can only go in " +
                 name_of(line) + ", so it cannot go elsewhere in that " + unit_name(kind));
      }
    }
  }
}

void collect_claiming(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (UnitKind kind : {UnitKind::Row, UnitKind::Column}) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit line{kind, u};
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        if (holds_digit(grid, line, d)) continue;
        const std::vector<int> where = grid.cells_with_candidate(line, d);
        if (where.empty()) continue;  // contradiction; excluded by the guard
        const Unit box = unit_of(UnitKind::Box, where.front());
        const bool confined = std::all_of(where.begin(), where.end(), [&](int c) {
          return unit_of(UnitKind::Box, c) == box; });
        if (!confined) continue;
        auto targets = targets_of(grid, box, line, d);
        if (targets.empty()) continue;
        emit(out, Technique::Claiming, line, box, where, d, std::move(targets),
             "in " + name_of(line) + ", digit " + std::to_string(d) + " can only go in " +
                 name_of(box) + ", so it cannot go elsewhere in that box");
      }
    }
  }
}

}  // namespace

DetectResult find_pointing(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_pointing(grid, out.deductions);
  return out;
}

DetectResult find_claiming(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_claiming(grid, out.deductions);
  return out;
}

DetectResult find_locked_candidates(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) {
    collect_pointing(grid, out.deductions);
    collect_claiming(grid, out.deductions);
  }
  return out;
}

DetectResult find_locked_candidates(const Board& board) {
  CandidateInitResult init = CandidateGrid::from_board(board);
  if (init.status != InitStatus::Ok) {
    DetectResult out;
    out.status = DetectStatus::InvalidBoard;
    out.conflicts = std::move(init.conflicts);
    return out;
  }
  return find_locked_candidates(*init.grid);
}

}  // namespace sudoku
