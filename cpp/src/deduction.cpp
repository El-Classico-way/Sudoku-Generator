#include "sudoku/deduction.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace sudoku {
namespace {

void validate_target(const Elimination& e) {
  if (e.cell < 0 || e.cell >= kCellCount) throw std::out_of_range("deduction cell out of range");
  if (e.digit < kMinDigit || e.digit > kMaxDigit) throw std::invalid_argument("deduction digit must be 1..9");
}

void validate_common(const Evidence& evidence, const std::string& explanation) {
  if (explanation.empty()) throw std::invalid_argument("deduction needs an explanation");
  for (int c : evidence.cells) {
    if (c < 0 || c >= kCellCount) throw std::out_of_range("evidence cell out of range");
  }
  if (evidence.unit) unit_cells(*evidence.unit);  // validates the unit index
  if (evidence.target_unit) unit_cells(*evidence.target_unit);
  if (evidence.second_unit) unit_cells(*evidence.second_unit);
}

}  // namespace

std::string_view technique_name(Technique t) noexcept {
  switch (t) {
    case Technique::NakedSingle: return "naked single";
    case Technique::HiddenSingle: return "hidden single";
    case Technique::Pointing: return "pointing";
    case Technique::Claiming: return "claiming";
    case Technique::NakedPair: return "naked pair";
    case Technique::HiddenPair: return "hidden pair";
    case Technique::NakedTriple: return "naked triple";
    case Technique::HiddenTriple: return "hidden triple";
    case Technique::XWing: return "X-Wing";
  }
  return "unknown";
}

Deduction Deduction::place(Technique technique, int cell, int digit,
                           Evidence evidence, std::string explanation) {
  const Elimination target{cell, digit};
  validate_target(target);
  validate_common(evidence, explanation);
  Deduction d;
  d.technique_ = technique;
  d.action_ = DeductionAction::PlaceDigit;
  d.targets_ = {target};
  d.evidence_ = std::move(evidence);
  d.explanation_ = std::move(explanation);
  return d;
}

Deduction Deduction::eliminate(Technique technique,
                               std::vector<Elimination> eliminations,
                               Evidence evidence, std::string explanation) {
  if (eliminations.empty()) throw std::invalid_argument("elimination list is empty");
  for (const Elimination& e : eliminations) validate_target(e);
  for (std::size_t i = 0; i < eliminations.size(); ++i) {
    if (std::find(eliminations.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                  eliminations.end(), eliminations[i]) != eliminations.end()) {
      throw std::invalid_argument("duplicate elimination");
    }
  }
  validate_common(evidence, explanation);
  Deduction d;
  d.technique_ = technique;
  d.action_ = DeductionAction::EliminateCandidates;
  d.targets_ = std::move(eliminations);
  d.evidence_ = std::move(evidence);
  d.explanation_ = std::move(explanation);
  return d;
}

