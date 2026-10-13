// Symbolic inference prototype: "do these two symbols have the same value?"
// answered mathematically, with tri-state results and honest Unknowns.
#include <cstdio>
#include <string>

#include "symbolic/symbolic.h"

using namespace bridgeir::symbolic;

static int failures = 0;

static void expect(bool cond, const std::string& what) {
  std::printf("%s %s\n", cond ? "ok:  " : "FAIL:", what.c_str());
  if (!cond) ++failures;
}

static void show(const char* q, Tri t) {
  std::printf("      %s -> %s\n", q, toString(t));
}

static Expr C(int64_t c) { return ExprTerm::constant(c); }
static Expr V(const char* n) { return ExprTerm::var(n); }
static Expr Add(std::vector<Expr> xs) { return ExprTerm::add(std::move(xs)); }
static Expr Mul(std::vector<Expr> xs) { return ExprTerm::mul(std::move(xs)); }

int main() {
  // 1. Polynomial identities: (a + b) * 2 == 2*a + 2*b
  {
    Solver s;
    const Expr lhs = Mul({Add({V("a"), V("b")}), C(2)});
    const Expr rhs = Add({Mul({C(2), V("a")}), Mul({C(2), V("b")})});
    show("(a+b)*2 == 2*a + 2*b", s.isEqual(lhs, rhs));
    expect(s.isEqual(lhs, rhs) == Tri::Yes, "polynomial identity proved");
  }

  // 2. Substitution + constant folding: N = 128; M = N + 32  =>  M == 160
  {
    Solver s;
    s.assertEqual(V("N"), C(128));
    s.assertEqual(V("M"), Add({V("N"), C(32)}));
    show("M = N + 32, N = 128  =>  M == 160", s.isEqual(V("M"), C(160)));
    expect(s.isEqual(V("M"), C(160)) == Tri::Yes, "substitution folds to 160");
    auto [lo, hi] = s.bounds(V("M"));
    std::printf("      bounds(M) -> [%lld, %lld]\n",
                static_cast<long long>(lo.value_or(-1)),
                static_cast<long long>(hi.value_or(-1)));
    expect(lo == 160 && hi == 160, "bounds(M) is [160, 160]");
  }

  // 3. Tri-state honesty: unprovable equality is Unknown, not No.
  {
    Solver s;
    show("a == b (unconstrained)", s.isEqual(V("a"), V("b")));
    show("a + 1 == a", s.isEqual(Add({V("a"), C(1)}), V("a")));
    expect(s.isEqual(V("a"), V("b")) == Tri::Unknown,
           "unconstrained equality is Unknown");
    expect(s.isEqual(Add({V("a"), C(1)}), V("a")) == Tri::No,
           "constant difference proves inequality");
  }

  // 4. Bounds: N = 1024; 0 <= i < N;  prove i < N without enumerating values.
  {
    Solver s;
    s.assertEqual(V("N"), C(1024));
    s.assertRange("i", 0, 1023);
    show("i < N   (N = 1024, i in [0, 1023])", s.isLess(V("i"), V("N")));
    show("N < i", s.isLess(V("N"), V("i")));
    show("i < 5", s.isLess(V("i"), C(5)));
    expect(s.isLess(V("i"), V("N")) == Tri::Yes, "bounds prove i < N");
    expect(s.isLess(V("N"), V("i")) == Tri::No, "bounds refute N < i");
    expect(s.isLess(V("i"), C(5)) == Tri::Unknown,
           "out-of-range comparison is Unknown");
  }

  // 5. Alignment (vectorization legality): N = 0 (mod 16); M = N + 32;
  //    any 4*k is 0 (mod 4) without knowing k.
  {
    Solver s;
    s.assertMod("N", 16, 0);
    s.assertEqual(V("M"), Add({V("N"), C(32)}));
    show("aligned(M, 16)  with N = 0 (mod 16), M = N + 32",
         s.aligned(V("M"), 16));
    show("aligned(N + 1, 16)", s.aligned(Add({V("N"), C(1)}), 16));
    show("aligned(4*i, 4)", s.aligned(Mul({C(4), V("i")}), 4));
    show("aligned(2*i + 2*j, 2)", s.aligned(Add({Mul({C(2), V("i")}), Mul({C(2), V("j")})}), 2));
    show("aligned(2*i + 1, 2)", s.aligned(Add({Mul({C(2), V("i")}), C(1)}), 2));
    expect(s.aligned(V("M"), 16) == Tri::Yes, "M inherits alignment from N");
    expect(s.aligned(Add({V("N"), C(1)}), 16) == Tri::No,
           "offset by 1 breaks alignment");
    expect(s.aligned(Mul({C(4), V("i")}), 4) == Tri::Yes,
           "coefficient proves alignment without knowing i");
    expect(s.aligned(Add({Mul({C(2), V("i")}), Mul({C(2), V("j")})}), 2) ==
               Tri::Yes,
           "sum of aligned terms stays aligned");
    expect(s.aligned(Add({Mul({C(2), V("i")}), C(1)}), 2) == Tri::No,
           "odd offset breaks alignment");
  }

  // 6. Pass usage sketch: FoldReshape/ComposeReshapeLoosely-style decision.
  //    Grounded in bishengir CanonicalizeTensorReshape.cpp, which admits:
  //    "Without divisibility API, parting can only assume the value is the same"
  //    (and hand-rolls getStaticTotalMult, ignoring dynamic dims).
  //    With facts, the pass asks first-hand: Yes -> compose, No -> keep,
  //    Unknown -> conservative (runtime guard), never "assume".
  {
    Solver s;
    s.assertEqual(V("src_size"), C(512));
    s.assertEqual(V("dst_size"), C(512));
    s.assertMod("dst_size", 16, 0);
    const Tri same = s.isEqual(V("src_size"), V("dst_size"));
    const Tri div16 = s.aligned(V("dst_size"), 16);
    std::printf("      fold-reshape: same=%s aligned16=%s -> %s\n",
                toString(same), toString(div16),
                same == Tri::Yes && div16 == Tri::Yes ? "compose" : "keep");
    expect(same == Tri::Yes && div16 == Tri::Yes,
           "fold-reshape: facts allow composition");
  }
  {
    Solver s;
    s.assertEqual(V("src_size"), C(512));
    s.assertEqual(V("dst_size"), C(384));
    const Tri same = s.isEqual(V("src_size"), V("dst_size"));
    std::printf("      fold-reshape: 512 vs 384 -> %s (keep)\n", toString(same));
    expect(same == Tri::No, "fold-reshape: size mismatch keeps the reshapes");
  }
  {
    Solver s;  // dynamic dims, no facts: honest Unknown, not an assumption
    const Tri same = s.isEqual(V("src_size"), V("dst_size"));
    std::printf("      fold-reshape: unknown sizes -> %s (runtime guard)\n",
                toString(same));
    expect(same == Tri::Unknown,
           "fold-reshape: unknowns fall back instead of assuming");
  }

  if (failures == 0) std::printf("\nall symbolic tests passed\n");
  return failures == 0 ? 0 : 1;
}
