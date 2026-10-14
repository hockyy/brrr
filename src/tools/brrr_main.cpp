#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "interop/affine.h"
#include "io/json.h"
#include "ir/ir.h"
#include "ir/text.h"
#include "pipeline/runs.h"
#include "schema/schema.h"
#include "sema/sema.h"
#include "symbolic/expr_io.h"
#include "symbolic/facts.h"
#include "symbolic/z3_backend.h"
#include "verify/check.h"

namespace {

std::vector<std::string> g_include_dirs;

std::string readFile(const std::string& path, bool* ok, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *ok = false;
    *err = "cannot open " + path;
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *ok = true;
  return ss.str();
}

std::string dirOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool fileExists(const std::string& p) { return std::ifstream(p).good(); }

// "name" searches relative-first; <name> searches include dirs first. `.br` is
// appended when the bare name does not exist.
bool resolveInclude(const std::string& from_dir, bool angled, const std::string& target,
                    std::string* resolved) {
  auto tryPath = [&](const std::string& base) {
    for (const std::string& p : {base, base + ".br"}) {
      if (fileExists(p)) {
        *resolved = p;
        return true;
      }
    }
    return false;
  };
  std::vector<std::string> order;
  if (!angled) order.push_back(from_dir);
  for (const std::string& d : g_include_dirs) order.push_back(d);
  if (angled) order.push_back(from_dir);
  for (const std::string& d : order) {
    if (tryPath(d + "/" + target)) return true;
  }
  return false;
}

// C-style textual inclusion. Cycle-checked, depth-limited.
bool expandIncludes(const std::string& path, std::vector<std::string>* stack, std::string* out,
                    std::string* err) {
  if (stack->size() > 32) {
    *err = "include depth exceeded at " + path;
    return false;
  }
  for (const std::string& s : *stack) {
    if (s == path) {
      *err = "include cycle: " + path;
      return false;
    }
  }
  stack->push_back(path);
  bool ok = false;
  std::string rerr;
  const std::string text = readFile(path, &ok, &rerr);
  if (!ok) {
    *err = rerr;
    return false;
  }
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t b = line.find_first_not_of(" \t");
    if (b != std::string::npos && line.compare(b, 8, "#include") == 0) {
      std::string rest = line.substr(b + 8);
      const size_t rb = rest.find_first_not_of(" \t");
      rest = rb == std::string::npos ? "" : rest.substr(rb);
      const bool angled = !rest.empty() && rest[0] == '<';
      const size_t e = rest.find(angled ? '>' : '"', 1);
      if (rest.empty() || e == std::string::npos) {
        *err = path + ": malformed #include";
        return false;
      }
      const std::string target = rest.substr(1, e - 1);
      std::string resolved;
      if (!resolveInclude(dirOf(path), angled, target, &resolved)) {
        *err = path + ": cannot resolve #include \"" + target + "\"";
        return false;
      }
      *out += "# from " + resolved + "\n";
      if (!expandIncludes(resolved, stack, out, err)) return false;
      continue;
    }
    *out += line + "\n";
  }
  stack->pop_back();
  return true;
}

int usage() {
  std::cerr <<
      R"USAGE(usage: brrr <command> [args] [flags]

  inspect a file
    print <file> [--ids]        canonical text (clean, or stored form with ids)
    check <file> [--strict]     verify; --strict turns unproven equations into errors
    types <file>                every value's type and integer meaning
    ask <file> <scope> "<q>"    a solver query in scope (root, @func, ^block, nID):
                                "%a == %b", "%i <= %n", "aligned(%n, 16)"
    facts <file>                the facts, by scope
    to-affine <file> <scope> "<e1>, <e2>"   MLIR export: each expression as an
                                OpFoldResult (static | value | affine.apply)
    from-affine "<affine_map>" <dims...> <symbols...>   MLIR import
    rename <file> <%value> <new>
    to-json <file> | roundtrip <file>
    schema [--core-only]        the loaded vocabulary

  compile (runs are recorded in .brrr/, see `--store`)
    passes                      the available passes
    run <file> --pipeline=<f> | --passes=<a,b>   [--steps=N] [--pause-on-warning]
    resume <run> [--pipeline=..|--passes=..] [--state=<edited.br>] [--steps=N]
    branch <run>@<k> [--pipeline=..|--passes=..] [--state=<edited.br>]
    runs                        list runs
    show <run>[@k] [--ids] [--record]

  flags: --schemas=<file> --include=<dir> --core-only --store=<dir>
         --solver-rlimit=N   Z3's per-check resource limit (0 = none; env BRRR_SOLVER_RLIMIT)
  env:   BRRR_SOLVER=z3|local|check   BRRR_SOLVER_STATS=1 (solver counters on exit)
)USAGE";
  return 64;
}

