// lower-aten: tensor-level ATen ops (from the PyTorch frontend, examples/kernel.py)
// to loop nests over buffers, so the kernel can be compiled (to-llvm).
//
//   signature   tensor arguments become buffers with the same dims; each tensor
//               result becomes an output buffer argument (destination-passing)
//               and the function returns nothing
//   values      the result that is returned is computed straight into the output
//               buffer; every other result gets an alloc, freed at the end
//   ops         aten.linear: for i < B, n < N: acc = b[n]; for k < K: acc +=
//               x[i, k] * w[n, k]; out[i, n] = acc.   aten.relu, aten.add:
//               elementwise loops over every dim
//   tiles       an aten.linear scheduled by tile-linear (attrs rows=R cols=C) is
//               register-blocked: per R x C tile of outputs, one k loop carries
//               R * C accumulators, and each loaded x[i, k] / w[n, k] is reused
//               C / R times. Tile indices are expressions (R * %ib + r).
//               form=inner (default): each output is a dot product along k
//               (vectorizes along k only with reassoc). form=outer: the weights
//               are transposed once into w_t[k, n], and the C outputs of a row
//               read C contiguous weights per k, so they vectorize across
//               columns, every output still summed in order (exact rounding).
//
// Every loop header states its range as a fact (Builder::countedLoop), so every
// load and store is proven in bounds. An ATen op without a lowering fails the
// pass, naming it; the run store then rolls the stage back.
#include "ir/builder.h"
#include "passes/pass.h"
#include "sema/sema.h"

namespace {

using namespace brrr;
using symbolic::Expr;
using symbolic::ExprTerm;

Type bufferOf(const Type& t) {
  return std::make_shared<TypeTerm>(TypeTerm{"buf", true, t->dims, t->elem});
}

Expr atom(ValueRef v) { return ExprTerm::var(valueAtom(v)); }

Operand at(const std::vector<ValueRef>& ivs) {
  std::vector<Expr> es;
  for (const ValueRef& v : ivs) es.push_back(atom(v));
  return Operand::ofList(std::move(es));
}

// t * block + r: the r-th index inside tile `block` of size t.
Expr tileIndex(ValueRef block, int64_t t, int64_t r) {
  Expr e = t == 1 ? atom(block) : ExprTerm::mul({ExprTerm::constant(t), atom(block)});
  return r ? ExprTerm::add({e, ExprTerm::constant(r)}) : e;
}

// How many tiles of size t fit in d (t divides d: tile-linear proved it).
Expr tiles(const Expr& d, int64_t t) {
  if (t == 1) return d;
  if (d->kind == ExprTerm::Kind::Const) return ExprTerm::constant(d->c / t);
  return ExprTerm::div(ExprTerm::Kind::FloorDiv, d, t);
}

int64_t intAttr(const Node& n, const char* key, int64_t fallback) {
  const AttrValue* a = n.findAttr(key);
  return a && a->kind == AttrValue::Kind::Int && a->i > 0 ? a->i : fallback;
}

class Lowering {
 public:
  Lowering(Module& m, NodeId func, sema::Sema& sema) : m_(m), func_(func), sema_(sema), b_(m, func) {}

