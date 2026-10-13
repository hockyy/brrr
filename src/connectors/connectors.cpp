#include "connectors/connectors.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace bridgeir::connectors {
namespace {

// Our expression grammar (identifiers, integers, "+", "*", parentheses) is safe
// inside single quotes, so it can be embedded directly into a shell command.
std::string runPython(const std::string& code, const std::string& arg1,
                      const std::string& arg2) {
  const std::string cmd = "timeout 10 python3 -c '" + code + "' '" + arg1 +
                          "' '" + arg2 + "' 2>/dev/null";
  FILE* f = ::popen(cmd.c_str(), "r");
  if (!f) return {};
  char buf[128] = {0};
  const bool got = std::fgets(buf, sizeof buf, f) != nullptr;
  ::pclose(f);
  return got ? std::string(buf) : std::string();
}

}  // namespace

Hub::Hub() {
  sympy_available_ =
      std::system("python3 -c 'import sympy' >/dev/null 2>&1") == 0;
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
  const std::string out =
      runPython(kScript, symbolic::pretty(a), symbolic::pretty(b));
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

}  // namespace bridgeir::connectors
