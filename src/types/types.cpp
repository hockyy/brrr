#include "types/types.h"

namespace bridgeir::types {
namespace {

std::string varName(uint32_t id) {
  std::string s(1, static_cast<char>('A' + (id % 26)));
  if (id >= 26) s += std::to_string(id / 26);
  return s;
}

std::string atCtx(const std::string& ctx) {
  return ctx.empty() ? std::string() : "at " + ctx + ": ";
}

}  // namespace

TypeRef TypeTerm::makeVar(uint32_t id) {
  auto t = std::make_shared<TypeTerm>();
  t->kind = Kind::Var;
  t->var = id;
  return t;
}

TypeRef TypeTerm::makeName(std::string name, std::vector<TypeRef> args) {
  auto t = std::make_shared<TypeTerm>();
  t->kind = Kind::Name;
  t->name = std::move(name);
  t->args = std::move(args);
  return t;
}

bool structurallyEqual(const TypeRef& a, const TypeRef& b) {
  if (a->kind != b->kind) return false;
  if (a->kind == TypeTerm::Kind::Var) return a->var == b->var;
  if (a->name != b->name || a->args.size() != b->args.size()) return false;
  for (size_t i = 0; i < a->args.size(); ++i) {
    if (!structurallyEqual(a->args[i], b->args[i])) return false;
  }
  return true;
}

TypeRef Unifier::fresh() { return TypeTerm::makeVar(next_var_++); }

TypeRef Unifier::resolveShallow(const TypeRef& t) const {
  TypeRef cur = t;
  while (cur->kind == TypeTerm::Kind::Var && cur->var < bindings_.size() &&
         bindings_[cur->var]) {
    cur = bindings_[cur->var];
  }
  return cur;
}

TypeRef Unifier::resolve(const TypeRef& t) const {
  const TypeRef r = resolveShallow(t);
  if (r->kind == TypeTerm::Kind::Var) return r;
  std::vector<TypeRef> args;
  args.reserve(r->args.size());
  for (const auto& a : r->args) args.push_back(resolve(a));
  return TypeTerm::makeName(r->name, std::move(args));
}

std::string Unifier::pretty(const TypeRef& t) const {
  const TypeRef r = resolve(t);
  if (r->kind == TypeTerm::Kind::Var) return varName(r->var);
  std::string out = r->name;
  if (!r->args.empty()) {
    out += "<";
    for (size_t i = 0; i < r->args.size(); ++i) {
      if (i) out += ", ";
      out += pretty(r->args[i]);
    }
    out += ">";
  }
  return out;
}

bool Unifier::occurs(uint32_t var, const TypeRef& t) const {
  const TypeRef r = resolveShallow(t);
  if (r->kind == TypeTerm::Kind::Var) return r->var == var;
  for (const auto& a : r->args) {
    if (occurs(var, a)) return true;
  }
  return false;
}

void Unifier::bind(uint32_t var, const TypeRef& t) {
  if (var >= bindings_.size()) bindings_.resize(var + 1);
  bindings_[var] = t;
}

bool Unifier::unify(const TypeRef& a, const TypeRef& b, std::string* error) {
  return unifyImpl(a, b, "", error);
}

bool Unifier::unifyImpl(const TypeRef& a0, const TypeRef& b0,
                        const std::string& ctx, std::string* error) {
  const TypeRef a = resolveShallow(a0);
  const TypeRef b = resolveShallow(b0);

  if (a->kind == TypeTerm::Kind::Var && b->kind == TypeTerm::Kind::Var &&
      a->var == b->var) {
    return true;
  }
  if (a->kind == TypeTerm::Kind::Var) {
    if (occurs(a->var, b)) {
      *error = atCtx(ctx) + "occurs check: " + pretty(a) + " occurs in " +
               pretty(b);
      return false;
    }
    bind(a->var, b);
    return true;
  }
  if (b->kind == TypeTerm::Kind::Var) {
    if (occurs(b->var, a)) {
      *error = atCtx(ctx) + "occurs check: " + pretty(b) + " occurs in " +
               pretty(a);
      return false;
    }
    bind(b->var, a);
    return true;
  }
  if (a->name != b->name || a->args.size() != b->args.size()) {
    *error = atCtx(ctx) + "cannot unify " + pretty(a) + " with " + pretty(b);
    return false;
  }
  for (size_t i = 0; i < a->args.size(); ++i) {
    const std::string c = (ctx.empty() ? std::string() : ctx + ".") + a->name +
                          "[" + std::to_string(i) + "]";
    if (!unifyImpl(a->args[i], b->args[i], c, error)) return false;
  }
  return true;
}

}  // namespace bridgeir::types