  bool run(std::string* err) {
    std::vector<NodeId> ops, args;
    NodeId ret = kInvalidId;
    for (NodeId c : m_.childrenOf(func_)) {
      const Node* n = m_.find(c);
      if (n->kind == "arg") args.push_back(c);
      else if (n->kind == "return") ret = c;
      else if (n->kind.rfind("aten.", 0) == 0) ops.push_back(c);
      else if (n->kind == "block") return fail(err, *n, "lower-aten handles straight-line functions only");
    }
    if (ops.empty()) return true;

    // Types first, while the tensor ops are still there to ask.
    std::map<ValueRef, Type> types;
    for (NodeId id : ops) types[{id, 0}] = sema_.typeOf({id, 0});
    for (NodeId id : args) types[{id, 0}] = sema_.typeOf({id, 0});

    // The signature: tensors become buffers; the result an output buffer.
    for (NodeId id : args) {
      const Type t = types[{id, 0}];
      if (t && t->shaped) m_.setType({id, 0}, bufferOf(t));
      buffer_[{id, 0}] = {id, 0};
    }
    Node* fn = m_.find(func_);
    const Node* r = m_.find(ret);
    ValueRef result{};
    if (r && r->operands.size() == 1 && r->operands[0].kind == Operand::Kind::Value) {
      result = r->operands[0].value;
    }
    if (!fn->types.empty()) {
      if (fn->types.size() != 1 || !fn->types[0] || !fn->types[0]->shaped || !result.id) {
        return fail(err, *fn, "lower-aten expects one tensor result");
      }
      Node out;
      out.kind = "arg";
      out.scope = func_;
      out.names = {b_.uniqueName("out")};
      out.types = {bufferOf(fn->types[0])};
      const NodeId oid = m_.insertAfter(args.back(), std::move(out)).id;
      m_.log("lower-aten", oid, "result -> output buffer argument");
      fn = m_.find(func_);
      fn->types.clear();
      output_ = {oid, 0};
    }

    // Destinations: the returned value is written into the output; the rest get allocs.
    b_.at(func_);
    std::vector<ValueRef>& temps = temps_;
    for (NodeId id : ops) {
      const ValueRef v{id, 0};
      if (v == result && output_.id) {
        buffer_[v] = output_;
        continue;
      }
      const Type t = types[v];
      if (!t || !t->shaped) return fail(err, *m_.find(id), "a result without a tensor type");
      const Node* n = m_.find(id);
      buffer_[v] = b_.op("alloc", {}, bufferOf(t),
                         (n->names.empty() || n->names[0].empty() ? "tmp" : n->names[0]) + "_buf");
      temps.push_back(buffer_[v]);
    }

    for (NodeId id : ops) {
      const Node n = *m_.find(id);
      if (n.kind == "aten.linear") linear(n, types);
      else if (n.kind == "aten.relu") elementwise(n, types, "relu");
      else if (n.kind == "aten.add") elementwise(n, types, "add");
      else if (n.kind == "aten.mul") elementwise(n, types, "mul");
      else return fail(err, n, "no lowering for '" + n.kind + "' yet");
    }

    for (const ValueRef& t : temps) b_.effect("dealloc", {Operand::ofValue(t)});
    b_.effect("return", {});
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) m_.erase(*it);
    if (ret) m_.erase(ret);
    m_.log("lower-aten", func_, std::to_string(ops.size()) + " ATen op(s) lowered to loops");
    return true;
  }

 private:
  bool fail(std::string* err, const Node& n, const std::string& msg) {
    *err = "n" + std::to_string(n.id) + " (" + n.kind + "): " + msg;
    return false;
  }

  ValueRef bufferFor(const Operand& o) {
    return o.kind == Operand::Kind::Value ? buffer_.at(o.value) : ValueRef{};
  }

