// Data model for logical-solving deductions. This milestone provides ONLY the
// vocabulary, a safe way to apply deductions, and (sudoku/singles.hpp) the
// naked/hidden single techniques.
//
// Concepts kept apart:
//   original givens   : NOT tracked by Board; nothing here claims a filled cell
//                       was a given.
//   current Board     : placed values.
//   candidate state   : sudoku/candidates.hpp.
//   proposed deduction: Deduction. Plain, immutable description of what a
//                       technique concludes. Building one never touches a
//                       Board or a CandidateGrid.
//   applied deduction : AppliedDeduction, the outcome of apply() on a grid:
//                       what actually changed.
//
// Evidence only records what the producing technique actually examined; it is
// descriptive, not a proof.
//
// Errors: malformed deductions (bad cell/digit, empty or duplicated
// eliminations, a missing explanation) throw std::invalid_argument at
// construction. apply() reports problems through ApplyStatus.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sudoku/candidates.hpp"

namespace sudoku {

// Identifiers for the techniques planned in later milestones. Declaring a
// value does not implement it.
enum class Technique {
  NakedSingle,
  HiddenSingle,
  Pointing,  // locked candidates: box -> row/column
  Claiming,  // locked candidates: row/column -> box
  NakedPair,
  HiddenPair,
  NakedTriple,
  HiddenTriple,
  XWing,
};

std::string_view technique_name(Technique technique) noexcept;
// True only for techniques with a tested implementation (see singles.hpp).
constexpr bool is_implemented(Technique t) noexcept {
  return t == Technique::NakedSingle || t == Technique::HiddenSingle ||
         t == Technique::Pointing || t == Technique::Claiming ||
         t == Technique::NakedPair || t == Technique::HiddenPair ||
         t == Technique::NakedTriple || t == Technique::HiddenTriple ||
         t == Technique::XWing;
}

enum class DeductionAction {
  PlaceDigit,           // a digit belongs in a cell
  EliminateCandidates,  // these (cell, digit) pairs are not possible
};

struct Elimination {
  int cell;
  int digit;
  friend bool operator==(const Elimination&, const Elimination&) = default;
};

// What the technique looked at to justify its conclusion.
struct Evidence {
  std::optional<Unit> unit;  // the (source) unit involved, if any
  // For techniques that eliminate along a second region (Pointing, Claiming):
  // the unit that contains the eliminated candidates.
  std::optional<Unit> target_unit;
  // For patterns with two defining units of the same kind (XWing): the second
  // one; `unit` is the first. The pattern's orientation is unit->kind (Row =
  // row-oriented, Column = column-oriented). Unused by all other techniques.
  std::optional<Unit> second_unit;
  std::vector<int> cells;    // cells examined / forming the pattern
  DigitSet digits;           // digits involved in the pattern
  friend bool operator==(const Evidence&, const Evidence&) = default;
};

class Deduction {
 public:
  // "digit belongs in cell". Validates cell (out_of_range) and digit.
  static Deduction place(Technique technique, int cell, int digit,
                         Evidence evidence, std::string explanation);
  // Removes each listed candidate. Requires a non-empty list without repeated
  // pairs; validates every cell and digit.
  static Deduction eliminate(Technique technique,
                             std::vector<Elimination> eliminations,
                             Evidence evidence, std::string explanation);

  Technique technique() const noexcept { return technique_; }
  DeductionAction action() const noexcept { return action_; }
  // PlaceDigit: exactly one {cell, digit}. EliminateCandidates: the list.
  const std::vector<Elimination>& targets() const noexcept { return targets_; }
  const Evidence& evidence() const noexcept { return evidence_; }
  const std::string& explanation() const noexcept { return explanation_; }

  friend bool operator==(const Deduction&, const Deduction&) = default;

