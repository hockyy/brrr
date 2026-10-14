// The epilogue chain of an aten.linear: the elementwise ops that fuse-epilogue
// folds into its store, and that lower-aten applies there. Shared, so the pass
// that schedules a fusion and the lowering that realizes it check the same
// conditions: an attribute that no longer holds (say, after a hand edit) fails
// the lowering instead of miscompiling.
//
//   %linear = aten.linear %x, %w, %b epilogue="relu,add"
//   %relu = aten.relu %linear          <- the sole reader of %linear
//   %add = aten.add %relu, %y          <- the sole reader of %relu; %y exists
//                                         before %linear runs
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ir/ir.h"

namespace brrr::passes {

struct Use {
  NodeId user = kInvalidId;
  size_t operand = 0;  // SIZE_MAX: read some other way (an expression atom, a branch)
};

// The ops of one function, in order, and who reads each value. Only
// straight-line functions are analyzed; with blocks, nothing has a sole user.
struct Dataflow {
  Dataflow(const Module& m, NodeId func);

  NodeId func = kInvalidId;
  bool straight = true;
  std::vector<NodeId> order;
  std::map<NodeId, size_t> position;
  std::map<ValueRef, std::vector<Use>> uses;

  // The one op reading v (as a value operand), or kInvalidId.
  NodeId soleUser(ValueRef v, size_t* operand = nullptr) const;
  // v exists before `at` runs: a function argument or an earlier op's result.
  bool availableBefore(const Module& m, ValueRef v, NodeId at) const;
};

// Can `user`, reading v as operand `operand`, fold into the store of `linear`?
// Returns "relu", "add" or "mul", or "" with *why.
std::string epilogueStep(const Module& m, const Dataflow& df, NodeId linear, NodeId user, size_t operand,
                         std::string* why);

struct Chain {
  std::vector<NodeId> ops;  // the folded ops, in order
  ValueRef final;           // what the chain produces (the linear's own value if empty)
};

// Follow the linear's `epilogue` attribute. False, with *err, when a step no
// longer holds.
bool epilogueChain(const Module& m, const Dataflow& df, NodeId linear, Chain* out, std::string* err);

std::vector<std::string> splitList(const std::string& s);  // "relu,add" -> {relu, add}

}  // namespace brrr::passes
