// Brrr core — the v1 IR model. Surface syntax: docs/scratch/syntax-v1.md.
//
// The defining property: STABLE NODE IDENTITY. Ids are allocated once, never
// renumbered and never reused; transformations allocate fresh ids for new nodes and
// leave existing ones alone. Names (`%acc`, `^body`, `@f`) are the readable layer
// on top and can change freely.
//
// One record, `Node`, covers every definition:
//   module | func | block | arg | <op kind>
// A scope (module, func, block, region op) owns the nodes whose `scope` is its id.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "symbolic/symbolic.h"

namespace brrr {

using NodeId = uint64_t;
inline constexpr NodeId kInvalidId = 0;

// `nID` id tokens (stored form).
bool parseNodeIdToken(const std::string& tok, NodeId* out);
// Value and label names: [A-Za-z_][A-Za-z0-9_.]*, not all digits.
bool isValidName(const std::string& name);

// One result of one node.
struct ValueRef {
  NodeId id = kInvalidId;
  uint32_t port = 0;
  bool operator==(const ValueRef&) const = default;
  bool operator<(const ValueRef& o) const {
    return id != o.id ? id < o.id : port < o.port;
  }
};

// Values inside expressions are atoms named `%<id>` (port 0) or `%<id>#<port>`.
std::string valueAtom(const ValueRef& v);
bool parseValueAtom(const std::string& atom, ValueRef* out);
// `dim(%7, k)` is the atom `?%7.k`: the k-th dimension of a value.
std::string dimAtom(const ValueRef& v, size_t k);
// The value an atom depends on: `%7` or `?%7.k` -> %7.
bool atomValue(const std::string& atom, ValueRef* out);

// A scalar (`i64`, `index`, `f16`) or a shaped type (`tensor<[%n, 128], f16>`).
// Dims are expressions; `?` is the atom "?" until a node gives it identity.
struct TypeTerm {
  std::string name;
  bool shaped = false;
  std::vector<symbolic::Expr> dims;
  std::string elem;
};
using Type = std::shared_ptr<const TypeTerm>;

// `rename` maps atom names to their printed spelling (e.g. `%7` -> `%n`).
using AtomNamer = std::function<std::string(const std::string&)>;
std::string printType(const Type& t, const AtomNamer& rename = {});
bool typesEqualText(const Type& a, const Type& b);

// Attributes are literal data that configure an op (never a value's type).
struct AttrValue {
  enum class Kind { Int, Float, Bool, Str, Ident, Type, Array };
  Kind kind = Kind::Int;
  int64_t i = 0;
  double f = 0.0;
  bool b = false;
  std::string s;                // Str, Ident
  brrr::Type type;          // Type
  std::vector<AttrValue> items; // Array
  brrr::Type ascribed;      // `(literal : type)` when written explicitly

  static AttrValue fromInt(int64_t v);
  static AttrValue fromFloat(double v);
  static AttrValue fromBool(bool v);
  static AttrValue fromStr(std::string v);
  static AttrValue fromIdent(std::string v);

  std::string toString(const AtomNamer& rename = {}) const;  // canonical text
};

// What an op consumes: a value, an integer expression over values, or a list of
// expressions. Expressions replace MLIR's Attribute-or-Value (`OpFoldResult`).
struct Operand {
  enum class Kind { Value, Expr, List };
  Kind kind = Kind::Value;
  ValueRef value;                     // Value
  std::vector<symbolic::Expr> exprs;  // Expr (exactly one) / List

  static Operand ofValue(ValueRef v);
  static Operand ofExpr(symbolic::Expr e);
  static Operand ofList(std::vector<symbolic::Expr> es);
  // Every value this operand reads (expression atoms included).
  void uses(std::vector<ValueRef>* out) const;
};

// A branch target and the values for its block's arguments.
struct Successor {
  NodeId block = kInvalidId;
  std::vector<ValueRef> args;
};

struct Node {
  NodeId id = kInvalidId;
  std::string kind;                    // module | func | block | arg | op kind
  std::string symbol;                  // func/module `@symbol`, block `^label`
  std::vector<std::string> names;      // one per result (arg: one); "" = unnamed
  std::vector<Type> types;             // per result (arg: one; func: returns)
  std::vector<Operand> operands;
  std::vector<Successor> successors;
  std::map<std::string, AttrValue> attrs;  // sorted => canonical printing
  NodeId scope = kInvalidId;           // enclosing scope, or root
  bool is_scope = false;               // module, func, block, or an op with a region

  size_t numResults() const { return names.size(); }
  Type typeOf(uint32_t port) const {
    return port < types.size() ? types[port] : nullptr;
  }
  const AttrValue* findAttr(const std::string& key) const;
  // Every value this node reads: operands, expression atoms, successor args.
  std::vector<ValueRef> uses() const;
};

struct JournalEntry {
  std::string action;  // e.g. "insert", "replace", "erase", "rename", "set-type"
  NodeId id = kInvalidId;
  std::string detail;
};

class Module {
 public:
  // Facts hold in the scope they are written in and every scope inside it. `text`
  // is canonical: value atoms are spelled by id (`0 <= %7`).
  struct FactEntry {
    NodeId scope = kInvalidId;
    std::string text;
  };

  std::vector<Node> nodes;        // program order
  std::vector<NodeId> top_level;  // root-level definitions, in order
  std::vector<JournalEntry> journal;
  std::vector<FactEntry> facts;

  NodeId allocId();
  void reserveId(NodeId id);  // keep the allocator above an id seen in text
  NodeId nextId() const { return next_id_; }

  Node* find(NodeId id);
  const Node* find(NodeId id) const;
  bool contains(NodeId id) const { return find(id) != nullptr; }

  // Appends a node; a zero id gets a fresh one. Returns a reference that stays
  // valid until the next insertion.
  Node& append(Node n);
  // Inserts right after / before an existing node in program order (fresh id
  // when `n.id` is zero).
  Node& insertAfter(NodeId after, Node n);
  Node& insertBefore(NodeId before, Node n);
  // Children of a scope, in program order.
  std::vector<NodeId> childrenOf(NodeId scope) const;

  // Journaled edits — passes use these so provenance is never optional.
  void replaceAllUses(ValueRef from, ValueRef to);
  void erase(NodeId id);  // removes the node, its scope contents, and its facts
  void setType(ValueRef v, Type t);
  bool rename(ValueRef v, const std::string& new_name, std::string* err);
  size_t useCount(ValueRef v) const;

  void log(std::string action, NodeId id, std::string detail);

  // The nearest enclosing func or module (names are unique within one).
  NodeId namingScope(NodeId scope) const;
  // Resolve `%name` as seen from `scope`: its naming scope, then outward.
  bool resolveName(NodeId scope, const std::string& name, ValueRef* out) const;

 private:
  void reindex();
  NodeId next_id_ = 1;
  std::unordered_map<NodeId, size_t> index_;
};

}  // namespace brrr
