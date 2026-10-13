#include "passes/fold_reshape.h"

#include <cstdio>
#include <string>
#include <vector>

#include "symbolic/expr_io.h"

namespace bridgeir {
namespace {

std::string attrString(const Node* n, const char* key) {
  const AttrValue* a = n ? n->findAttr(key) : nullptr;
  return a && a->kind == AttrValue::Kind::Str ? a->s : std::string();
}

}  // namespace

void foldLooseReshapePairs(Module& m, const symbolic::Solver& facts) {
  std::vector<NodeId> reshapes;
  for (const Node& n : m.nodes) {
    if (n.kind == "reshape") reshapes.push_back(n.id);
  }

  for (const NodeId rid : reshapes) {
    Node* r2 = m.find(rid);
    if (!r2 || r2->operands.empty()) continue;
    Node* r1 = m.find(r2->operands[0].id);
    if (!r1 || r1->kind != "reshape") continue;

    // Single-use check: composing replaces both reshapes.
    int uses = 0;
    for (const Node& n : m.nodes) {
      for (const ValueRef& vr : n.operands) {
        if (vr.id == r1->id) ++uses;
      }
    }
    if (uses > 1) {
      m.log("skip", rid, "producer reshape is multi-use");
      continue;
    }

    const NodeId r1_id = r1->id;
    const NodeId r2_id = r2->id;
    const NodeId scope = r1->scope;
    const NodeId src = r1->operands.empty() ? kInvalidId : r1->operands[0].id;
    const std::string in_s = attrString(r1, "in");
    const std::string out_s = attrString(r2, "out");

    std::string err;
    const symbolic::Expr in_e = symbolic::parseExpr(in_s, &err);
    if (!in_e) {
      m.log("skip", rid, "bad 'in' expression: " + err);
      continue;
    }
    const symbolic::Expr out_e = symbolic::parseExpr(out_s, &err);
    if (!out_e) {
      m.log("skip", rid, "bad 'out' expression: " + err);
      continue;
    }

    // The two first-hand queries the pass needs.
    const symbolic::Tri same = facts.isEqual(in_e, out_e);
    const symbolic::Tri aligned16 = facts.aligned(out_e, 16);
    std::fprintf(stderr,
                 "[fold-reshape] pair n%llu,n%llu: isEqual(%s, %s) -> %s; "
                 "aligned(%s, 16) -> %s\n",
                 static_cast<unsigned long long>(r1_id),
                 static_cast<unsigned long long>(r2_id), in_s.c_str(),
                 out_s.c_str(), symbolic::toString(same), out_s.c_str(),
                 symbolic::toString(aligned16));

    if (same == symbolic::Tri::Yes && aligned16 == symbolic::Tri::Yes) {
      AttrValue in_attr = *r1->findAttr("in");
      AttrValue out_attr = *r2->findAttr("out");
      Node& fused = m.insertNodeAfter(r2_id, "reshape", scope);
      fused.name = "r_fused";
      fused.attrs["in"] = in_attr;
      fused.attrs["out"] = out_attr;
      fused.operands.push_back({src, 0});
      const NodeId fused_id = fused.id;

      for (Node& n : m.nodes) {
        bool changed = false;
        for (ValueRef& vr : n.operands) {
          if (vr.id == r2_id) {
            vr.id = fused_id;
            changed = true;
          }
        }
        if (changed) {
          m.log("retarget", n.id, "operand -> n" + std::to_string(fused_id));
        }
      }
      m.log("compose", fused_id,
            "reshape n" + std::to_string(r1_id) + "+n" + std::to_string(r2_id) +
                " (in=" + in_s + " out=" + out_s + ")");
      continue;
    }

    const char* why = same == symbolic::Tri::No     ? "sizes not equal"
                      : same == symbolic::Tri::Unknown ? "size equality unknown"
                      : aligned16 == symbolic::Tri::No ? "output not aligned"
                                                       : "alignment unknown";
    m.log("keep", r2_id, std::string("fold-reshape: ") + why);
  }
}

}  // namespace bridgeir
