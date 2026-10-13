// BridgeIR core — v0 prototype.
//
// The defining property: STABLE NODE IDENTITY.
// Ids are allocated once, are never renumbered, and are preserved exactly by the
// canonical text printer and parser. Transformations allocate fresh ids for new nodes
// and never touch existing ones.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bridgeir {

using NodeId = uint64_t;
inline constexpr NodeId kInvalidId = 0;

// A reference to a node's result port (v0: data edges).
struct ValueRef {
  NodeId id = kInvalidId;
  uint32_t port = 0;
  bool operator==(const ValueRef&) const = default;
};

// Attr values are small typed data — the "everything else is data" principle.
struct AttrValue {
  enum class Kind { Int, Float, Str, Sym };
  Kind kind = Kind::Str;
  int64_t i = 0;
  double f = 0.0;
  std::string s;

  static AttrValue fromInt(int64_t v);
  static AttrValue fromFloat(double v);
  static AttrValue fromStr(std::string v);
  static AttrValue fromSym(std::string v);

  std::string toString() const;  // canonical textual form
};

struct Node {
  NodeId id = kInvalidId;
  std::string kind;
  std::vector<ValueRef> operands;          // data edges
  std::map<std::string, AttrValue> attrs;  // sorted => canonical printing
  NodeId scope = kInvalidId;               // enclosing scope (e.g. func) or invalid

  const AttrValue* findAttr(const std::string& key) const;
  std::string attrString(const std::string& key,
                         const std::string& fallback = "") const;
};

// Every mutation records what happened — the provenance journal (v0 sketch).
struct JournalEntry {
  std::string action;  // e.g. "insert", "retarget"
  NodeId id = kInvalidId;
  std::string detail;
};

class Module {
 public:
  std::vector<Node> nodes;        // program order
  std::vector<NodeId> top_level;  // functions and scope-less nodes, in order
  std::vector<JournalEntry> journal;

  // Stable-id allocator: strictly monotonic — never renumbers, never reuses.
  NodeId allocId();

  Node* find(NodeId id);
  const Node* find(NodeId id) const;
  bool contains(NodeId id) const { return find(id) != nullptr; }

  // Appends a node with a freshly allocated id.
  Node& appendNode(std::string kind, NodeId scope = kInvalidId);
  // Inserts right after `after` in program order, with a freshly allocated id.
  Node& insertNodeAfter(NodeId after, std::string kind, NodeId scope);
  // Adds a node carrying an explicitly parsed id (keeps the allocator above it).
  Node& appendParsedNode(Node node);
  // Children of a scope, in program order.
  std::vector<NodeId> childrenOf(NodeId scope) const;

  void log(std::string action, NodeId id, std::string detail);

 private:
  NodeId next_id_ = 1;
};

}  // namespace bridgeir
