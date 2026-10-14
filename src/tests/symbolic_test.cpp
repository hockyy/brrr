// Symbolic inference prototype: "do these two symbols have the same value?"
// answered mathematically, with tri-state results and honest Unknowns.
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "symbolic/expr_io.h"
#include "symbolic/facts.h"
#include "symbolic/symbolic.h"
#include "symbolic/z3_backend.h"

using namespace brrr::symbolic;

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

  // 8. Products commute and distribute: numel([n, m]) == numel([m * n]).
  {
    Solver s;
    expect(s.isEqual(Mul({V("n"), V("m")}), Mul({V("m"), V("n")})) == Tri::Yes,
           "n * m == m * n (polynomial normal form)");
    expect(s.isEqual(Mul({V("n"), V("m")}), Add({V("n"), V("m")})) == Tri::Unknown,
           "n * m == n + m stays Unknown");
  }

  // 9. `a = b` between two atoms no longer ping-pongs in substitution.
  {
    Solver s;
    s.assertEqual(V("k"), V("n"));
    s.assertEqual(V("n"), C(64));
    expect(s.isEqual(V("k"), C(64)) == Tri::Yes, "k = n, n = 64  =>  k == 64");
    expect(s.isEqual(V("k"), V("n")) == Tri::Yes, "k == n after both are mapped");
  }

  // 10. A linear equation is solved for a unit-coefficient atom.
  {
    Solver s;
    s.assertEqual(Add({V("m"), C(1)}), V("n"));
    s.assertEqual(V("n"), C(10));
    expect(s.constantValue(V("m")) == std::optional<int64_t>(9), "m + 1 = n, n = 10  =>  m == 9");
  }

  // 11. A nonlinear assumption proves exactly itself (a guard on n * m = n + m).
  {
    Solver s;
    s.assertEqual(Mul({V("n"), V("m")}), Add({V("n"), V("m")}));
    expect(s.isEqual(Add({V("m"), V("n")}), Mul({V("m"), V("n")})) == Tri::Yes,
           "an unorientable equality still proves itself");
  }

  // 12. Facts: `0 <= n` is consistent when n is known positive (a v0 bug
  //     refuted it), chains apply every link, strict `<` is rejected.
  {
    Solver s;
    std::string err;
    expect(applyFact("fact range(n, 5, 10)", s, &err), "range(n, 5, 10)");
    expect(applyFact("fact 0 <= n", s, &err), "0 <= n is consistent with n in [5, 10]: " + err);
    expect(applyFact("fact 0 <= i <= n", s, &err) && s.lessOrEqual(V("i"), V("n")) == Tri::Yes,
           "a chain asserts every link");
    expect(!applyFact("fact i < n", s, &err), "strict < is rejected");
    expect(!applyFact("fact n = 11", s, &err), "n = 11 contradicts the range");
  }

  // 13. Expression text: subtraction, dim(%x, k), minimal parentheses.
  {
    std::string err;
    const Expr e = parseExpr("2 * (%n - 1) + dim(%x, 0)", &err);
    expect(e && printExpr(e) == "2 * (%n - 1) + dim(%x, 0)", "parse/print round trip: " +
                                                                  (e ? printExpr(e) : err));
    Solver s;
    expect(s.isEqual(e, parseExpr("dim(%x, 0) + 2 * %n - 2", &err)) == Tri::Yes,
           "2 * (%n - 1) + dim(%x, 0) == dim(%x, 0) + 2 * %n - 2");
  }

  // 14. Division by a positive constant: floor/ceil semantics on negatives,
  //     exact division, mod as e - c * floordiv(e, c), defining bounds.
  {
    std::string err;
    Solver s;
    auto E = [&](const char* t) { return parseExpr(t, &err); };
    expect(s.constantValue(E("-7 floordiv 2")) == std::optional<int64_t>(-4), "-7 floordiv 2 == -4");
    expect(s.constantValue(E("-7 ceildiv 2")) == std::optional<int64_t>(-3), "-7 ceildiv 2 == -3");
    expect(s.constantValue(E("-7 mod 2")) == std::optional<int64_t>(1), "-7 mod 2 == 1");
    expect(s.isEqual(E("(4 * n + 8) floordiv 4"), E("n + 2")) == Tri::Yes, "exact division");
    expect(s.isEqual(E("n mod 4"), E("n - 4 * (n floordiv 4)")) == Tri::Yes, "mod via floordiv");
    expect(s.isEqual(E("n ceildiv 4"), E("(n + 3) floordiv 4")) == Tri::Yes, "ceildiv via floordiv");
    expect(s.lessOrEqual(E("n"), E("(n ceildiv 32) * 32")) == Tri::Yes, "a ceildiv covers n");
    expect(s.lessOrEqual(E("(n floordiv 8) * 8"), E("n")) == Tri::Yes, "a floordiv stays below n");
    expect(s.isEqual(E("n floordiv 2"), E("n")) == Tri::Unknown, "n floordiv 2 == n is Unknown");
    expect(!parseExpr("n / 2", &err), "`/` is rejected");
    expect(!parseExpr("n floordiv m", &err), "division by a value is rejected");
    expect(printExpr(E("x * (y floordiv 3)")) == "x * (y floordiv 3)" &&
               printExpr(E("x * y floordiv 3")) == "(x * y) floordiv 3",
           "precedence survives printing");
  }

  // 15. Interval soundness (two bugs found 2026-10-14): a product of
  //     half-bounded intervals, and corner products that overflow.
  {
    Solver s;
    std::string err;
    applyFact("fact 0 <= n", s, &err);
    applyFact("fact 0 <= m", s, &err);
    expect(s.lessOrEqual(C(0), Mul({V("n"), V("m")})) == Tri::Yes,
           "[0, inf) * [0, inf) is non-negative");
    expect(s.lessOrEqual(Mul({V("n"), V("m")}), C(0)) == Tri::Unknown,
           "...and not bounded above (it was [0, 0])");
    Solver big;
    big.assertRange("x", INT64_MIN / 2, INT64_MAX / 2);
    expect(big.lessOrEqual(Mul({V("x"), V("x")}), C(0)) == Tri::Unknown,
           "an overflowing corner product makes the interval unknown, not partial");
  }

  // 16. With Z3 behind the solver, questions the local rules leave open are
  //     decided: transitivity needs a real decision procedure.
  if (backend() != Backend::Local) {
    Solver s;
    s.assertLe(V("x"), V("y"));
    s.assertLe(V("y"), V("z"));
    expect(s.lessOrEqual(V("x"), V("z")) == Tri::Yes, "z3: x <= y, y <= z  =>  x <= z");
    expect(s.lessOrEqual(V("z"), V("x")) == Tri::Unknown, "z3: z <= x stays Unknown");
    Solver t;
    t.assertEqual(Add({Mul({C(2), V("a")}), V("b")}), C(10));
    t.assertEqual(Add({V("a"), Mul({C(-1), V("b")})}), C(2));
    expect(t.constantValue(V("a")) == std::optional<int64_t>(4), "z3: 2a + b = 10, a - b = 2 => a == 4");
  } else {
    std::printf("      (built without Z3: z3 tests skipped)\n");
  }

  // 17. Determinism: a Z3 answer depends only on the facts and the question, not
  //     on machine load or on the questions asked before it; answers recorded in
  //     an AnswerCache are replayed.
  if (backend() == Backend::Z3) {
    // A question only Z3 decides (nonlinear): x * y = 6, 1 <= x <= 2, y >= 0
    // imply y >= 3. Below, (x, z) name the question's two sides.
    Solver s;
    s.assertEqual(Mul({V("x"), V("y")}), C(6));
    s.assertRange("x", 1, 2);
    s.assertLe(C(0), V("y"));
    auto ask = [&]() { return s.lessOrEqual(C(3), V("y")); };
    expect(ask() == Tri::Yes, "determinism: asked once");
    const uint64_t first = z3::stats().last_rlimit_used;
    for (int i = 0; i < 20; ++i) {
      Solver o;
      o.assertLe(V("p"), C(i));
      o.assertEqual(Mul({V("q"), V("q")}), Add({V("p"), C(i)}));
      (void)o.lessOrEqual(V("p"), C(i + 1));
      (void)o.constantValue(V("q"));
    }
    expect(ask() == Tri::Yes, "determinism: asked again");
    std::printf("      (rlimit used: first %llu, again %llu)\n", (unsigned long long)first,
                (unsigned long long)z3::stats().last_rlimit_used);
    expect(first > 0 && z3::stats().last_rlimit_used == first,
           "the same question costs the same rlimit, whatever was asked before");

    const uint64_t saved = z3::rlimit();
    z3::setRlimit(1);
    bool all_unknown = true;
    for (int i = 0; i < 5; ++i) all_unknown &= ask() == Tri::Unknown;
    z3::setRlimit(saved);
    expect(all_unknown, "a limit too small for the work: Unknown, every time");
    expect(ask() == Tri::Yes, "the default limit decides it again");

    const std::string path =
        (std::filesystem::temp_directory_path() / "brrr_answers_test.txt").string();
    std::filesystem::remove(path);
    {
      AnswerCache cache;
      setAnswerCache(&cache);
      expect(ask() == Tri::Yes, "recorded: the first answer is asked");
      expect(ask() == Tri::Yes && cache.asked() == 1 &&
                 cache.replayed() == 1,
             "recorded: the second is replayed");
      expect(cache.appendNew(path), "recorded: written to the store");
      setAnswerCache(nullptr);
    }
    {
      std::ifstream in(path, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      std::string text = ss.str();
      const size_t at = text.find(" yes");
      if (at != std::string::npos) text.replace(at, 4, " no");
      std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
      AnswerCache poisoned;
      poisoned.load(path);
      setAnswerCache(&poisoned);
      expect(ask() == Tri::No,
             "recorded: a replayed answer wins (poisoned on purpose)");
      setAnswerCache(nullptr);
      Solver more = s;
      more.assertLe(V("y"), C(100));
      setAnswerCache(&poisoned);
      expect(more.lessOrEqual(C(3), V("y")) == Tri::Yes,
             "recorded: a new fact makes a new question");
      setAnswerCache(nullptr);
    }
    std::filesystem::remove(path);
  }

  // 18. The local tiers against Z3 on random linear systems with divisions by
  //     constants (fixed seed). Soundness: every Yes/No agrees and every settled
  //     Unknown is Unknown for Z3 too. Completeness: on small division-free
  //     systems whatever Z3 decides the local tiers decide; on bigger ones the
  //     exact tier may give up (size cap) and Z3 answers, which is reported.
  if (backend() == Backend::Z3) {
    uint64_t seed = 0x5eed;
    auto rnd = [&](int lo, int hi) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      return lo + static_cast<int>((seed >> 33) % static_cast<uint64_t>(hi - lo + 1));
    };
    const char* names[] = {"a", "b", "c", "d"};
    bool with_div = false;  // the current trial uses a division
    auto randLin = [&](int nv) {
      std::vector<Expr> terms{C(rnd(-6, 6))};
      for (int i = 0; i < nv; ++i) {
        if (const int k = rnd(-3, 3)) terms.push_back(Mul({C(k), V(names[i])}));
      }
      Expr e = Add(terms);
      const int shape = rnd(0, 5);
      with_div = with_div || shape <= 2;
      switch (shape) {
        case 0: e = ExprTerm::div(ExprTerm::Kind::FloorDiv, e, rnd(2, 5)); break;
        case 1: e = ExprTerm::div(ExprTerm::Kind::Mod, e, rnd(2, 5)); break;
        case 2:
          e = Add({e, ExprTerm::div(ExprTerm::Kind::CeilDiv, V(names[rnd(0, nv - 1)]), rnd(2, 4))});
          break;
        default: break;
      }
      return e;
    };
    int trials = 0, decided = 0, clashes = 0, overclaims = 0, gaps = 0, small_gaps = 0;
    const uint64_t saved_rlimit = z3::rlimit();
    z3::setRlimit(500000);  // small systems; keeps the test fast
    for (int t = 0; t < 400; ++t) {
      const int nv = rnd(2, 4);
      with_div = false;
      Solver s;
      z3::Session z;
      std::string facts;  // printed when the trial fails
      for (int f = rnd(1, 4); f > 0; --f) {
        const Expr x = randLin(nv), y = randLin(nv);
        if (rnd(0, 3) == 0) {
          s.assertEqual(x, y);
          z.assertEqual(x, y);
          facts += "  fact " + printExpr(x) + " == " + printExpr(y) + "\n";
        } else {
          s.assertLe(x, y);
          z.assertLe(x, y);
          facts += "  fact " + printExpr(x) + " <= " + printExpr(y) + "\n";
        }
      }
      for (int i = 0; i < nv; ++i) {
        if (rnd(0, 1)) continue;
        const int lo = rnd(-10, 0), hi = rnd(1, 20);
        s.assertRange(names[i], lo, hi);
        z.assertRange(names[i], lo, hi);
        facts += "  fact " + std::to_string(lo) + " <= " + names[i] + " <= " + std::to_string(hi) + "\n";
      }
      // Under contradictory facts every answer holds vacuously; skip them.
      if (z.lessOrEqual(C(0), C(-1)) == Tri::Yes) continue;
      const Expr a = randLin(nv), b = randLin(nv);
      bool settled = false;
      Tri ours = Tri::Unknown, ref = Tri::Unknown;
      std::string question;
      switch (rnd(0, 3)) {
        case 0:
          ours = s.localTiers('=', a, b, 0, &settled);
          ref = z.isEqual(a, b);
          question = printExpr(a) + " == " + printExpr(b);
          break;
        case 1:
          ours = s.localTiers('l', a, b, 0, &settled);
          ref = z.lessOrEqual(a, b);
          question = printExpr(a) + " <= " + printExpr(b);
          break;
        case 2:
          ours = s.localTiers('<', a, b, 0, &settled);
          ref = z.isLess(a, b);
          question = printExpr(a) + " < " + printExpr(b);
          break;
        default: {
          const int m = rnd(2, 6);
          ours = s.localTiers('a', a, nullptr, m, &settled);
          ref = z.aligned(a, m);
          question = "aligned(" + printExpr(a) + ", " + std::to_string(m) + ")";
        }
      }
      const bool bad = (ours != Tri::Unknown && ref != Tri::Unknown && ours != ref) ||
                       (settled && ref != Tri::Unknown) ||
                       (ours == Tri::Unknown && ref != Tri::Unknown && nv <= 3 && !with_div);
      if (bad && clashes + overclaims + small_gaps < 6) {
        std::printf("      fuzz failure: %s -> local %s%s, z3 %s\n%s", question.c_str(),
                    toString(ours), settled ? " (settled)" : "", toString(ref), facts.c_str());
      }
      ++trials;
      decided += ref != Tri::Unknown;
      clashes += ours != Tri::Unknown && ref != Tri::Unknown && ours != ref;
      overclaims += settled && ref != Tri::Unknown;
      gaps += ours == Tri::Unknown && ref != Tri::Unknown;
      small_gaps += ours == Tri::Unknown && ref != Tri::Unknown && nv <= 3 && !with_div;
    }
    z3::setRlimit(saved_rlimit);
    std::printf("      (fuzz: %d questions, %d decided by Z3; clashes %d, overclaims %d, "
                "left to Z3 %d, of them small %d)\n",
                trials, decided, clashes, overclaims, gaps, small_gaps);
    expect(clashes == 0, "fuzz: the local tiers never contradict Z3");
    expect(overclaims == 0, "fuzz: a settled Unknown is Unknown for Z3 too");
    expect(small_gaps == 0, "fuzz: on small division-free systems the local tiers are complete");
  }

  if (failures == 0) std::printf("\nall symbolic tests passed\n");
  return failures == 0 ? 0 : 1;
}