namespace {

bool contains_cell(const std::array<int, kBoardSize>& cells, int cell) {
  return std::find(cells.begin(), cells.end(), cell) != cells.end();
}

// Does the current grid still support a Pointing/Claiming elimination?
bool locked_supported(const CandidateGrid& grid, const Deduction& d) {
  const Evidence& ev = d.evidence();
  if (!ev.unit || !ev.target_unit || ev.digits.size() != 1) return false;
  const Unit src = *ev.unit, tgt = *ev.target_unit;
  const bool line_kind = [](UnitKind k) { return k != UnitKind::Box; }(src.kind);
  if (d.technique() == Technique::Pointing) {
    if (src.kind != UnitKind::Box || tgt.kind == UnitKind::Box) return false;
  } else {
    if (!line_kind || tgt.kind != UnitKind::Box) return false;
  }
  const int digit = ev.digits.digits().front();
  const auto src_cells = unit_cells(src);
  const auto tgt_cells = unit_cells(tgt);
  for (int c : src_cells) {
    if (grid.board().get(c) == digit) return false;  // digit already placed
  }
  const std::vector<int> where = grid.cells_with_candidate(src, digit);
  if (where.empty()) return false;
  for (int c : where) {
    if (!contains_cell(tgt_cells, c)) return false;  // no longer confined
  }
  for (const Elimination& t : d.targets()) {
    if (t.digit != digit || !contains_cell(tgt_cells, t.cell) ||
        contains_cell(src_cells, t.cell)) {
      return false;
    }
  }
  return true;
}

bool pattern_shape_ok(const Evidence& ev, std::size_t n);

// Does the current grid still support a NakedPair/HiddenPair elimination?
bool pair_supported(const CandidateGrid& grid, const Deduction& d) {
  const Evidence& ev = d.evidence();
  if (!pattern_shape_ok(ev, 2)) return false;
  const auto members = unit_cells(*ev.unit);
  const int c0 = ev.cells[0], c1 = ev.cells[1];
  const std::vector<int> pair = ev.digits.digits();
  if (d.technique() == Technique::NakedPair) {
    if (grid.candidates(c0) != ev.digits || grid.candidates(c1) != ev.digits) return false;
    for (const Elimination& t : d.targets()) {
      if (!ev.digits.contains(t.digit) || !contains_cell(members, t.cell) ||
          t.cell == c0 || t.cell == c1) {
        return false;
      }
    }
    return true;
  }
  const std::vector<int> expected = c0 < c1 ? std::vector<int>{c0, c1} : std::vector<int>{c1, c0};
  for (int digit : pair) {
    for (int m : members) {
      if (grid.board().get(m) == digit) return false;  // placed: not eligible
    }
    if (grid.cells_with_candidate(*ev.unit, digit) != expected) return false;
  }
  for (const Elimination& t : d.targets()) {
    if (ev.digits.contains(t.digit) || (t.cell != c0 && t.cell != c1)) return false;
  }
  return true;
}

// Shared evidence-shape check for N-cell / N-digit patterns (pairs: N=2,
// triples: N=3): a source unit, N distinct cells inside it, N digits.
bool pattern_shape_ok(const Evidence& ev, std::size_t n) {
  if (!ev.unit || ev.cells.size() != n || ev.digits.size() != static_cast<int>(n)) return false;
  const auto members = unit_cells(*ev.unit);
  for (std::size_t i = 0; i < n; ++i) {
    if (!contains_cell(members, ev.cells[i])) return false;
    for (std::size_t j = i + 1; j < n; ++j) {
      if (ev.cells[i] == ev.cells[j]) return false;
    }
  }
  return true;
}

// Does the current grid still support a NakedTriple/HiddenTriple elimination?
bool triple_supported(const CandidateGrid& grid, const Deduction& d) {
  const Evidence& ev = d.evidence();
  if (!pattern_shape_ok(ev, 3)) return false;
  const auto members = unit_cells(*ev.unit);
  const auto in_triple = [&](int cell) {
    return std::find(ev.cells.begin(), ev.cells.end(), cell) != ev.cells.end();
  };
  if (d.technique() == Technique::NakedTriple) {
    unsigned seen = 0;
    for (int c : ev.cells) {
      if (grid.board().get(c) != kEmptyCell) return false;
      const DigitSet set = grid.candidates(c);
      if (set.empty() || (set.mask() & ~ev.digits.mask()) != 0) return false;
      seen |= set.mask();
    }
    if (seen != ev.digits.mask()) return false;  // union must be exactly the digits
    for (const Elimination& t : d.targets()) {
      if (!ev.digits.contains(t.digit) || !contains_cell(members, t.cell) || in_triple(t.cell)) {
        return false;
      }
    }
    return true;
  }
  // HiddenTriple
  std::vector<int> union_cells;
  for (int digit : ev.digits.digits()) {
    for (int m : members) {
      if (grid.board().get(m) == digit) return false;  // placed: not eligible
    }
    const std::vector<int> where = grid.cells_with_candidate(*ev.unit, digit);
    if (where.empty()) return false;
    for (int c : where) {
      if (!in_triple(c)) return false;  // no longer confined to the three cells
      if (std::find(union_cells.begin(), union_cells.end(), c) == union_cells.end()) {
        union_cells.push_back(c);
      }
    }
  }
  if (union_cells.size() != 3) return false;  // each cell must still be needed
  for (const Elimination& t : d.targets()) {
    if (ev.digits.contains(t.digit) || !in_triple(t.cell)) return false;
  }
  return true;
}

// Does the current grid still support an XWing elimination?
bool xwing_supported(const CandidateGrid& grid, const Deduction& d) {
  const Evidence& ev = d.evidence();
  if (!ev.unit || !ev.second_unit || ev.digits.size() != 1 || ev.cells.size() != 4) return false;
  const Unit a = *ev.unit, b = *ev.second_unit;
  if (a.kind == UnitKind::Box || a.kind != b.kind || a.index == b.index) return false;
  const int digit = ev.digits.digits().front();
  const bool rows = a.kind == UnitKind::Row;
  const auto cross = [rows](int cell) { return rows ? cell_column(cell) : cell_row(cell); };
  const auto base = [rows](int cell) { return rows ? cell_row(cell) : cell_column(cell); };
  std::vector<int> positions[2];
  const Unit bases[2] = {a, b};
  for (int k = 0; k < 2; ++k) {
    for (int m : unit_cells(bases[k])) {
      if (grid.board().get(m) == digit) return false;  // placed: not eligible
    }
    const std::vector<int> where = grid.cells_with_candidate(bases[k], digit);
    if (where.size() != 2) return false;  // exactly two positions, no more or less
    positions[k] = {cross(where[0]), cross(where[1])};
  }
  if (positions[0] != positions[1]) return false;
  std::vector<int> corners;
  for (const Unit& u : bases) {
    for (int p : positions[0]) corners.push_back(rows ? u.index * kBoardSize + p : p * kBoardSize + u.index);
  }
  std::sort(corners.begin(), corners.end());
  std::vector<int> given = ev.cells;
  std::sort(given.begin(), given.end());
  if (given != corners) return false;
  for (const Elimination& t : d.targets()) {
    const int c = cross(t.cell);
    if (t.digit != digit || (c != positions[0][0] && c != positions[0][1])) return false;
    if (base(t.cell) == a.index || base(t.cell) == b.index) return false;
  }
  return true;
}

AppliedDeduction apply_placement(CandidateGrid& grid, const Deduction& deduction) {
  AppliedDeduction out{deduction, ApplyStatus::Rejected, {}};
  const Technique tech = deduction.technique();
  if (tech != Technique::NakedSingle && tech != Technique::HiddenSingle) return out;
  const int cell = deduction.targets()[0].cell;
  const int digit = deduction.targets()[0].digit;
  if (grid.board().get(cell) != kEmptyCell || !grid.is_candidate(cell, digit)) return out;

  out.status = ApplyStatus::Unsupported;
  if (tech == Technique::NakedSingle) {
    if (grid.candidates(cell).size() != 1) return out;
  } else {
    const auto& unit = deduction.evidence().unit;
    if (!unit) return out;
    const auto members = unit_cells(*unit);
    if (std::find(members.begin(), members.end(), cell) == members.end()) return out;
    for (int m : members) {
      if (grid.board().get(m) == digit) return out;
    }
    if (grid.cells_with_candidate(*unit, digit) != std::vector<int>{cell}) return out;
  }

  const CandidateGrid before = grid;
  switch (grid.place(cell, digit)) {
    case PlaceOutcome::Placed: break;
    case PlaceOutcome::WouldEmptyPeer: out.status = ApplyStatus::WouldContradict; return out;
    default: out.status = ApplyStatus::Rejected; return out;
  }
  for (int c = 0; c < kCellCount; ++c) {
    for (int d : before.candidates(c).digits()) {
      if (!grid.candidates(c).contains(d)) out.effective.push_back({c, d});
    }
  }
  out.status = ApplyStatus::Applied;
  return out;
}

}  // namespace