  // A register-blocked linear layer: per tile (ib, nb), R * C accumulators
  // start at the bias, one k loop updates them all, and they are stored at the end.
  void linearTiled(const Node& n, std::map<ValueRef, Type>& types, int64_t R, int64_t C) {
    const ValueRef x = bufferFor(n.operands[0]), w = bufferFor(n.operands[1]),
                   bias = bufferFor(n.operands[2]), out = buffer_.at({n.id, 0});
    const Type xt = types[n.operands[0].value], wt = types[n.operands[1].value];
    const Type elem = Builder::scalar(xt->elem);
    const std::string base = n.names.empty() || n.names[0].empty() ? "linear" : n.names[0];
    const AttrValue* form = n.findAttr("form");
    const bool outer = form && form->kind == AttrValue::Kind::Str && form->s == "outer";
    ValueRef wsrc = w;  // what the k loop reads: w[n, k], or w_t[k, n] for form=outer
    if (outer) {
      const Type wtt = std::make_shared<TypeTerm>(TypeTerm{"buf", true, {wt->dims[1], wt->dims[0]}, wt->elem});
      wsrc = b_.op("alloc", {}, wtt, base + "_wt");
      temps_.push_back(wsrc);
      b_.countedLoop(base + "_tk", wt->dims[1], {}, [&](ValueRef k, const std::vector<ValueRef>&) {
        b_.countedLoop(base + "_tn", wt->dims[0], {}, [&](ValueRef j, const std::vector<ValueRef>&) {
          const ValueRef v = b_.op("load", {Operand::ofValue(w), at({j, k})}, elem, base + "_wv");
          b_.effect("store", {Operand::ofValue(v), Operand::ofValue(wsrc), at({k, j})});
          return std::vector<ValueRef>{};
        });
        return std::vector<ValueRef>{};
      });
    }
    b_.countedLoop(base + "_ib", tiles(xt->dims[0], R), {}, [&](ValueRef ib, const std::vector<ValueRef>&) {
      b_.countedLoop(base + "_nb", tiles(wt->dims[0], C), {}, [&](ValueRef nb, const std::vector<ValueRef>&) {
        std::vector<ValueRef> bvals;
        for (int64_t c = 0; c < C; ++c) {
          bvals.push_back(b_.op("load", {Operand::ofValue(bias), Operand::ofList({tileIndex(nb, C, c)})},
                                elem, base + "_b"));
        }
        std::vector<std::pair<std::string, ValueRef>> carried;
        for (int64_t r = 0; r < R; ++r) {
          for (int64_t c = 0; c < C; ++c) carried.push_back({base + "_acc", bvals[c]});
        }
        const std::vector<ValueRef> acc = b_.countedLoop(
            base + "_k", xt->dims[1], carried, [&](ValueRef k, const std::vector<ValueRef>& a) {
              std::vector<ValueRef> xs, ws, next;
              for (int64_t r = 0; r < R; ++r) {
                xs.push_back(b_.op("load", {Operand::ofValue(x), Operand::ofList({tileIndex(ib, R, r), atom(k)})},
                                   elem, base + "_x"));
              }
              for (int64_t c = 0; c < C; ++c) {
                ws.push_back(outer ? b_.op("load", {Operand::ofValue(wsrc),
                                                    Operand::ofList({atom(k), tileIndex(nb, C, c)})},
                                           elem, base + "_w")
                                   : b_.op("load", {Operand::ofValue(wsrc),
                                                    Operand::ofList({tileIndex(nb, C, c), atom(k)})},
                                           elem, base + "_w"));
              }
              for (int64_t r = 0; r < R; ++r) {
                for (int64_t c = 0; c < C; ++c) {
                  const ValueRef p = b_.op("mul", {Operand::ofValue(xs[r]), Operand::ofValue(ws[c])}, elem,
                                           base + "_p");
                  next.push_back(b_.op("add", {Operand::ofValue(a[r * C + c]), Operand::ofValue(p)}, elem,
                                       base + "_sum"));
                }
              }
              return next;
            });
        for (int64_t r = 0; r < R; ++r) {
          for (int64_t c = 0; c < C; ++c) {
            b_.effect("store", {Operand::ofValue(acc[r * C + c]), Operand::ofValue(out),
                                Operand::ofList({tileIndex(ib, R, r), tileIndex(nb, C, c)})});
          }
        }
        return std::vector<ValueRef>{};
      });
      return std::vector<ValueRef>{};
    });
  }

