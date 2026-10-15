#include "sudoku/pairs.hpp"

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

void collect_naked(const CandidateGrid& grid, std::vector<Deduction>& out) {
  for (UnitKind kind : kKinds) {
    for (int u = 0; u < kBoardSize; ++u) {
      const Unit unit{kind, u};
      const auto members = unit_cells(unit);
      for (int a = kMinDigit; a < kMaxDigit; ++a) {
        for (int b = a + 1; b <= kMaxDigit; ++b) {
          DigitSet pair;
          pair.insert(a);
          pair.insert(b);
          std::vector<int> holders;  // empty cells whose set is exactly {a, b}
          for (int c : members) {
            if (grid.candidates(c) == pair) holders.push_back(c);
          }
          if (holders.size() != 2) continue;  // 3+ is ambiguous, 1 is no pair
          std::vector<Elimination> targets;
          for (int c : members) {
            if (c == holders[0] || c == holders[1]) continue;
            for (int d : {a, b}) {
              if (grid.is_candidate(c, d)) targets.push_back({c, d});
            }
          }
          if (targets.empty()) continue;
          Evidence ev;
          ev.unit = unit;
          ev.cells = holders;
          ev.digits = pair;
          out.push_back(Deduction::eliminate(
              Technique::NakedPair, std::move(targets), std::move(ev),
              "in " + name_of(unit) + ", cells " + std::to_string(holders[0]) + " and " +
                  std::to_string(holders[1]) + " can only hold " + std::to_string(a) +
                  " and " + std::to_string(b) + ", so no other cell of the unit can"));
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
      // Candidate cells per digit (empty for a digit placed in the unit).
      std::array<std::vector<int>, kMaxDigit + 1> where;
      for (int d = kMinDigit; d <= kMaxDigit; ++d) {
        bool placed = false;
        for (int m : members) placed = placed || grid.board().get(m) == d;
        if (!placed) where[static_cast<std::size_t>(d)] = grid.cells_with_candidate(unit, d);
      }
      for (int a = kMinDigit; a < kMaxDigit; ++a) {
        for (int b = a + 1; b <= kMaxDigit; ++b) {
          const auto& wa = where[static_cast<std::size_t>(a)];
          const auto& wb = where[static_cast<std::size_t>(b)];
          if (wa.size() != 2 || wa != wb) continue;
          // Ambiguous if a third digit has the same two cells.
          int sharing = 0;
          for (int d = kMinDigit; d <= kMaxDigit; ++d) {
            sharing += where[static_cast<std::size_t>(d)] == wa ? 1 : 0;
          }
          if (sharing != 2) continue;
          std::vector<Elimination> targets;
          for (int c : wa) {
            for (int d : grid.candidates(c).digits()) {
              if (d != a && d != b) targets.push_back({c, d});
            }
          }
          if (targets.empty()) continue;
          Evidence ev;
          ev.unit = unit;
          ev.cells = wa;
          ev.digits.insert(a);
          ev.digits.insert(b);
          out.push_back(Deduction::eliminate(
              Technique::HiddenPair, std::move(targets), std::move(ev),
              "in " + name_of(unit) + ", digits " + std::to_string(a) + " and " +
                  std::to_string(b) + " can only go in cells " + std::to_string(wa[0]) +
                  " and " + std::to_string(wa[1]) + ", so those cells hold nothing else"));
        }
      }
    }
  }
}

}  // namespace

DetectResult find_naked_pairs(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_naked(grid, out.deductions);
  return out;
}

DetectResult find_hidden_pairs(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) collect_hidden(grid, out.deductions);
  return out;
}

DetectResult find_pairs(const CandidateGrid& grid) {
  DetectResult out;
  if (!contradiction_guard(grid, out)) {
    collect_naked(grid, out.deductions);
    collect_hidden(grid, out.deductions);
  }
  return out;
}

DetectResult find_pairs(const Board& board) {
  CandidateInitResult init = CandidateGrid::from_board(board);
  if (init.status != InitStatus::Ok) {
    DetectResult out;
    out.status = DetectStatus::InvalidBoard;
    out.conflicts = std::move(init.conflicts);
    return out;
  }
  return find_pairs(*init.grid);
}

}  // namespace sudoku
