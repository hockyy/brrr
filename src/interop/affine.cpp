#include "interop/affine.h"

#include <map>

#include "symbolic/expr_io.h"

namespace brrr::interop {
namespace {

using symbolic::Expr;
using symbolic::ExprTerm;

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool affine(const Expr& e, std::string* why,
            const std::function<std::string(const std::string&)>& name) {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
    case ExprTerm::Kind::Var:
      return true;
    case ExprTerm::Kind::Add:
      for (const Expr& a : e->args) {
        if (!affine(a, why, name)) return false;
      }
      return true;
    case ExprTerm::Kind::Mul: {
      const Expr* variable = nullptr;
      for (const Expr& a : e->args) {
        if (a->kind == ExprTerm::Kind::Const) continue;
        if (variable) {
          *why = "not affine: " + symbolic::printExpr(e, name) +
                 " multiplies two non-constant terms (affine maps are linear)";
          return false;
        }
        variable = &a;
      }
      return !variable || affine(*variable, why, name);
    }
    default:  // floordiv / ceildiv / mod by a constant: affine when the left side is
      return affine(e->args[0], why, name);
  }
}

void atomsInOrder(const Expr& e, std::vector<std::string>* out) {
  if (e->kind == ExprTerm::Kind::Var) {
    for (const std::string& a : *out) {
      if (a == e->name) return;
    }
    out->push_back(e->name);
    return;
  }
  for (const Expr& a : e->args) atomsInOrder(a, out);
}

// Rename whole atoms (a dim(%x, k) atom included) — not just the value inside.
Expr renameWhole(const Expr& e, const std::map<std::string, std::string>& to) {
  if (e->kind == ExprTerm::Kind::Const) return e;
  if (e->kind == ExprTerm::Kind::Var) {
    auto it = to.find(e->name);
    return it == to.end() ? e : ExprTerm::var(it->second);
  }
  std::vector<Expr> args;
  for (const Expr& a : e->args) args.push_back(renameWhole(a, to));
  return ExprTerm::rebuild(e, std::move(args));
}

struct Parsed {
  std::vector<std::string> dims, symbols, results;
};

// `(d0, d1)[s0] -> (r0, r1)`, optionally wrapped in affine_map<...>.
bool parseMap(const std::string& text, Parsed* out, std::string* err) {
  std::string m = trim(text);
  if (m.rfind("affine_map<", 0) == 0) {
    if (m.back() != '>') {
      *err = "affine_map<...> is not closed";
      return false;
    }
    m = trim(m.substr(11, m.size() - 12));
  }
  auto group = [&](size_t* pos, char open, char close, std::vector<std::string>* items) {
    if (*pos >= m.size() || m[*pos] != open) return false;
    int depth = 0;
    size_t i = *pos;
    for (; i < m.size(); ++i) {
      if (m[i] == open) ++depth;
      if (m[i] == close && --depth == 0) break;
    }
    if (i >= m.size()) return false;
    const std::string inner = trim(m.substr(*pos + 1, i - *pos - 1));
    if (!inner.empty()) *items = symbolic::splitTopLevel(inner, ',');
    *pos = i + 1;
    while (*pos < m.size() && m[*pos] == ' ') ++*pos;
    return true;
  };
  size_t pos = 0;
  if (!group(&pos, '(', ')', &out->dims)) {
    *err = "expected the dims list `(d0, ...)`";
    return false;
  }
  if (pos < m.size() && m[pos] == '[' && !group(&pos, '[', ']', &out->symbols)) {
    *err = "the symbols list `[s0, ...]` is not closed";
    return false;
  }
  if (m.compare(pos, 2, "->") != 0) {
    *err = "expected `->` after the dims and symbols";
    return false;
  }
  pos += 2;
  while (pos < m.size() && m[pos] == ' ') ++pos;
  if (!group(&pos, '(', ')', &out->results) || pos != m.size()) {
    *err = "expected the results `(expr, ...)` at the end";
    return false;
  }
  return true;
}

}  // namespace

bool toAffineMap(const std::vector<Expr>& results,
                 const std::function<bool(const std::string&)>& isSymbol, AffineMap* out,
                 std::string* err,
                 const std::function<std::string(const std::string&)>& name) {
  std::vector<std::string> atoms;
  for (const Expr& r : results) {
    if (!affine(r, err, name)) return false;
    atomsInOrder(r, &atoms);
  }
  std::map<std::string, std::string> to;
  for (const std::string& a : atoms) {
    if (isSymbol(a)) {
      to[a] = "s" + std::to_string(out->symbols.size());
      out->symbols.push_back(a);
    } else {
      to[a] = "d" + std::to_string(out->dims.size());
      out->dims.push_back(a);
    }
  }
  std::string t = "affine_map<(";
  for (size_t i = 0; i < out->dims.size(); ++i) t += (i ? ", d" : "d") + std::to_string(i);
  t += ")";
  if (!out->symbols.empty()) {
    t += "[";
    for (size_t i = 0; i < out->symbols.size(); ++i) t += (i ? ", s" : "s") + std::to_string(i);
    t += "]";
  }
  t += " -> (";
  for (size_t i = 0; i < results.size(); ++i) {
    t += (i ? ", " : "") + symbolic::printExpr(renameWhole(results[i], to));
  }
  out->text = t + ")>";
  return true;
}

bool affineArity(const std::string& map, size_t* dims, size_t* symbols, std::string* err) {
  Parsed p;
  if (!parseMap(map, &p, err)) return false;
  *dims = p.dims.size();
  *symbols = p.symbols.size();
  return true;
}

bool fromAffineMap(const std::string& map, const std::vector<std::string>& dims,
                   const std::vector<std::string>& symbols, std::vector<Expr>* out,
                   std::string* err) {
  Parsed p;
  if (!parseMap(map, &p, err)) return false;
  if (p.dims.size() != dims.size() || p.symbols.size() != symbols.size()) {
    *err = "the map takes " + std::to_string(p.dims.size()) + " dim(s) and " +
           std::to_string(p.symbols.size()) + " symbol(s); got " + std::to_string(dims.size()) +
           " and " + std::to_string(symbols.size());
    return false;
  }
  std::map<std::string, std::string> to;
  for (size_t i = 0; i < dims.size(); ++i) to[trim(p.dims[i])] = dims[i];
  for (size_t i = 0; i < symbols.size(); ++i) to[trim(p.symbols[i])] = symbols[i];
  for (const std::string& r : p.results) {
    Expr e = symbolic::parseExpr(r, err);
    if (!e) {
      *err = "in `" + r + "`: " + *err;
      return false;
    }
    std::string unknown;
    e = symbolic::mapVars(e, [&](const std::string& a) {
      auto it = to.find(a);
      if (it == to.end()) {
        unknown = a;
        return a;
      }
      return it->second;
    });
    if (!unknown.empty()) {
      *err = "`" + unknown + "` is not a dim or symbol of the map";
      return false;
    }
    out->push_back(e);
  }
  return true;
}

}  // namespace brrr::interop