 private:
  Deduction() = default;
  Technique technique_ = Technique::NakedSingle;
  DeductionAction action_ = DeductionAction::PlaceDigit;
  std::vector<Elimination> targets_;
  Evidence evidence_;
  std::string explanation_;
};

enum class ApplyStatus {
  Applied,       // the grid changed (see AppliedDeduction::effective)
  NoChange,      // valid elimination whose candidates were all already absent
  Rejected,      // cannot be done in this state: touches a filled cell, digit
                 // is not a candidate (placement), or a technique that may not
                 // place; grid unchanged
  Unsupported,   // evidence the grid does not support (stale deduction, or one
                 // made from a different state); grid unchanged. Checked for
                 // placements and for Pointing/Claiming/pair/triple/XWing
                 // eliminations.
  WouldContradict, // refused because it would leave a cell with no
                 // candidates (an elimination of a last candidate, or a
                 // placement emptying a peer); grid unchanged
};

struct AppliedDeduction {
  Deduction deduction;                // the deduction that was attempted
  ApplyStatus status;
  // Candidates actually removed, ascending by cell then digit for placements
  // (the cell's own candidates, including the placed digit, then the digit in
  // its peers), in list order for eliminations. Empty unless Applied.
  std::vector<Elimination> effective;
};

// Detection results, shared by all detection functions (singles.hpp,
// locked.hpp).
//   Ok            : state is healthy; `deductions` may legitimately be empty.
//   Contradiction : the grid has a local contradiction; `contradictions` lists
//                   them and NO deductions are produced.
//   InvalidBoard  : only from Board overloads: the Board repeats a digit in a
//                   unit, so no candidate state exists; `conflicts` lists them.
enum class DetectStatus { Ok, Contradiction, InvalidBoard };

struct DetectResult {
  DetectStatus status = DetectStatus::Ok;
  std::vector<Deduction> deductions;
  std::vector<Contradiction> contradictions;  // status == Contradiction
  std::vector<Conflict> conflicts;            // status == InvalidBoard
};

// Applies a deduction to `grid` transactionally; any status other than
// Applied/NoChange leaves the grid unchanged.
//  * Eliminations: every listed candidate is processed or none is.
//  * Placements (NakedSingle / HiddenSingle only) are re-validated against the
//    grid first, so a stale or foreign deduction is refused: the cell must be
//    empty and the digit a candidate (else Rejected); then NakedSingle needs
//    the digit to be the cell's ONLY candidate, and HiddenSingle needs
//    evidence.unit to contain the cell, not already hold the digit, and have
//    the cell as the digit's only location (else Unsupported). The placement
//    itself is CandidateGrid::place().
//  * Pointing / Claiming eliminations are re-validated too: the digit (the
//    evidence's single digit) must not be placed in the source unit, its
//    current candidate cells in the source unit must be non-empty and all lie
//    in the target unit, every target must be that digit, lie in the target
//    unit and outside the source unit; otherwise Unsupported. Candidate cells
//    may have shrunk since detection (still supported); targets already
//    absent are skipped, so a repeat application is NoChange.
//  * NakedPair / HiddenPair eliminations are re-validated as well. Evidence
//    must give a source unit, exactly two distinct pair cells inside it, and
//    exactly two pair digits. NakedPair: both cells must still be empty with
//    candidate set exactly the pair digits (if either has shrunk, it is no
//    longer that pattern: Unsupported); every target must be a pair digit in
//    another cell of the unit. HiddenPair: neither digit is placed in the
//    unit, and each digit's current candidate cells in the unit are exactly
//    the two pair cells; every target must be a non-pair digit in a pair
//    cell. Targets already absent are skipped; repeating is NoChange.
//  * NakedTriple / HiddenTriple eliminations are re-validated with the same
//    evidence shape (source unit, N distinct cells inside it, N digits) for
//    N = 3. Unlike pairs, a triple stays valid when candidates have SHRUNK
//    since detection, because the logic only needs confinement: NakedTriple
//    needs three empty cells whose candidate sets are non-empty subsets of the
//    triple digits with union exactly the triple digits; HiddenTriple needs
//    the three digits unplaced in the unit with every current candidate cell
//    inside the three cells and their union exactly those cells. Targets must
//    be triple digits in other cells (naked) or other digits in the triple
//    cells (hidden). Otherwise Unsupported.
//  * XWing eliminations are re-validated: evidence.unit and second_unit are two
//    distinct rows or two distinct columns (never boxes) of the same kind,
//    evidence.digits holds one digit, and evidence.cells are the four corner
//    cells. The digit must be unplaced in both base units, have EXACTLY two
//    current candidate cells in each, and those two must sit in the same two
//    cross units (columns for rows, rows for columns); the corners must be
//    exactly those four cells (any order). Every target must be that digit, lie
//    in one of the two cross units and outside both base units. Anything else
//    (including a base unit that has since lost a candidate position) is
//    Unsupported. Targets already absent are skipped, as for the other
//    techniques.
//  * Elimination deductions are accepted ONLY for techniques that have an
//    evidence validator above (Pointing, Claiming, the pairs, the triples,
//    XWing). An elimination labelled NakedSingle or HiddenSingle (which only
//    place digits), or with a technique value outside the enum, is
//    Unsupported and changes nothing. There is no "apply as given" path.
//  * An elimination of a cell's last candidate is WouldContradict; one aimed
//    at a filled cell is Rejected.
// The caller's Board is never touched; the grid's own snapshot is updated.
AppliedDeduction apply(CandidateGrid& grid, const Deduction& deduction);

}  // namespace sudoku
