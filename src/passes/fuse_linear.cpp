// Memory schedules for aten.linear, so the kernel stops allocating its
// intermediates. Both record a decision on the op; lower-aten realizes it (and
// re-checks it, see linear_chain.h).
//
// fuse-epilogue: fold the elementwise ops that consume a linear's result into
// its store. A relu, or an add/mul whose other operand already exists, that is
// the sole reader of the value before it, joins the chain:
//
//   %linear_1 = aten.linear %relu, %w, %b      epilogue="add"
//   %add = aten.add %x, %linear_1              <- folded: out[i, n] = x[i, n] + acc
//
// Only the chain's last value keeps a buffer (here the function's output), so
// every buffer before it is gone.
//
// fuse-rows: when a linear's input is another linear's (chain) result and
// nothing else reads it, compute the producer R rows at a time, inside the
// consumer's row loop, into an R x N scratch. Its full [B, N] buffer is gone and
// the rows it writes are read back while still in L1. Both layers must use the
// same row tile (tile-linear rows=R); there is nothing else to prove, since
// tile-linear proved R divides B.
#include "passes/linear_chain.h"
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;
using passes::Chain;
using passes::Dataflow;

std::vector<NodeId> funcsOf(const Module& m) {
  std::vector<NodeId> out;
  for (NodeId id : m.top_level) {
    if (const Node* n = m.find(id); n && n->kind == "func") out.push_back(id);
  }
  return out;
}

int64_t rowsOf(const Node& n) {
  const AttrValue* a = n.findAttr("rows");
  return a && a->kind == AttrValue::Kind::Int && a->i > 0 ? a->i : 1;
}

bool fuseEpilogue(Module& m, const passes::Context&, std::string*) {
  for (NodeId f : funcsOf(m)) {
    const Dataflow df(m, f);
    if (!df.straight) continue;
    std::set<NodeId> claimed;
    for (NodeId id : df.order) {
      const Node* n = m.find(id);
      if (n->kind != "aten.linear" || n->findAttr("epilogue")) continue;
      std::vector<std::string> steps;
      std::string folded, stopped;
      ValueRef cur{id, 0};
      for (;;) {
        size_t operand = 0;
        const NodeId u = df.soleUser(cur, &operand);
        if (u == kInvalidId || claimed.count(u)) break;
        std::string why;
        const std::string step = passes::epilogueStep(m, df, id, u, operand, &why);
        if (step.empty()) {
          const std::string& k = m.find(u)->kind;
          if (k == "aten.relu" || k == "aten.add" || k == "aten.mul") stopped = why;  // a near miss
          break;
        }
        steps.push_back(step);
        claimed.insert(u);
        folded += (folded.empty() ? "" : ", ") + ("n" + std::to_string(u) + " (" + m.find(u)->kind + ")");
        cur = {u, 0};
      }
      if (steps.empty()) {
        if (!stopped.empty()) m.log("fuse-epilogue", id, "not fused: " + stopped);
        continue;
      }
      std::string list;
      for (const std::string& s : steps) list += (list.empty() ? "" : ",") + s;
      m.find(id)->attrs["epilogue"] = AttrValue::fromStr(list);
      m.log("fuse-epilogue", id,
            "epilogue=" + list + ": " + folded + " folded into the store, " + std::to_string(steps.size()) +
                " buffer(s) gone" + (stopped.empty() ? "" : "; stopped at " + stopped));
    }
  }
  return true;
}

bool fuseRows(Module& m, const passes::Context& ctx, std::string* err) {
  for (NodeId f : funcsOf(m)) {
    const Dataflow df(m, f);
    if (!df.straight) continue;
    std::map<ValueRef, NodeId> producer;  // a linear chain's result -> the linear
    std::vector<NodeId> linears;
    for (NodeId id : df.order) {
      if (m.find(id)->kind != "aten.linear") continue;
      linears.push_back(id);
      Chain c;
      if (!passes::epilogueChain(m, df, id, &c, err)) return false;
      producer[c.final] = id;
    }
    sema::Sema sema(m, ctx.reg);
    for (NodeId id : linears) {
      const Node& n = *m.find(id);
      if (n.findAttr("fuse_rows") || n.operands.empty() || n.operands[0].kind != Operand::Kind::Value) continue;
      const ValueRef in = n.operands[0].value;
      const auto it = producer.find(in);
      size_t operand = 0;
      if (it == producer.end() || df.soleUser(in, &operand) != id || operand != 0) continue;
      const Node& p = *m.find(it->second);
      if (rowsOf(p) != rowsOf(n)) {
        *err = "n" + std::to_string(id) + " (aten.linear): its input comes from n" + std::to_string(p.id) +
               " (aten.linear) with rows=" + std::to_string(rowsOf(p)) + ", but it has rows=" +
               std::to_string(rowsOf(n)) + "; tile both with the same rows to fuse them";
        return false;
      }
      const Type t = sema.typeOf(in);
      std::string dims;
      for (const symbolic::Expr& d : t ? t->dims : std::vector<symbolic::Expr>{}) {
        dims += (dims.empty() ? "" : ", ") + sema.show(d);
      }
      m.find(id)->attrs["fuse_rows"] = AttrValue::fromBool(true);
      m.log("fuse-rows", id,
            "n" + std::to_string(p.id) + " (aten.linear) computed " + std::to_string(rowsOf(n)) +
                " row(s) at a time inside this row loop; its [" + dims + "] buffer is gone");
    }
  }
  return true;
}

}  // namespace

BRRR_PASS(fuse_epilogue, "fuse-epilogue",
          "fold relu / add / mul that consume an aten.linear into its store; their buffers are gone",
          fuseEpilogue);
BRRR_PASS(fuse_rows, "fuse-rows",
          "compute a linear feeding another linear R rows at a time inside the consumer's row loop; "
          "its full buffer is gone",
          fuseRows);