AppliedDeduction apply(CandidateGrid& grid, const Deduction& deduction) {
  if (deduction.action() == DeductionAction::PlaceDigit) {
    return apply_placement(grid, deduction);
  }
  AppliedDeduction out{deduction, ApplyStatus::NoChange, {}};
  bool supported = false;
  switch (deduction.technique()) {
    case Technique::Pointing:
    case Technique::Claiming: supported = locked_supported(grid, deduction); break;
    case Technique::NakedPair:
    case Technique::HiddenPair: supported = pair_supported(grid, deduction); break;
    case Technique::NakedTriple:
    case Technique::HiddenTriple: supported = triple_supported(grid, deduction); break;
    case Technique::XWing: supported = xwing_supported(grid, deduction); break;
    // The singles only place digits and have no elimination evidence, and any
    // value outside the enum is unknown: neither may change the grid.
    case Technique::NakedSingle:
    case Technique::HiddenSingle:
    default: supported = false; break;
  }
  if (!supported) {
    out.status = ApplyStatus::Unsupported;
    return out;
  }
  CandidateGrid trial = grid;  // all-or-nothing: work on a copy
  for (const Elimination& e : deduction.targets()) {
    switch (trial.eliminate(e.cell, e.digit)) {
      case EliminateOutcome::Eliminated: out.effective.push_back(e); break;
      case EliminateOutcome::AlreadyAbsent: break;
      case EliminateOutcome::FilledCell:
        out.status = ApplyStatus::Rejected;
        out.effective.clear();
        return out;
      case EliminateOutcome::WouldEmptyCell:
        out.status = ApplyStatus::WouldContradict;
        out.effective.clear();
        return out;
    }
  }
  if (!out.effective.empty()) {
    out.status = ApplyStatus::Applied;
    grid = std::move(trial);
  }
  return out;
}

}  // namespace sudoku
