// Semantics: what values mean and what shapes they have, read from schema data.
//
//   meaning   `%i2 = sub %i, 1`          -> `%i - 1`        (kind's `expr:`)
//             `%k = dim %x index=0`       -> first dim of %x's type
//   type      annotated (`: tensor<[%n, 128], f16>`), or inferred from the kind's
//             `shape:` rule; each `?` gets its own atom (`?<node>.<dim>`)
//   judgment  every equation a rule implies goes to the solver:
//             Yes = accepted, No = error, Unknown = warning naming the equation
//
// Soundness. Expressions are exact integers; machine integers wrap. So a value
// gets a meaning only when its arithmetic cannot wrap:
//   index (and untyped)   always — the stated assumption, as in MLIR's affine
//   iN / uN               only with `nsw=true` / `nuw=true` on the op
// Signedness is on the type (iN signed, uN unsigned, index signed), and every
// typed integer atom carries its type's range as an implicit fact. Float, bool,
// pointer and tensor values never get a meaning.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "schema/schema.h"
#include "symbolic/symbolic.h"
#include "verify/check.h"

namespace brrr::sema {

class Sema {
 public:
  Sema(const Module& m, const schema::Registry& reg);

  // The integer expression a value stands for; the value's own atom (`%7`) when
  // nothing more is known.
  symbolic::Expr meaning(ValueRef v);
  // Annotated type, else inferred; null when untyped.
  Type typeOf(ValueRef v);
  // True when typeOf(v) came from a shape rule rather than an annotation.
  bool isInferred(ValueRef v);

  // Definitional equalities (every value's meaning) + the facts visible in scope.
  const symbolic::Solver& solverFor(NodeId scope);

  // Rule mismatches (errors) and unproven equations (warnings), then facts:
  // block facts must hold on every incoming edge (branch conditions included),
  // loop-entry facts by induction, and facts about op results follow from what
  // precedes them. Module facts are assumptions; function facts on arguments are
  // preconditions.
  std::vector<verify::Issue> check();

  // Equations check() could not decide, for `insert-guards`.
  struct Unproven {
    NodeId node;
    symbolic::Expr a, b;
    std::string what;
    std::string rel = "eq";  // a == b, or "le": a <= b
  };
  const std::vector<Unproven>& unproven() const { return unproven_; }

  // Print an expression with value names and `dim(%x, k)` for fresh dims.
  std::string show(const symbolic::Expr& e);

 private:
  struct Pattern {
    bool whole = false;              // `S`
    std::string var;                 // whole-shape variable
    std::vector<std::string> dims;   // `[M, 128, ?]`
  };
  struct Rule {
    bool ok = false;
    std::vector<Pattern> ins;
    Pattern out;
    std::string out_elem;  // `-> [?] : index` names the result element type
  };
  struct Obligation {
    symbolic::Expr a, b;
    std::string what;
  };

  struct Edge {
    const Node* from;                     // the branching op (or the loop)
    std::vector<symbolic::Expr> passed;   // values for the block's arguments
    NodeId scope;                         // where the branch happens
    const Operand* cond = nullptr;        // taken when cond == polarity
    bool polarity = true;
    std::string label;
  };
  symbolic::Solver build(NodeId scope, size_t stop_fact);
  // The blocks that strictly dominate `block` in its function's CFG (the
  // function's own scope, the entry, excluded: it is on the lexical chain).
  const std::vector<NodeId>& dominatingBlocks(NodeId block);
  void addCondition(symbolic::Solver& s, const Operand& cond, bool polarity);
  symbolic::Expr operandExpr(const Operand& o);
  std::vector<Edge> edgesInto(const Node& block);
  void checkFacts(std::vector<verify::Issue>* out);
  void checkAccess(const Node& n, const schema::Kind& k, std::vector<verify::Issue>* out);
  const Rule& ruleFor(const schema::Kind& k);
  // Match a node against its kind's shape rule. Returns the inferred result type
  // (null if the rule cannot apply), and appends the equations it implies.
  Type applyRule(const Node& n, std::vector<Obligation>* obligations,
                 std::vector<verify::Issue>* issues);
  Type freshen(const Type& t, ValueRef owner) const;
  bool isIntegerType(const Type& t) const;
  bool intRange(const Type& t, __int128* lo, __int128* hi) const;
  bool fitsIn(const Type& from, const Type& to) const;  // every value of `from` fits `to`
  void judge(const Node& n, NodeId scope, const std::vector<Obligation>& obs,
             std::vector<verify::Issue>* out);
  void compareTypes(const Node& n, const Type& want, const Type& got,
                    const std::string& what, std::vector<Obligation>* obs,
                    std::vector<verify::Issue>* issues);

  const Module& m_;
  const schema::Registry& reg_;
  std::map<ValueRef, symbolic::Expr> meanings_;
  std::map<ValueRef, Type> types_;
  std::set<ValueRef> inferred_;
  std::set<ValueRef> in_progress_;          // typeOf
  std::set<ValueRef> in_progress_meaning_;  // meaning
  std::map<std::string, Rule> rules_;
  std::map<NodeId, symbolic::Solver> solvers_;
  std::map<NodeId, std::vector<NodeId>> dominators_;  // block -> strict dominator blocks
  std::vector<std::pair<symbolic::Expr, symbolic::Expr>> defs_;
  bool defs_ready_ = false;
  std::vector<verify::Issue> fact_issues_;
  std::vector<Unproven> unproven_;
};

// Everything a state must satisfy: consistent facts, the schema's structural
// rules, then (when those hold) shapes and meanings.
std::vector<verify::Issue> verifyModule(const Module& m, const schema::Registry& reg);

}  // namespace brrr::sema
