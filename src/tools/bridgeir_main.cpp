#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "io/c_like.h"
#include "io/json.h"
#include "ir/ir.h"
#include "ir/text.h"
#include "passes/demo_fuse.h"
#include "passes/fold_reshape.h"
#include "schema/schema.h"
#include "symbolic/expr_io.h"
#include "symbolic/facts.h"
#include "symbolic/symbolic.h"
#include "verify/check.h"

namespace {

// Include search paths (`--include=<dir>`), C-style.
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
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool fileExists(const std::string& p) {
  std::ifstream in(p);
  return in.good();
}

// "name" searches relative-first; <name> searches include dirs first. `.br` is
// appended when the bare name does not exist.
bool resolveInclude(const std::string& from_dir, bool angled,
                    const std::string& target, std::string* resolved) {
  auto tryPath = [&](const std::string& base) -> bool {
    if (fileExists(base)) {
      *resolved = base;
      return true;
    }
    if (fileExists(base + ".br")) {
      *resolved = base + ".br";
      return true;
    }
    return false;
  };
  if (!angled) {
    if (tryPath(from_dir + "/" + target)) return true;
    for (const std::string& d : g_include_dirs) {
      if (tryPath(d + "/" + target)) return true;
    }
  } else {
    for (const std::string& d : g_include_dirs) {
      if (tryPath(d + "/" + target)) return true;
    }
    if (tryPath(from_dir + "/" + target)) return true;
  }
  return false;
}

// C-style textual inclusion: the rest is just modules. Cycle-checked, depth-limited.
bool expandIncludes(const std::string& path, std::vector<std::string>* stack,
                    std::string* out, std::string* err) {
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
      const char close = angled ? '>' : '"';
      const size_t e = rest.find(close, 1);
      if (rest.empty() || e == std::string::npos || e < 1) {
        *err = path + ": malformed #include";
        return false;
      }
      const std::string target = rest.substr(1, e - 1);
      std::string resolved;
      if (!resolveInclude(dirOf(path), angled, target, &resolved)) {
        *err = path + ": cannot resolve #include <" + target + ">";
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
  std::cerr
      << "usage: bridgeir <command> <file.br> [args] [--schemas=<file>]\n"
         "                 [--include=<dir>] [--c-like] [--core-only]\n"
         "  print | roundtrip | demo | rename | facts | fold-reshape | check | "
         "to-json\n"
         "  schema   print the loaded vocabulary (scalars + kinds; no file "
         "needed)\n"
         "  ask      ask a scoped question: ask <file> <scope|root> \"a <= b\"\n";
  return 64;
}

void printJournal(const bridgeir::Module& m) {
  for (const bridgeir::JournalEntry& e : m.journal) {
    std::cerr << "[journal] " << e.action << " n" << e.id << ": " << e.detail
              << "\n";
  }
}

std::string attrPolicyString(const bridgeir::schema::Kind& k) {
  using Policy = bridgeir::schema::Kind::AttrPolicy;
  if (k.attr_policy == Policy::None) return "none";
  if (k.attr_policy == Policy::Free) return "free";
  std::string s;
  for (const auto& [an, at] : k.attrs) {
    if (!s.empty()) s += ", ";
    s += an + ":" + at;
  }
  return s;
}

void printIssues(const std::vector<bridgeir::verify::Issue>& issues,
                 bool include_warnings) {
  for (const bridgeir::verify::Issue& is : issues) {
    if (!is.error && !include_warnings) continue;
    std::cout << (is.error ? "error: " : "warning: ") << "n" << is.node << " ("
              << is.kind << "): " << is.message << "\n";
  }
}

std::string scopeLabel(const bridgeir::Module& m, bridgeir::NodeId scope) {
  if (scope == bridgeir::kInvalidId) return "root";
  const bridgeir::Node* n = m.find(scope);
  if (!n) return "n" + std::to_string(scope);
  if (!n->name.empty()) return n->name;
  const std::string sym = n->attrString("sym", "");
  if (!sym.empty()) return sym;
  return "n" + std::to_string(scope);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];

  // Flags + positional arguments (paths and query words in order).
  std::vector<std::string> positional;
  bool c_like = false;
  bool core_only = false;
  std::vector<std::pair<std::string, std::string>> schema_files;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--schemas=", 0) == 0) {
      schema_files.emplace_back(a.substr(10), std::string());
    } else if (a.rfind("--include=", 0) == 0) {
      g_include_dirs.push_back(a.substr(10));
    } else if (a == "--c-like") {
      c_like = true;
    } else if (a == "--core-only") {
      core_only = true;
    } else {
      positional.push_back(a);
    }
  }

  // Vocabulary: core module always; std + control modules by default.
  bridgeir::schema::Registry reg =
      bridgeir::schema::buildRegistry(!core_only, !core_only);
  for (auto& [sp, unused] : schema_files) {
    bool sok = false;
    std::string serr;
    const std::string stext = readFile(sp, &sok, &serr);
    if (!sok) {
      std::cerr << "error: " << serr << "\n";
      return 66;
    }
    std::string perr;
    if (!bridgeir::schema::parseSchema(stext, &reg, &perr)) {
      std::cerr << sp << ": schema error: " << perr << "\n";
      return 1;
    }
  }

  if (cmd == "schema") {
    for (const auto& [name, s] : reg.scalars()) {
      std::cout << "scalar " << name << " { domain: " << s.domain
                << ", bits: " << s.bits << " }\n";
    }
    if (!reg.scalars().empty()) std::cout << "\n";
    for (const auto& [name, k] : reg.kinds()) {
      std::string head = "kind " + name;
      if (!k.parent.empty()) head += " < " + k.parent;
      std::cout << head << " {\n";
      if (k.operands_none) {
        std::cout << "  operands: none\n";
      } else {
        std::string ops;
        for (size_t i = 0; i < k.operands.size(); ++i) {
          if (i) ops += ", ";
          ops += k.operands[i];
          if (i < k.operand_types.size() && !k.operand_types[i].empty() &&
              k.operand_types[i] != "any") {
            if (!k.operands[i].empty()) ops += ": ";
            ops += k.operand_types[i];
          }
        }
        if (k.operand_tuple_width > 0) {
          ops = "(" + ops + ")*";
        } else if (k.operands_variadic) {
          ops += "*";
        }
        std::cout << "  operands: " << ops << "\n";
      }
      std::cout << "  results: " << (k.results_value ? "value" : "none")
                << "\n";
      std::cout << "  attrs: " << attrPolicyString(k) << "\n";
      if (!k.traits.empty()) {
        std::cout << "  traits:";
        for (const std::string& t : k.traits) std::cout << " " << t;
        std::cout << "\n";
      }
      std::cout << "}\n";
    }
    return 0;
  }

  if (positional.empty()) return usage();
  const std::string path = positional[0];

  // The rest is just modules: expand includes into one document.
  std::string text;
  {
    std::vector<std::string> stack;
    std::string ierr;
    if (!expandIncludes(path, &stack, &text, &ierr)) {
      std::cerr << "error: " << ierr << "\n";
      return 1;
    }
  }

  bridgeir::ParseResult pr = bridgeir::parseModule(text);
  if (!pr.ok) {
    std::cerr << path << ":" << pr.line << ": error: " << pr.error << "\n";
    return 1;
  }

  // Language-level validation: scoped facts must be consistent for every command.
  {
    std::string ferr;
    if (!bridgeir::symbolic::validateAllFacts(pr.module, &ferr)) {
      std::cerr << path << ": error: " << ferr << "\n";
      return 1;
    }
  }

  // Schema validation: declared vocabulary is strict; foreign kinds warn.
  const std::vector<bridgeir::verify::Issue> issues =
      bridgeir::verify::checkModule(pr.module, reg);
  const bool errors = bridgeir::verify::hasErrors(issues);

  if (cmd == "check") {
    printIssues(issues, true);
    size_t e = 0, w = 0;
    for (const bridgeir::verify::Issue& is : issues) (is.error ? e : w)++;
    std::cout << "errors: " << e << ", warnings: " << w << "\n";
    return e ? 1 : 0;
  }

  if (errors) {  // an ill-formed document never reaches a pass
    printIssues(issues, false);
    return 1;
  }

  if (cmd == "print") {
    std::cout << (c_like ? bridgeir::io::toCLike(pr.module)
                         : bridgeir::printModule(pr.module));
    return 0;
  }

  if (cmd == "to-json") {
    std::cout << bridgeir::io::toJson(pr.module);
    return 0;
  }

  if (cmd == "roundtrip") {
    const std::string a = bridgeir::printModule(pr.module);
    bridgeir::ParseResult pr2 = bridgeir::parseModule(a);
    if (!pr2.ok) {
      std::cerr << "roundtrip reparse failed: " << pr2.error << "\n";
      return 2;
    }
    const std::string b = bridgeir::printModule(pr2.module);
    if (a != b) {
      std::cerr << "roundtrip mismatch: id/text stability violated\n";
      return 3;
    }
    std::cout << a;
    std::cerr << "roundtrip ok: " << pr2.module.nodes.size()
              << " nodes, ids stable\n";
    return 0;
  }

  if (cmd == "demo") {
    bridgeir::demoInsertReluAfterAdd(pr.module);
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  if (cmd == "rename") {
    if (positional.size() < 3) return usage();
    const std::string target_str = positional[1];
    std::string new_name = positional[2];
    if (new_name == "-") new_name.clear();

    bridgeir::NodeId target = bridgeir::kInvalidId;
    bridgeir::NodeId as_id = 0;
    if (bridgeir::parseNodeIdToken(target_str, &as_id)) {
      target = as_id;
    } else {
      int matches = 0;
      for (const bridgeir::Node& n : pr.module.nodes) {
        if (n.name == target_str) {
          target = n.id;
          ++matches;
        }
      }
      if (matches != 1) {
        std::cerr << "error: " << (matches == 0 ? "unknown" : "ambiguous")
                  << " name '" << target_str << "'\n";
        return 1;
      }
    }

    std::string rerr;
    if (!pr.module.renameNode(target, new_name, &rerr)) {
      std::cerr << "error: " << rerr << "\n";
      return 1;
    }
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  if (cmd == "facts") {
    for (const bridgeir::Module::FactEntry& f : pr.module.facts) {
      std::cout << "ok: [" << scopeLabel(pr.module, f.scope) << "] " << f.line
                << "\n";
    }
    std::cerr << pr.module.facts.size() << " facts, all consistent\n";
    return 0;
  }

  if (cmd == "fold-reshape") {
    std::string serr;
    const bridgeir::symbolic::Solver facts = bridgeir::symbolic::solverForScope(
        pr.module, bridgeir::kInvalidId, &serr);
    bridgeir::foldLooseReshapePairs(pr.module, facts);
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  if (cmd == "ask") {
    if (positional.size() < 3) return usage();
    const std::string scope_tok = positional[1];
    std::string query;
    for (size_t i = 2; i < positional.size(); ++i) {
      if (i > 2) query += " ";
      query += positional[i];
    }

    bridgeir::NodeId scope = bridgeir::kInvalidId;
    bridgeir::NodeId as_id = 0;
    if (scope_tok == "root") {
      scope = bridgeir::kInvalidId;
    } else if (bridgeir::parseNodeIdToken(scope_tok, &as_id)) {
      if (!pr.module.contains(as_id)) {
        std::cerr << "error: no such scope n" << as_id << "\n";
        return 1;
      }
      scope = as_id;
    } else {
      const bridgeir::Node* found = nullptr;
      for (const bridgeir::Node& n : pr.module.nodes) {
        const bool by_name = !n.name.empty() && n.name == scope_tok;
        const bool by_sym = n.attrString("sym", "") == scope_tok;
        if (by_name || by_sym) {
          if (found && found->id != n.id) {
            std::cerr << "error: ambiguous scope '" << scope_tok << "'\n";
            return 1;
          }
          found = &n;
        }
      }
      if (!found) {
        std::cerr << "error: unknown scope '" << scope_tok << "'\n";
        return 1;
      }
      scope = found->id;
    }

    // Functor query: aligned(expr, modulus)
    if (query.rfind("aligned(", 0) == 0 && query.back() == ')') {
      const std::string inner = query.substr(8, query.size() - 9);
      const std::vector<std::string> parts =
          bridgeir::symbolic::splitTopLevel(inner, ',');
      if (parts.size() != 2) {
        std::cerr << "error: aligned expects aligned(expr, modulus)\n";
        return 1;
      }
      std::string perr;
      const bridgeir::symbolic::Expr e =
          bridgeir::symbolic::parseExpr(parts[0], &perr);
      if (!e) {
        std::cerr << "error: " << perr << "\n";
        return 1;
      }
      int64_t m = 0;
      bool ok_int = !parts[1].empty();
      for (const char c : parts[1]) {
        if (c < '0' || c > '9') {
          ok_int = false;
          break;
        }
        m = m * 10 + (c - '0');
      }
      if (!ok_int) {
        std::cerr << "error: modulus must be a positive integer\n";
        return 1;
      }
      std::string serr;
      const bridgeir::symbolic::Solver sf =
          bridgeir::symbolic::solverForScope(pr.module, scope, &serr);
      const bridgeir::symbolic::Tri t = sf.aligned(e, m);
      std::cout << "aligned(" << parts[0] << ", " << m << ") -> "
                << bridgeir::symbolic::toString(t) << " (scope "
                << scopeLabel(pr.module, scope) << ")\n";
      return 0;
    }

    const std::pair<std::string, int> ops[] = {
        {"==", 0}, {"<=", 1}, {">=", 3}, {"<", 2}, {">", 4}};
    size_t op_pos = std::string::npos;
    int op_kind = -1;
    std::string op_str;
    for (const auto& [tok, kind] : ops) {
      const size_t p = query.find(tok);
      if (p != std::string::npos) {
        op_pos = p;
        op_kind = kind;
        op_str = tok;
        break;
      }
    }
    if (op_pos == std::string::npos) {
      std::cerr << "error: query needs an operator (==, <=, <, >=, >)\n";
      return 1;
    }
    const std::string lhs_s = query.substr(0, op_pos);
    const std::string rhs_s = query.substr(op_pos + op_str.size());
    std::string perr;
    const bridgeir::symbolic::Expr lhs =
        bridgeir::symbolic::parseExpr(lhs_s, &perr);
    if (!lhs) {
      std::cerr << "error: lhs: " << perr << "\n";
      return 1;
    }
    const bridgeir::symbolic::Expr rhs =
        bridgeir::symbolic::parseExpr(rhs_s, &perr);
    if (!rhs) {
      std::cerr << "error: rhs: " << perr << "\n";
      return 1;
    }

    std::string serr;
    const bridgeir::symbolic::Solver facts =
        bridgeir::symbolic::solverForScope(pr.module, scope, &serr);
    bridgeir::symbolic::Tri tri = bridgeir::symbolic::Tri::Unknown;
    switch (op_kind) {
      case 0: tri = facts.isEqual(lhs, rhs); break;
      case 1: tri = facts.lessOrEqual(lhs, rhs); break;
      case 2: tri = facts.isLess(lhs, rhs); break;
      case 3: tri = facts.lessOrEqual(rhs, lhs); break;
      case 4: tri = facts.isLess(rhs, lhs); break;
    }
    std::cout << lhs_s << " " << op_str << " " << rhs_s << " -> "
              << bridgeir::symbolic::toString(tri) << " (scope "
              << scopeLabel(pr.module, scope) << ")\n";
    return 0;
  }

  return usage();
}
