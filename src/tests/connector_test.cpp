// Connector test: local-first routing, SymPy escalation, content caching.
#include <cstdio>
#include <string>
#include <vector>

#include "connectors/connectors.h"
#include "symbolic/symbolic.h"

using brrr::connectors::Hub;
using brrr::symbolic::Expr;
using brrr::symbolic::ExprTerm;
using brrr::symbolic::Solver;
using brrr::symbolic::Tri;

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
  expect(hub.sympyAvailable(), "sympy connector available (python3 or python, with sympy)");

  const Solver facts;

  // 1. Local solver decides the affine fragment; connector untouched.
  {
    const Expr lhs = Mul({Add({V("a"), V("b")}), C(2)});
    const Expr rhs = Add({Mul({C(2), V("a")}), Mul({C(2), V("b")})});
    const int before = hub.externalCalls();
    const auto ans = hub.isEqual(facts, lhs, rhs);
    std::printf("      local:    (a+b)*2 == 2*a + 2*b   -> %s [%s]\n",
                brrr::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Yes && ans.source == "local" &&
               hub.externalCalls() == before,
           "local decides without calling the connector");
  }

  // 2. Nonlinear identities are local too now: the solver's polynomial normal
  //    form decides them, so the connector is not called.
  {
    const Expr lhs = Mul({Add({V("a"), V("b")}), Add({V("a"), V("b")})});
    const Expr rhs = Add({Mul({V("a"), V("a")}),
                          Mul({C(2), V("a"), V("b")}),
                          Mul({V("b"), V("b")})});
    const int before = hub.externalCalls();
    const auto ans = hub.isEqual(facts, lhs, rhs);
    std::printf("      local:    (a+b)*(a+b) == a*a+2*a*b+b*b -> %s [%s]\n",
                brrr::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Yes && ans.source == "local" && hub.externalCalls() == before,
           "the local solver proves the nonlinear identity");

    const auto no = hub.isEqual(facts, Add({Mul({V("a"), V("a")}), C(1)}), Mul({V("a"), V("a")}));
    std::printf("      local:    a*a + 1 == a*a            -> %s [%s]\n",
                brrr::symbolic::toString(no.tri), no.source.c_str());
    expect(no.tri == Tri::No && no.source == "local", "the local solver refutes a*a + 1 == a*a");
  }

  // 3. What the local solver leaves open escalates, and is cached by content.
  //    a*b == a holds at a = 0, so Unknown is the mathematically correct answer
  //    (an identity must hold for all values), not No.
  {
    const auto ans = hub.isEqual(facts, Mul({V("a"), V("b")}), V("a"));
    std::printf("      escalate: a*b == a                  -> %s [%s] "
                "(satisfiable at a=0)\n",
                brrr::symbolic::toString(ans.tri), ans.source.c_str());
    expect(ans.tri == Tri::Unknown && ans.source == "connector@sympy",
           "sympy keeps satisfiable-but-not-identical as Unknown");

    const int calls = hub.externalCalls();
    const auto again = hub.isEqual(facts, Mul({V("a"), V("b")}), V("a"));
    expect(again.detail.find("cached") != std::string::npos && hub.externalCalls() == calls,
           "a second identical query is served from the cache");
  }

  if (failures == 0) std::printf("\nall connector tests passed\n");
  return failures == 0 ? 0 : 1;
}