void printIssues(const std::vector<brrr::verify::Issue>& issues, bool warnings) {
  for (const auto& is : issues) {
    if (!is.error && !warnings) continue;
    std::cout << (is.error ? "error: " : "warning: ") << "n" << is.node << " (" << is.kind
              << "): " << is.message << "\n";
  }
}

void printSolverStats() {
  const brrr::symbolic::z3::Stats& st = brrr::symbolic::z3::stats();
  std::cerr << "solver stats: " << st.checks << " z3 check(s), max rlimit used "
            << st.max_rlimit_used << " (limit " << brrr::symbolic::z3::rlimit() << "), "
            << st.resource_unknowns << " stopped by the limit, " << st.safety_net_hits
            << " by the wall-clock safety net\n";
}

// root | @func | ^label | nID
bool findScope(const brrr::Module& m, const std::string& tok, brrr::NodeId* out) {
  using namespace brrr;
  if (tok == "root") {
    *out = kInvalidId;
    return true;
  }
  NodeId id = kInvalidId;
  if (parseNodeIdToken(tok, &id)) {
    *out = id;
    return m.contains(id);
  }
  for (const Node& n : m.nodes) {
    if ((tok[0] == '@' && n.kind == "func" && n.symbol == tok.substr(1)) ||
        (tok[0] == '^' && n.kind == "block" && n.symbol == tok.substr(1))) {
      *out = n.id;
      return true;
    }
  }
  return false;
}

