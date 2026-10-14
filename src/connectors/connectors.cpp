#include "connectors/connectors.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "symbolic/expr_io.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace brrr::connectors {
namespace {

#ifdef _WIN32
constexpr const char* kNull = "NUL";
#else
constexpr const char* kNull = "/dev/null";
#endif

// Runs `python <script> "<a>" "<b>"` and returns its first output line. The
// script travels through a file and the arguments through double quotes, which
// POSIX sh and cmd.exe both accept; the arguments are sanitized expressions
// (identifiers, integers, + - * and parentheses), so nothing needs escaping.
std::string runPython(const std::string& python, const std::string& code,
                      const std::string& a, const std::string& b) {
  const auto script = std::filesystem::temp_directory_path() / "brrr_sympy.py";
  std::ofstream(script, std::ios::binary) << code;
  std::string cmd;
#ifndef _WIN32
  cmd = "timeout 10 ";
#endif
  cmd += python + " \"" + script.string() + "\" \"" + a + "\" \"" + b + "\" 2>" + kNull;
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return {};
  char buf[128] = {0};
  const bool got = std::fgets(buf, sizeof buf, f) != nullptr;
  pclose(f);
  return got ? std::string(buf) : std::string();
}

// SymPy cannot read our atom spellings (`%7`, `dim(%7, 0)`), so every atom is
// renamed s0, s1, ... consistently across both sides of the question.
Expr renameAtoms(const Expr& e, std::map<std::string, std::string>* names) {
  using symbolic::ExprTerm;
  if (e->kind == ExprTerm::Kind::Const) return e;
  if (e->kind == ExprTerm::Kind::Var) {
    auto it = names->find(e->name);
    if (it == names->end()) {
      it = names->emplace(e->name, "s" + std::to_string(names->size())).first;
    }
    return ExprTerm::var(it->second);
  }
  std::vector<Expr> args;
  for (const Expr& a : e->args) args.push_back(renameAtoms(a, names));
  return ExprTerm::rebuild(e, std::move(args));
}

}  // namespace

Hub::Hub() {
  for (const char* py : {"python3", "python"}) {
    const std::string probe = std::string(py) + " -c \"import sympy\" >" + kNull + " 2>&1";
    if (std::system(probe.c_str()) == 0) {
      python_ = py;
      sympy_available_ = true;
      break;
    }
  }
}

Answer Hub::isEqual(const Solver& facts, const Expr& a, const Expr& b) {
  // 1. Local first: cheap, deterministic, in-process.
  const Tri local = facts.isEqual(a, b);
  if (local != Tri::Unknown) return {local, "local", ""};

  // 2. Cache by content, over fact-substituted expressions.
  const Expr a2 = facts.subst(a);
  const Expr b2 = facts.subst(b);
  const std::string key = symbolic::pretty(a2) + " == " + symbolic::pretty(b2);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    Answer cached = it->second;
    cached.detail += " (cached)";
    return cached;
  }

  // 3. Escalate to the connector.
  Answer ans;
  if (!sympy_available_) {
    ans.detail = "no connector available to decide";
    return ans;
  }
  ans = askSympyEqual(a2, b2);
  cache_[key] = ans;
  return ans;
}

Answer Hub::askSympyEqual(const Expr& a, const Expr& b) {
  static const char* kScript =
      "import sys\n"
      "import sympy\n"
      "try:\n"
      "    d = sympy.simplify(sympy.sympify(sys.argv[1]) - sympy.sympify(sys.argv[2]))\n"
      "    z = d.is_zero\n"
      "except Exception:\n"
      "    z = None\n"
      "print(\"Yes\" if z is True else (\"No\" if z is False else \"Unknown\"))\n";

  ++external_calls_;
  Answer ans;
  ans.source = "connector@sympy";
  std::map<std::string, std::string> names;
  const std::string out = runPython(python_, kScript, symbolic::printExpr(renameAtoms(a, &names)),
                                    symbolic::printExpr(renameAtoms(b, &names)));
  if (out.rfind("Yes", 0) == 0) {
    ans.tri = Tri::Yes;
    ans.detail = "sympy.simplify(a - b).is_zero";
  } else if (out.rfind("No", 0) == 0) {
    ans.tri = Tri::No;
    ans.detail = "sympy.simplify(a - b).is_zero";
  } else {
    ans.detail = out.empty() ? "connector failed" : "connector undecided";
  }
  return ans;
}

}  // namespace brrr::connectors
