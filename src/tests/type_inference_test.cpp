// Type-inference prototype: schema signatures as data + unification solving.
#include <cstdio>
#include <string>
#include <vector>

#include "types/infer.h"

using bridgeir::types::InferNode;
using bridgeir::types::Signature;
using bridgeir::types::TypeRef;
using bridgeir::types::TypeTerm;

static int failures = 0;

static void expect(bool cond, const std::string& what) {
  std::printf("%s %s\n", cond ? "ok:  " : "FAIL:", what.c_str());
  if (!cond) ++failures;
}

static TypeRef named(const std::string& n) {
  return TypeTerm::makeName(n, {});
}
static TypeRef tensor(std::vector<TypeRef> xs) {
  return TypeTerm::makeName("tensor", std::move(xs));
}
static TypeRef dim(const std::string& n) { return named(n); }

static std::vector<Signature> stdlib() {
  // mm : tensor<A, M, K> x tensor<A, K, N> -> tensor<f32, M, N>
  //      with params M=0, K=1, N=2, A=3 — one line of data.
  Signature mm;
  mm.kind = "mm";
  mm.type_params = 4;
  mm.operand_types = {
      tensor({TypeTerm::makeVar(3), TypeTerm::makeVar(0), TypeTerm::makeVar(1)}),
      tensor({TypeTerm::makeVar(3), TypeTerm::makeVar(1), TypeTerm::makeVar(2)})};
  mm.result_type =
      tensor({named("f32"), TypeTerm::makeVar(0), TypeTerm::makeVar(2)});

  // add : T x T -> T
  Signature add;
  add.kind = "add";
  add.type_params = 1;
  add.operand_types = {TypeTerm::makeVar(0), TypeTerm::makeVar(0)};
  add.result_type = TypeTerm::makeVar(0);

  return {mm, add};
}

int main() {
  const std::vector<Signature> sigs = stdlib();

  // 1. Matmul dims/dtype propagate through unification.
  {
    InferNode a{0, "param", {},
                tensor({named("f16"), dim("64"), dim("128")})};
    InferNode b{1, "param", {},
                tensor({named("f16"), dim("128"), dim("256")})};
    InferNode m{2, "mm", {0, 1}, nullptr};
    auto r = infer({a, b, m}, sigs);
    expect(r.ok, "mm: inference succeeds");
    for (const auto& e : r.equations) std::printf("      %s\n", e.c_str());
    expect(structurallyEqual(
               r.types[2],
               tensor({named("f32"), dim("64"), dim("256")})),
           "mm: result is tensor<f32, 64, 256>");
  }

  // 2. Dimension mismatch produces the failing equation, with the path.
  {
    InferNode a{0, "param", {},
                tensor({named("f16"), dim("64"), dim("128")})};
    InferNode b{1, "param", {},
                tensor({named("f16"), dim("100"), dim("256")})};
    InferNode m{2, "mm", {0, 1}, nullptr};
    auto r = infer({a, b, m}, sigs);
    expect(!r.ok, "mm: dim mismatch rejected");
    std::printf("      %s\n", r.error.c_str());
    expect(r.error.find("cannot unify") != std::string::npos &&
               r.error.find("100") != std::string::npos &&
               r.error.find("128") != std::string::npos,
           "mm: error names both dimensions");
  }

  // 3. Polymorphic elementwise: T x T -> T.
  {
    InferNode x{0, "param", {}, named("f32")};
    InferNode y{1, "param", {}, named("f32")};
    InferNode s{2, "add", {0, 1}, nullptr};
    auto r = infer({x, y, s}, sigs);
    expect(r.ok && structurallyEqual(r.types[2], named("f32")),
           "add: f32 x f32 -> f32");
  }
  {
    InferNode x{0, "param", {}, named("f32")};
    InferNode y{1, "param", {}, named("f16")};
    InferNode s{2, "add", {0, 1}, nullptr};
    auto r = infer({x, y, s}, sigs);
    expect(!r.ok, "add: mixed dtypes rejected");
    std::printf("      %s\n", r.error.c_str());
  }

  // 4. Symbolic dims survive inference as variables.
  {
    InferNode a{0, "param", {},
                tensor({named("f16"), TypeTerm::makeVar(7), dim("128")})};
    InferNode b{1, "param", {},
                tensor({named("f16"), dim("128"), TypeTerm::makeVar(8)})};
    InferNode m{2, "mm", {0, 1}, nullptr};
    auto r = infer({a, b, m}, sigs);
    expect(r.ok, "mm: symbolic shapes accepted");
    for (const auto& e : r.equations) std::printf("      %s\n", e.c_str());
    expect(r.types[2]->args.size() == 3 &&
               r.types[2]->args[1]->kind == TypeTerm::Kind::Var &&
               r.types[2]->args[2]->kind == TypeTerm::Kind::Var,
           "mm: result keeps symbolic dims as variables");
  }

  if (failures == 0) std::printf("\nall type-inference tests passed\n");
  return failures == 0 ? 0 : 1;
}