std::string scopeLabel(const brrr::Module& m, brrr::NodeId scope) {
  if (scope == brrr::kInvalidId) return "root";
  const brrr::Node* n = m.find(scope);
  if (!n) return "n" + std::to_string(scope);
  if (n->kind == "func" || n->kind == "module") return "@" + n->symbol;
  if (n->kind == "block") return "^" + n->symbol;
  return "n" + std::to_string(scope);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Canonical text is LF-only on every platform; text-mode stdio would emit CRLF.
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  using namespace brrr;
  if (argc < 2) return usage();
  if (const char* v = std::getenv("BRRR_SOLVER_STATS"); v && *v && *v != '0') {
    std::atexit(printSolverStats);
  }
  const std::string cmd = argv[1];

  std::vector<std::string> positional;
  pipeline::Options popts;
  bool core_only = false, strict = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&](const char* flag, std::string* out) {
      const size_t n = std::char_traits<char>::length(flag);
      if (a.compare(0, n, flag) != 0) return false;
      *out = a.substr(n);
      return true;
    };
    std::string v;
    if (val("--schemas=", &v)) popts.schema_files.push_back(v);
    else if (val("--include=", &v)) g_include_dirs.push_back(v);
    else if (val("--store=", &v)) popts.store = v;
    else if (val("--pipeline=", &v)) popts.pipeline_file = v;
    else if (val("--passes=", &v)) popts.passes = v;
    else if (val("--state=", &v)) popts.state_file = v;
    else if (val("--steps=", &v)) popts.steps = std::atoi(v.c_str());
    else if (val("--solver-rlimit=", &v)) symbolic::z3::setRlimit(std::strtoull(v.c_str(), nullptr, 10));
    else if (a == "--pause-on-warning") popts.pause_on_warning = true;
    else if (a == "--ids") popts.ids = true;
    else if (a == "--record") popts.record = true;
    else if (a == "--core-only") core_only = true;
    else if (a == "--strict") strict = true;
    else if (a.rfind("--", 0) == 0) {
      std::cerr << "unknown flag " << a << "\n";
      return usage();
    } else {
      positional.push_back(a);
    }
  }

  schema::Registry reg = schema::buildRegistry(!core_only, !core_only);
  for (const std::string& sp : popts.schema_files) {
    bool ok = false;
    std::string e;
    const std::string text = readFile(sp, &ok, &e);
    if (!ok) {
      std::cerr << "error: " << e << "\n";
      return 66;
    }
    if (!schema::parseSchema(text, &reg, &e)) {
      std::cerr << sp << ": schema error: " << e << "\n";
      return 1;
    }
  }

  if (cmd == "from-affine") {  // MLIR import needs no file: map + operands
    if (positional.empty()) return usage();
    size_t nd = 0, ns = 0;
    std::string e;
    if (!interop::affineArity(positional[0], &nd, &ns, &e)) {
      std::cerr << "error: " << e << "\n";
      return 1;
    }
    if (positional.size() != 1 + nd + ns) {
      std::cerr << "error: the map takes " << nd << " dim(s) and " << ns
                << " symbol(s): give them in that order\n";
      return 64;
    }
    const std::vector<std::string> dims(positional.begin() + 1, positional.begin() + 1 + nd);
    const std::vector<std::string> syms(positional.begin() + 1 + nd, positional.end());
    std::vector<symbolic::Expr> out;
    if (!interop::fromAffineMap(positional[0], dims, syms, &out, &e)) {
      std::cerr << "error: " << e << "\n";
      return 1;
    }
    for (size_t i = 0; i < out.size(); ++i) {
      std::cout << (i ? ", " : "") << symbolic::printExpr(out[i]);
    }
    std::cout << "\n";
    return 0;
  }

  if (cmd == "schema") {
    for (const auto& [name, s] : reg.scalars()) {
      std::cout << "scalar " << name << " { domain: " << s.domain << ", bits: " << s.bits << " }\n";
    }
    for (const auto& [name, k] : reg.kinds()) std::cout << "\n" << schema::printKind(k);
    return 0;
  }

  // The rest is just modules: expand includes into one document.
  std::string text;
  const bool needs_file = !pipeline::isCommand(cmd) || cmd == "run";
  if (needs_file) {
    if (positional.empty()) return usage();
    std::vector<std::string> stack;
    std::string e;
    if (!expandIncludes(positional[0], &stack, &text, &e)) {
      std::cerr << "error: " << e << "\n";
      return 1;
    }
  }
  if (pipeline::isCommand(cmd)) {
    return pipeline::command(cmd, positional, popts, reg, text, std::cout, std::cerr);
  }

  const std::string& path = positional[0];
  ParseResult pr = parseModule(text);
  if (!pr.ok) {
    std::cerr << path << ":" << pr.line << ": error: " << pr.error << "\n";
    return 1;
  }
  Module& m = pr.module;
  const std::vector<verify::Issue> issues = sema::verifyModule(m, reg);

  if (cmd == "check") {
    printIssues(issues, true);
    size_t e = 0, w = 0;
    for (const auto& is : issues) (is.error ? e : w)++;
    std::cout << "errors: " << e << ", warnings: " << w << (strict && w ? " (strict)" : "") << "\n";
    return e || (strict && w) ? 1 : 0;
  }
  if (verify::hasErrors(issues)) {  // an ill-formed document never reaches a command
    printIssues(issues, false);
    return 1;
  }

  if (cmd == "print") {
    std::cout << printModule(m, {popts.ids});
    return 0;
  }
  if (cmd == "to-json") {
    std::cout << io::toJson(m);
    return 0;
  }
  if (cmd == "roundtrip") {
    const std::string a = printModule(m, {true});
    ParseResult pr2 = parseModule(a);
    if (!pr2.ok) {
      std::cerr << "roundtrip reparse failed: line " << pr2.line << ": " << pr2.error << "\n";
      return 2;
    }
    if (printModule(pr2.module, {true}) != a) {
      std::cerr << "roundtrip mismatch: id/text stability violated\n";
      return 3;
    }
    const std::string clean = printModule(m);
    ParseResult pr3 = parseModule(clean);
    if (!pr3.ok || printModule(pr3.module) != clean) {
      std::cerr << "roundtrip mismatch: the clean form does not reparse to itself\n";
      return 3;
    }
    std::cout << a;
    std::cerr << "roundtrip ok: " << m.nodes.size() << " nodes, ids stable\n";
    return 0;
  }
  if (cmd == "facts") {
    for (const Module::FactEntry& f : m.facts) {
      symbolic::ParsedFact pf;
      std::string e;
      symbolic::parseFact(f.text, &pf, &e);
      std::cout << "[" << scopeLabel(m, f.scope) << "] fact "
                << symbolic::printFact(pf, [&](const std::string& a) {
                     ValueRef v;
                     if (!parseValueAtom(a, &v)) return a;
                     const Node* n = m.find(v.id);
                     return n && v.port < n->names.size() && !n->names[v.port].empty()
                                ? "%" + n->names[v.port]
                                : a;
                   })
                << "\n";
    }
    return 0;
  }
  if (cmd == "types") {
    sema::Sema sema(m, reg);
    for (const Node& n : m.nodes) {
      if (n.kind == "func") std::cout << "@" << n.symbol << "\n";
      if (n.kind == "func" || n.kind == "module" || n.kind == "block") continue;
      const std::string indent = m.namingScope(n.scope) != kInvalidId ? "  " : "";
      for (uint32_t p = 0; p < n.numResults(); ++p) {
        const ValueRef v{n.id, p};
        const std::string self = valueAtom(v);
        std::string line = sema.show(symbolic::ExprTerm::var(self)) + " = " + n.kind;
        if (Type t = sema.typeOf(v)) {
          if (t->shaped) {  // the value's own fresh dims print as `?`
            auto own = std::make_shared<TypeTerm>(*t);
            for (size_t k = 0; k < own->dims.size(); ++k) {
              const auto& d = own->dims[k];
              if (d->kind == symbolic::ExprTerm::Kind::Var && d->name == dimAtom(v, k)) {
                own->dims[k] = symbolic::ExprTerm::var("?");
              }
            }
            t = own;
          }
          line += " : " + printType(t, [&](const std::string& a) {
            return sema.show(symbolic::ExprTerm::var(a));
          });
          if (sema.isInferred(v)) line += "  (inferred)";
        }
        const symbolic::Expr e = sema.meaning(v);
        if (!(e->kind == symbolic::ExprTerm::Kind::Var && e->name == self)) {
          line += "  means " + sema.show(e);
        }
        std::cout << indent << line << "\n";
      }
    }
    return 0;
  }
  if (cmd == "rename") {
    if (positional.size() < 3) return usage();
    ValueRef v;
    const std::string& who = positional[1];
    const bool ok = parseValueAtom(who, &v) ||
                    (who.size() > 1 && who[0] == '%' && m.resolveName(kInvalidId, who.substr(1), &v));
    bool found = ok;
    if (!found && who.size() > 1 && who[0] == '%') {  // search every function
      for (const Node& n : m.nodes) {
        for (uint32_t p = 0; p < n.names.size(); ++p) {
          if (n.names[p] == who.substr(1)) {
            v = {n.id, p};
            found = true;
          }
        }
      }
    }
    if (!found) {
      std::cerr << "error: unknown value '" << who << "'\n";
      return 1;
    }
    std::string nn = positional[2];
    if (!nn.empty() && nn[0] == '%') nn = nn.substr(1);
    std::string e;
    if (!m.rename(v, nn == "-" ? "" : nn, &e)) {
      std::cerr << "error: " << e << "\n";
      return 1;
    }
    std::cout << printModule(m, {popts.ids});
    for (const JournalEntry& j : m.journal) {
      std::cerr << "[journal] " << j.action << " n" << j.id << ": " << j.detail << "\n";
    }
    return 0;
  }
  if (cmd == "to-affine") {
    if (positional.size() < 3) return usage();
    NodeId scope = kInvalidId;
    if (!findScope(m, positional[1], &scope)) {
      std::cerr << "error: unknown scope '" << positional[1] << "' (root, @func, ^block, nID)\n";
      return 1;
    }
    std::string q;
    for (size_t i = 2; i < positional.size(); ++i) q += (i > 2 ? " " : "") + positional[i];
    sema::Sema sema(m, reg);
    const symbolic::Solver& s = sema.solverFor(scope);
    std::vector<symbolic::Expr> exprs;
    for (const std::string& piece : symbolic::splitTopLevel(q, ',')) {
      std::string e;
      symbolic::Expr x = symbolic::parseExpr(piece, &e);
      if (!x) {
        std::cerr << "error: in `" << piece << "`: " << e << "\n";
        return 1;
      }
      std::string bad;
      x = symbolic::mapVars(x, [&](const std::string& a) -> std::string {
        ValueRef v;
        if (a[0] == '%') {
          if (parseValueAtom(a, &v) || m.resolveName(scope, a.substr(1), &v)) return valueAtom(v);
          bad = "unknown value '" + a + "'";
          return a;
        }
        return a;
      });
      // A module symbol is not an SSA value: export only works if facts pin it.
      std::set<std::string> atoms;
      symbolic::collectVars(x, &atoms);
      std::map<std::string, symbolic::Expr> consts;
      for (const std::string& a : atoms) {
        if (a[0] == '%' || a[0] == '?') continue;
        if (auto c = s.constantValue(symbolic::ExprTerm::var(a))) {
          consts[a] = symbolic::ExprTerm::constant(*c);
        } else if (bad.empty()) {
          bad = "module symbol '" + a + "' has no constant value (MLIR needs an SSA value)";
        }
      }
      if (!bad.empty()) {
        std::cerr << "error: " << bad << "\n";
        return 1;
      }
      std::function<symbolic::Expr(const symbolic::Expr&)> sub = [&](const symbolic::Expr& t) {
        if (t->kind == symbolic::ExprTerm::Kind::Var) {
          auto it = consts.find(t->name);
          return it == consts.end() ? t : it->second;
        }
        if (t->kind == symbolic::ExprTerm::Kind::Const) return t;
        std::vector<symbolic::Expr> args;
        for (const auto& c : t->args) args.push_back(sub(c));
        return symbolic::ExprTerm::rebuild(t, std::move(args));
      };
      exprs.push_back(sub(x));
    }
    // MLIR symbols are values fixed for the whole function body: its arguments
    // and what its entry computes. Everything else is a dim.
    const NodeId fn = m.namingScope(scope);
    auto isSymbol = [&](const std::string& a) {
      ValueRef v;
      const Node* d = atomValue(a, &v) ? m.find(v.id) : nullptr;
      return d && d->scope == fn;
    };
    auto show = [&](const std::string& a) { return sema.show(symbolic::ExprTerm::var(a)); };
    auto operands = [&](const interop::AffineMap& am) {
      std::string t = "(";
      for (size_t i = 0; i < am.dims.size(); ++i) t += (i ? ", " : "") + show(am.dims[i]);
      t += ")";
      if (!am.symbols.empty()) {
        t += "[";
        for (size_t i = 0; i < am.symbols.size(); ++i) t += (i ? ", " : "") + show(am.symbols[i]);
        t += "]";
      }
      return t;
    };
    bool all_affine = true;
    for (size_t i = 0; i < exprs.size(); ++i) {
      std::cout << "[" << i << "] " << sema.show(exprs[i]) << "\n    ";
      ValueRef v;
      if (auto c = s.constantValue(exprs[i])) {
        std::cout << "static " << *c << "            (OpFoldResult: an attribute)\n";
      } else if (exprs[i]->kind == symbolic::ExprTerm::Kind::Var &&
                 parseValueAtom(exprs[i]->name, &v)) {
        std::cout << "value " << show(exprs[i]->name) << "          (OpFoldResult: a value)\n";
        // A value that is itself an affine formula (`apply`, `add`, ...) exports
        // as the affine.apply that computes it.
        const symbolic::Expr def = sema.meaning(v);
        interop::AffineMap am;
        std::string e;
        if (!(def->kind == symbolic::ExprTerm::Kind::Var && def->name == exprs[i]->name) &&
            interop::toAffineMap({def}, isSymbol, &am, &e, show)) {
          std::cout << "    defined as affine.apply " << am.text << operands(am) << "\n";
        }
      } else {
        interop::AffineMap am;
        std::string e;
        if (interop::toAffineMap({exprs[i]}, isSymbol, &am, &e, show)) {
          std::cout << "affine.apply " << am.text << operands(am)
                    << "   (OpFoldResult: the value it computes)\n";
        } else {
          all_affine = false;
          std::cout << e << " — export it as arith ops instead\n";
        }
      }
    }
    if (exprs.size() > 1 && all_affine) {
      interop::AffineMap am;
      std::string e;
      if (interop::toAffineMap(exprs, isSymbol, &am, &e)) {
        std::cout << "map: " << am.text << operands(am) << "\n";
      }
    }
    return 0;
  }
  if (cmd == "ask") {
    if (positional.size() < 3) return usage();
    NodeId scope = kInvalidId;
    if (!findScope(m, positional[1], &scope)) {
      std::cerr << "error: unknown scope '" << positional[1] << "' (root, @func, ^block, nID)\n";
      return 1;
    }
    std::string q;
    for (size_t i = 2; i < positional.size(); ++i) q += (i > 2 ? " " : "") + positional[i];
    symbolic::ParsedFact pf;
    std::string e;
    if (!symbolic::parseFact(q, &pf, &e)) {
      std::cerr << "error: " << e << "\n";
      return 1;
    }
    bool bad = false;
    pf = symbolic::mapFactVars(pf, [&](const std::string& a) {
      ValueRef v;
      if (a[0] != '%' || parseValueAtom(a, &v)) return a;
      if (m.resolveName(scope, a.substr(1), &v)) return valueAtom(v);
      bad = true;
      std::cerr << "error: unknown value '" << a << "' in scope " << positional[1] << "\n";
      return a;
    });
    if (bad) return 1;
    sema::Sema sema(m, reg);
    const symbolic::Solver& s = sema.solverFor(scope);
    symbolic::Tri t = symbolic::Tri::Yes;
    auto both = [&](symbolic::Tri x) {
      if (x == symbolic::Tri::No || t == symbolic::Tri::No) t = symbolic::Tri::No;
      else if (x == symbolic::Tri::Unknown) t = symbolic::Tri::Unknown;
    };
    if (pf.kind == symbolic::ParsedFact::Kind::Aligned) {
      both(s.aligned(pf.exprs[0], pf.a));
    } else if (pf.kind == symbolic::ParsedFact::Kind::Range) {
      both(s.lessOrEqual(symbolic::ExprTerm::constant(pf.a), pf.exprs[0]));
      both(s.lessOrEqual(pf.exprs[0], symbolic::ExprTerm::constant(pf.b)));
    } else {
      for (size_t k = 0; k < pf.ops.size(); ++k) {
        const auto& l = pf.exprs[k];
        const auto& r = pf.exprs[k + 1];
        both(pf.ops[k] == "=" ? s.isEqual(l, r)
                              : pf.ops[k] == "<=" ? s.lessOrEqual(l, r) : s.lessOrEqual(r, l));
      }
    }
    std::cout << q << " -> " << symbolic::toString(t) << " (scope " << positional[1] << ")\n";
    return 0;
  }
  return usage();
}
