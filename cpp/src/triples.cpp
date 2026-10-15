#include "sudoku/triples.hpp"

#include <algorithm>
#include <array>
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

bool contradiction_guard(const CandidateGrid& grid, DetectResult& out) {
  auto found = grid.find_contradictions();
  if (found.empty()) return false;
  out.status = DetectStatus::Contradiction;
  out.contradictions = std::move(found);
  return true;
}

constexpr UnitKind kKinds[] = {UnitKind::Row, UnitKind::Column, UnitKind::Box};

std::string digits_text(const DigitSet& s) {
  std::string out;
  for (int d : s.digits()) out += (out.empty() ? "" : ", ") + std::to_string(d);
  return out;
}

void collect_naked(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (UnitKind kind : kKinds) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit unit{kind, u};
      const auto members = unit_cells(unit);
      for (int a = kMinDigit; a <= kMaxDigit - 2; ++a) {
        for (int b = a + 1; b <= kMaxDigit - 1; ++b) {
          for (int c = b + 1; c <= kMaxDigit; ++c) {
            DigitSet triple;
            triple.insert(a);
            triple.insert(b);
            triple.insert(c);
            std::vector<int> confined;  // empty cells with candidates inside T
            bool ok = true;
            unsigned seen = 0;
            for (int cell : members) {
              const DigitSet set = grid.candidates(cell);
              if (set.empty() || (set.mask() & ~triple.mask()) != 0) continue;
              confined.push_back(cell);
              if (set.size() < 2) ok = false;  // a single inside T: not a triple
              seen |= set.mask();
            }
            if (!ok || confined.size() != 3 || seen != triple.mask()) continue;
            std::vector<Elimination> targets;
            for (int cell : members) {
              if (cell == confined[0] || cell == confined[1] || cell == confined[2]) continue;
              for (int d : {a, b, c}) {
                if (grid.is_candidate(cell, d)) targets.push_back({cell, d});
              }
            }
            if (targets.empty()) continue;
            Evidence ev;
            ev.unit = unit;
            ev.cells = confined;
            ev.digits = triple;
            out.push_back(Deduction::eliminate(
                Technique::NakedTriple, std::move(targets), std::move(ev),
                "in " + name_of(unit) + ", cells " + std::to_string(confined[0]) + ", " +
                    std::to_string(confined[1]) + " and " + std::to_string(confined[2]) +
                    " can only hold " + digits_text(triple) +
                    ", so no other cell of the unit can"));
          }
        }
      }
    }
  }
}

void collect_hidden(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (UnitKind kind : kKinds) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit unit{kind, u};
      const auto members = unit_cells(unit);
      std::array<std::vector<int>, kMaxDigit + 1> where;  // empty if placed
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        bool placed = false;
        for (int m : members) placed = placed || grid.board().get(m) == d;
        if (!placed) where[static_cast<std::size_t>(d)] = grid.cells_with_candidate(unit, d);
      }
      for (int a = kMinDigit; a <= kMaxDigit - 2; ++a) {
        for (int b = a + 1; b <= kMaxDigit - 1; ++b) {
          for (int c = b + 1; c <= kMaxDigit; ++c) {
            const int digits[3] = {a, b, c};
            bool ok = true;
            std::vector<int> cells;  // union of candidate cells, ascending
            for (int d : digits) {
              const auto& w = where[static_cast<std::size_t>(d)];
              if (w.size() < 2 || w.size() > 3) ok = false;
              for (int cell : w) {
                if (std::find(cells.begin(), cells.end(), cell) == cells.end()) cells.push_back(cell);
              }
            }
            if (!ok || cells.size() != 3) continue;
            std::sort(cells.begin(), cells.end());
            // A fourth digit confined to the same three cells is impossible.
            int confined_digits = 0;
            for (int d = kMinDigit; d <= kMaxDigit; ++d) {
              const auto& w = where[static_cast<std::size_t>(d)];
              if (w.empty()) continue;
              const bool inside = std::all_of(w.begin(), w.end(), [&](int cell) {
                return std::find(cells.begin(), cells.end(), cell) != cells.end(); });
              confined_digits += inside ? 1 : 0;
            }
            if (confined_digits != 3) continue;
            std::vector<Elimination> targets;
            for (int cell : cells) {
              for (int d : grid.candidates(cell).digits()) {
                if (d != a && d != b && d != c) targets.push_back({cell, d});
              }
            }
            if (targets.empty()) continue;
            Evidence ev;
            ev.unit = unit;
            ev.cells = cells;
            ev.digits.insert(a);
            ev.digits.insert(b);
            ev.digits.insert(c);
            const std::string digit_list = digits_text(ev.digits);
            out.push_back(Deduction::eliminate(
                Technique::HiddenTriple, std::move(targets), std::move(ev),
                "in " + name_of(unit) + ", digits " + digit_list +
                    " can only go in cells " + std::to_string(cells[0]) + ", " +
                    std::to_string(cells[1]) + " and " + std::to_string(cells[2]) +
                    ", so those cells hold nothing else"));
          }
        }
      }
    }
  }
}

}  // namespace

DetectResult find_naked_triples(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_naked(grid, out.deductions);
  return out;
}

DetectResult find_hidden_triples(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_hidden(grid, out.deductions);
  return out;
}

DetectResult find_triples(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) {
    collect_naked(grid, out.deductions);
    collect_hidden(grid, out.deductions);
  }
  return out;
}

DetectResult find_triples(const Board& board) {
  CandidateInitResult init = CandidateGrid::from_board(board);
  if (init.status != InitStatus::Ok) {
    DetectResult out;
    out.status = DetectStatus::InvalidBoard;
    out.conflicts = std::move(init.conflicts);
    return out;
  }
  return find_triples(*init.grid);
}

}  // namespace sudoku
