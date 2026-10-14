#include "ir/builder.h"

#include "symbolic/expr_io.h"

namespace brrr {

using symbolic::Expr;
using symbolic::ExprTerm;

Builder::Builder(Module& m, NodeId func) : m_(m), func_(func), at_(func) {
  for (const Node& n : m_.nodes) {
    if (m_.namingScope(n.scope) != func_ && n.id != func_) continue;
    for (const std::string& name : n.names) {
      if (!name.empty()) names_.insert(name);
    }
    if (n.kind == "block") labels_.insert(n.symbol);
  }
}

Type Builder::scalar(const std::string& name) {
  return std::make_shared<TypeTerm>(TypeTerm{name, false, {}, ""});
}

std::string Builder::uniqueName(const std::string& base) {
  std::string name = base;
  for (int k = 1; names_.count(name); ++k) name = base + "_" + std::to_string(k);
  names_.insert(name);
  return name;
}

std::string Builder::uniqueLabel(const std::string& base) {
  std::string label = base;
  for (int k = 1; labels_.count(label); ++k) label = base + "_" + std::to_string(k);
  labels_.insert(label);
  return label;
}

// The last node of `scope` in program order: ops go after it. The entry's ops
// must stay before the function's first block.
NodeId Builder::lastOf(NodeId scope) const {
  NodeId last = scope;
  for (const Node& n : m_.nodes) {
    if (scope == func_ && n.kind == "block" && n.scope == func_) break;
    if (n.scope == scope) last = n.id;
  }
  return last;
}

NodeId Builder::lastInFunction() const {
  NodeId last = func_;
  for (const Node& n : m_.nodes) {
    if (n.id == func_ || n.scope == func_) {
      last = n.id;
      continue;
    }
    const Node* s = m_.find(n.scope);
    if (s && s->kind == "block" && s->scope == func_) last = n.id;
  }
  return last;
}

Node& Builder::insert(Node n) {
  n.scope = at_;
  const NodeId after = lastOf(at_);
  if (at_ == func_) {
    // Before the first block, after the entry's last op.
    for (const Node& x : m_.nodes) {
      if (x.kind == "block" && x.scope == func_) return m_.insertBefore(x.id, std::move(n));
    }
  }
  return m_.insertAfter(after, std::move(n));
}

ValueRef Builder::op(const std::string& kind, std::vector<Operand> operands, Type result,
                     const std::string& name, std::map<std::string, AttrValue> attrs) {
  Node n;
  n.kind = kind;
  n.operands = std::move(operands);
  n.attrs = std::move(attrs);
  n.names = {name.empty() ? "" : uniqueName(name)};
  n.types = {result};
  const NodeId id = insert(std::move(n)).id;
  m_.log("insert", id, kind);
  return {id, 0};
}

NodeId Builder::effect(const std::string& kind, std::vector<Operand> operands,
                       std::map<std::string, AttrValue> attrs) {
  Node n;
  n.kind = kind;
  n.operands = std::move(operands);
  n.attrs = std::move(attrs);
  const NodeId id = insert(std::move(n)).id;
  m_.log("insert", id, kind);
  return id;
}

NodeId Builder::branch(std::vector<Operand> operands, std::vector<Successor> successors) {
  Node n;
  n.kind = successors.size() == 1 ? "br" : "cond_br";
  n.operands = std::move(operands);
  n.successors = std::move(successors);
  const NodeId id = insert(std::move(n)).id;
  m_.log("insert", id, n.kind);
  return id;
}

NodeId Builder::block(const std::string& label, const std::vector<std::pair<std::string, Type>>& args,
                      std::vector<ValueRef>* values) {
  Node b;
  b.kind = "block";
  b.symbol = uniqueLabel(label);
  b.scope = func_;
  b.is_scope = true;
  NodeId prev = m_.insertAfter(lastInFunction(), std::move(b)).id;
  const NodeId block = prev;
  m_.log("insert", block, "block ^" + m_.find(block)->symbol);
  values->clear();
  for (const auto& [name, type] : args) {
    Node a;
    a.kind = "arg";
    a.scope = block;
    a.names = {uniqueName(name)};
    a.types = {type};
    prev = m_.insertAfter(prev, std::move(a)).id;
    values->push_back({prev, 0});
  }
  return block;
}

void Builder::fact(NodeId scope, const std::string& text) {
  m_.facts.push_back({scope, text});
  m_.log("fact", scope, text);
}

std::vector<ValueRef> Builder::countedLoop(
    const std::string& name, const Expr& bound,
    const std::vector<std::pair<std::string, ValueRef>>& carried,
    const std::function<std::vector<ValueRef>(ValueRef, const std::vector<ValueRef>&)>& body) {
  const Type index = scalar("index");
  const NodeId start = at_;
  // The zero-trip check: the header's fact needs bound >= 1 on the way in.
  const ValueRef enter = op("icmp", {Operand::ofExpr(ExprTerm::constant(0)), Operand::ofExpr(bound)},
                            nullptr, name + "_enter",
                            {{"pred", AttrValue::fromIdent("lt")}});

  std::vector<std::pair<std::string, Type>> header_args{{name, index}};
  std::vector<std::pair<std::string, Type>> exit_args;
  for (const auto& [cname, v] : carried) {
    const Node* def = m_.find(v.id);
    const Type t = def && v.port < def->types.size() ? def->types[v.port] : nullptr;
    header_args.push_back({cname, t});
    exit_args.push_back({cname + "_out", t});
  }
  std::vector<ValueRef> hv;
  const NodeId header = block(name + "_loop", header_args, &hv);
  const ValueRef iv = hv[0];
  const std::vector<ValueRef> vals(hv.begin() + 1, hv.end());
  const Expr last = bound->kind == ExprTerm::Kind::Const
                       ? ExprTerm::constant(bound->c - 1)
                       : ExprTerm::add({bound, ExprTerm::constant(-1)});
  fact(header, "0 <= " + valueAtom(iv) + " <= " + symbolic::printExpr(last));

  at(header);
  const std::vector<ValueRef> next = body(iv, vals);
  // The latch, wherever the body left off.
  const ValueRef iv2 = op("add", {Operand::ofValue(iv), Operand::ofExpr(ExprTerm::constant(1))},
                          index, name + "_next");
  const ValueRef more = op("icmp", {Operand::ofValue(iv2), Operand::ofExpr(bound)}, nullptr,
                           name + "_more", {{"pred", AttrValue::fromIdent("lt")}});
  const NodeId latch = at_;
  std::vector<ValueRef> exitv;
  const NodeId exit = block(name + "_done", exit_args, &exitv);
  Successor back{header, {iv2}}, out{exit, {}};
  back.args.insert(back.args.end(), next.begin(), next.end());
  out.args = next;
  at(latch);
  branch({Operand::ofValue(more)}, {back, out});

  // Into the loop from where we started (the initial values), or straight to the exit.
  at(start);
  const ValueRef zero = op("const", {}, index, name + "_zero", {{"value", AttrValue::fromInt(0)}});
  Successor in{header, {zero}}, skip{exit, {}};
  for (const auto& [cname, v] : carried) {
    in.args.push_back(v);
    skip.args.push_back(v);
  }
  branch({Operand::ofValue(enter)}, {in, skip});
  at(exit);
  return exitv;
}

}  // namespace brrr