  // out[i, n] = b[n] + sum_k x[i, k] * w[n, k]
  void linear(const Node& n, std::map<ValueRef, Type>& types) {
    const int64_t R = intAttr(n, "rows", 1), C = intAttr(n, "cols", 1);
    if (R > 1 || C > 1) return linearTiled(n, types, R, C);
    const ValueRef x = bufferFor(n.operands[0]), w = bufferFor(n.operands[1]),
                   bias = bufferFor(n.operands[2]), out = buffer_.at({n.id, 0});
    const Type xt = types[n.operands[0].value], wt = types[n.operands[1].value];
    const Type elem = Builder::scalar(xt->elem);
    const std::string base = n.names.empty() || n.names[0].empty() ? "linear" : n.names[0];
    b_.countedLoop(base + "_i", xt->dims[0], {}, [&](ValueRef i, const std::vector<ValueRef>&) {
      b_.countedLoop(base + "_n", wt->dims[0], {}, [&](ValueRef j, const std::vector<ValueRef>&) {
        const ValueRef b0 = b_.op("load", {Operand::ofValue(bias), at({j})}, elem, base + "_b");
        const std::vector<ValueRef> acc = b_.countedLoop(
            base + "_k", xt->dims[1], {{base + "_acc", b0}},
            [&](ValueRef k, const std::vector<ValueRef>& carried) {
              const ValueRef xv = b_.op("load", {Operand::ofValue(x), at({i, k})}, elem, base + "_x");
              const ValueRef wv = b_.op("load", {Operand::ofValue(w), at({j, k})}, elem, base + "_w");
              const ValueRef p = b_.op("mul", {Operand::ofValue(xv), Operand::ofValue(wv)}, elem, base + "_p");
              return std::vector<ValueRef>{
                  b_.op("add", {Operand::ofValue(carried[0]), Operand::ofValue(p)}, elem, base + "_sum")};
            });
        b_.effect("store", {Operand::ofValue(acc[0]), Operand::ofValue(out), at({i, j})});
        return std::vector<ValueRef>{};
      });
      return std::vector<ValueRef>{};
    });
  }

  // out[ivs] = op(in0[ivs], in1[ivs]...) over every dim of the result.
  void elementwise(const Node& n, std::map<ValueRef, Type>& types, const std::string& scalar_op) {
    const ValueRef out = buffer_.at({n.id, 0});
    const Type rt = types[{n.id, 0}];
    const Type elem = Builder::scalar(rt->elem);
    const std::string base = n.names.empty() || n.names[0].empty() ? scalar_op : n.names[0];
    std::vector<ValueRef> ivs;
    std::function<void(size_t)> nest = [&](size_t d) {
      if (d == rt->dims.size()) {
        std::vector<Operand> vals;
        for (const Operand& o : n.operands) {
          vals.push_back(Operand::ofValue(
              b_.op("load", {Operand::ofValue(bufferFor(o)), at(ivs)}, elem, base + "_in")));
        }
        const ValueRef r = b_.op(scalar_op, std::move(vals), elem, base + "_v");
        b_.effect("store", {Operand::ofValue(r), Operand::ofValue(out), at(ivs)});
        return;
      }
      b_.countedLoop(base + "_d" + std::to_string(d), rt->dims[d], {},
                     [&](ValueRef iv, const std::vector<ValueRef>&) {
                       ivs.push_back(iv);
                       nest(d + 1);
                       ivs.pop_back();
                       return std::vector<ValueRef>{};
                     });
    };
    nest(0);
  }

  Module& m_;
  NodeId func_;
  sema::Sema& sema_;
  Builder b_;
  std::map<ValueRef, ValueRef> buffer_;  // tensor value -> the buffer holding it
  std::vector<ValueRef> temps_;          // buffers freed at the end
  ValueRef output_{};
};

bool lowerAten(Module& m, const passes::Context& ctx, std::string* err) {
  std::vector<NodeId> funcs;
  for (NodeId id : m.top_level) {
    if (const Node* n = m.find(id); n && n->kind == "func") funcs.push_back(id);
  }
  for (NodeId f : funcs) {
    sema::Sema sema(m, ctx.reg);
    if (!Lowering(m, f, sema).run(err)) return false;
  }
  return true;
}

}  // namespace

BRRR_PASS(lower_aten, "lower-aten",
          "ATen tensor ops (from the PyTorch frontend) to loop nests over buffers, "
          "every access proven in bounds",
          lowerAten);
