// Connector test: local-first routing, SymPy escalation, content caching.
#include <cstdio>
#include <string>
#include <vector>

#include "connectors/connectors.h"
#include "symbolic/symbolic.h"

using bridgeir::connectors::Hub;
using bridgeir::symbolic::Expr;
using bridgeir::symbolic::ExprTerm;
using bridgeir::symbolic::Solver;
using bridgeir::symbolic::Tri;

static int failures = 0;

static void expect(bool cond, const std::string& what) {
  std::printf("%s %s\n", cond ? "ok:  " : "FAIL:", what.c_str());
  if (!cond) ++failures;
}

static Expr C(int64_t c) { return ExprTerm::constant(c); }
static Expr V(const char* n) { return ExprTerm::var(n); }
static Expr Add(std::vector<Expr> xs) { return ExprTerm::add(std::move(xs)); }
static Expr Mul(std::vector<Expr> xs) { return ExprTerm::mul(std::move(xs)); }

int main() {
  Hub hub;
  expect(hub.sympyAvailable(),
         "sympy connector available (python3 -c 'import sympy')");

  const Solver facts;

  // 1. Local solver decides the affine fragment; connector untouched.
  {
    const Expr lhs = Mul({Add({V("a"), V("b")}), C(2)});
    const Expr rhs = Add({Mul({C(2), V("a")}), Mul({C(2), V("b")})});
    const int before = hub.externalCalls();
    const auto ans = hub.isEqual(facts, lhs, rhs);
    std::printf("      local:    (a+b)*2 == 2*a + 2*b   -> %s [%s]\n",
                bridgeir::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Yes && ans.source == "local" &&
               hub.externalCalls() == before,
           "local decides without calling the connector");
  }

  // 2. Nonlinear identity: local is honest (Unknown), SymPy escalates to Yes.
  {
    const Expr lhs = Mul({Add({V("a"), V("b")}), Add({V("a"), V("b")})});
    const Expr rhs = Add({Mul({V("a"), V("a")}),
                          Mul({C(2), V("a"), V("b")}),
                          Mul({V("b"), V("b")})});
    const auto ans = hub.isEqual(facts, lhs, rhs);
    std::printf("      escalate: (a+b)*(a+b) == a*a+2*a*b+b*b -> %s [%s]\n",
                bridgeir::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Yes && ans.source == "connector@sympy",
           "sympy proves the nonlinear identity");

    const int calls = hub.externalCalls();
    const auto again = hub.isEqual(facts, lhs, rhs);
    expect(again.tri == Tri::Yes &&
               again.detail.find("cached") != std::string::npos &&
               hub.externalCalls() == calls,
           "second identical query is served from cache");
  }

  // 3. External refutation of an identity (Yes means "equal for all values").
  {
    const auto ans = hub.isEqual(facts, Add({Mul({V("a"), V("a")}), C(1)}),
                                 Mul({V("a"), V("a")}));
    std::printf("      escalate: a*a + 1 == a*a            -> %s [%s]\n",
                bridgeir::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::No && ans.source == "connector@sympy",
           "sympy refutes a*a + 1 == a*a");
  }

  // 4. Identity vs satisfiability: a*b == a holds at a = 0, so Unknown is the
  //    mathematically correct answer, not No.
  {
    const auto ans = hub.isEqual(facts, Mul({V("a"), V("b")}), V("a"));
    std::printf("      escalate: a*b == a                  -> %s [%s] "
                "(satisfiable at a=0)\n",
                bridgeir::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Unknown,
           "sympy keeps satisfiable-but-not-identical as Unknown");
  }

  if (failures == 0) std::printf("\nall connector tests passed\n");
  return failures == 0 ? 0 : 1;
}
