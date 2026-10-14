// Building code inside one function: ops appended to a block in program order,
// new blocks after the function's last node, facts on any scope, names and
// labels kept unique. Every edit goes through Module's insertion, so stable ids
// and the run store's journal see it.
//
//   Builder b(m, func);
//   b.at(func);                                    // the entry: the function's own scope
//   ValueRef t = b.op("alloc", {}, bufType, "tmp");
//   b.countedLoop("i", bound, {}, [&](ValueRef i, const std::vector<ValueRef>&) {
//     ... b.op("load", ...) ...
//     return std::vector<ValueRef>{};
//   });
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir/ir.h"

namespace brrr {

class Builder {
 public:
  Builder(Module& m, NodeId func);

  NodeId func() const { return func_; }
  // Where ops go: the function itself (its entry) or one of its blocks.
  void at(NodeId scope) { at_ = scope; }
  NodeId where() const { return at_; }

  // An op with one result (`result` null: the kind's schema decides the type).
  ValueRef op(const std::string& kind, std::vector<Operand> operands, Type result,
              const std::string& name = "", std::map<std::string, AttrValue> attrs = {});
  // An op with no result (store, dealloc, guard, return).
  NodeId effect(const std::string& kind, std::vector<Operand> operands,
                std::map<std::string, AttrValue> attrs = {});
  // A branch: `br` (one successor) or `cond_br` (condition, two successors).
  NodeId branch(std::vector<Operand> operands, std::vector<Successor> successors);

  // A new block at the end of the function, with typed, named arguments.
  NodeId block(const std::string& label, const std::vector<std::pair<std::string, Type>>& args,
               std::vector<ValueRef>* values);
  // A fact (canonical text: value atoms by id) holding in `scope`.
  void fact(NodeId scope, const std::string& text);

  // for (iv = 0; iv < bound; ++iv) { body }, as blocks: a zero-trip check, a
  // header whose fact `0 <= iv <= bound - 1` is proven on both edges into it,
  // and an exit block. `carried` values flow through the loop (block arguments);
  // the body returns their next values; the result is their final values, and
  // the insertion point is the exit block.
  std::vector<ValueRef> countedLoop(
      const std::string& name, const symbolic::Expr& bound,
      const std::vector<std::pair<std::string, ValueRef>>& carried,
      const std::function<std::vector<ValueRef>(ValueRef iv, const std::vector<ValueRef>&)>& body);

  std::string uniqueName(const std::string& base);
  std::string uniqueLabel(const std::string& base);

  static Type scalar(const std::string& name);

 private:
  Node& insert(Node n);
  NodeId lastOf(NodeId scope) const;
  NodeId lastInFunction() const;

  Module& m_;
  NodeId func_;
  NodeId at_;
  std::set<std::string> names_, labels_;
};

}  // namespace brrr
