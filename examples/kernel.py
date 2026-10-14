"""PyTorch in, Brrr out: export one kernel with torch.export and write it as .br.

    python examples/kernel.py [outdir]     # default: next to this file
    brrr check examples/kernel.br --schemas=examples/aten.bs

The kernel is a transformer feed-forward block, y = x + fc2(relu(fc1(x))), over
a dynamic number of tokens that is always a multiple of 8 (torch: 8 * Dim("t")).

torch.export already knows the symbolic shapes and their ranges, and they map
straight onto Brrr: the symbol becomes an index argument (%t), shapes become
expressions in types (tensor<[8 * %t, 64], f32>), and range constraints become
facts (fact 1 <= %t <= 512). No MLIR in between: it would turn 8 * t into `?`.

Every ATen op becomes a Brrr op with the result type torch inferred, and the ops
are declared in aten.bs with shape rules, so Brrr re-proves torch's shapes with
its own solver. An op without a known rule is still emitted (with torch's types)
and declared without a rule: never silently dropped.
"""

import os
import sys

import sympy
import torch
import torch.nn as nn
from torch.export import Dim, export

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- the kernel ------------------------------------------------------------------

class FeedForward(nn.Module):
    def __init__(self, d_model=64, d_hidden=256):
        super().__init__()
        self.fc1 = nn.Linear(d_model, d_hidden)
        self.fc2 = nn.Linear(d_hidden, d_model)

    def forward(self, x):
        return x + self.fc2(torch.relu(self.fc1(x)))


def export_kernel():
    t = Dim("t", min=1, max=512)
    tokens = 8 * t  # the token count is always a multiple of 8
    example = (torch.randn(64, 64),)
    return export(FeedForward(), example, dynamic_shapes={"x": {0: tokens}})


# ---- ATen ops as Brrr schema data ---------------------------------------------------

# The shape rules Brrr checks. `S` is a whole shape; names inside [...] are dims.
RULES = {
    "aten.linear": ("input, weight, bias", "[B, K], [N, K], [N] -> [B, N]"),
    "aten.relu": ("self", "S -> S"),
    "aten.gelu": ("self", "S -> S"),
    "aten.add": ("self, other", "S, S -> S"),
    "aten.mul": ("self, other", "S, S -> S"),
    "aten.mm": ("self, mat2", "[M, K], [K, N] -> [M, N]"),
}

DTYPES = {
    torch.float32: "f32",
    torch.float16: "f16",
    torch.bfloat16: "bf16",
    torch.float64: "f64",
    torch.int64: "i64",
    torch.int32: "i32",
    torch.int8: "i8",
    torch.bool: "i1",
}


def kind_of(target):
    """aten.add.Tensor -> aten.add (the default overloads), else keep the overload."""
    packet = target.overloadpacket.__name__
    overload = target._overloadname
    if overload in ("default", "Tensor"):
        return "aten." + packet
    return "aten." + packet + "." + overload


def schema_text(kinds):
    out = [
        "# ATen ops used by examples/kernel.br, as Brrr schema data (written by kernel.py).",
        "# A rule lets Brrr re-prove the shapes torch inferred; an op without one keeps",
        "# torch's annotated types and is checked for arity only.",
        "",
    ]
    for kind in sorted(kinds):
        out.append("kind " + kind + " {")
        if kind in RULES:
            operands, shape = RULES[kind]
            out.append("  operands: " + operands)
            out.append("  shape: " + shape)
        else:
            out.append("  operands: args*")
        out.append("  traits: pure")
        out.append("}")
        out.append("")
    return "\n".join(out)


# ---- symbolic shapes -----------------------------------------------------------------

def expr_text(e, names):
    """A SymPy integer expression as a Brrr expression over %names."""
    e = sympy.sympify(e)
    if e.is_Integer:
        return str(int(e))
    if e.is_Symbol:
        return "%" + names.setdefault(e, str(e))
    if e.is_Add:
        parts = [expr_text(a, names) for a in sympy.Add.make_args(e)]
        text = parts[0]
        for p in parts[1:]:
            text += " - " + p[1:] if p.startswith("-") else " + " + p
        return text
    if e.is_Mul:
        coeff, rest = e.as_coeff_Mul()
        factors = [expr_text(f, names) for f in sympy.Mul.make_args(rest)]
        factors = ["(" + f + ")" if (" + " in f or " - " in f) else f for f in factors]
        body = " * ".join(factors)
        if coeff == 1:
            return body
        if coeff == -1:
            return "-" + body
        return str(int(coeff)) + " * " + body
    name = type(e).__name__
    if name == "FloorDiv":
        num, den = e.args
        return "(" + expr_text(num, names) + ") floordiv " + expr_text(den, names)
    if name in ("Mod", "PythonMod"):
        num, den = e.args
        return "(" + expr_text(num, names) + ") mod " + expr_text(den, names)
    if name == "CeilToInt" or name == "CeilDiv":
        num, den = e.args
        return "(" + expr_text(num, names) + ") ceildiv " + expr_text(den, names)
    raise ValueError("shape expression outside Brrr's language: %s (%s)" % (e, name))


def dim_text(d, names):
    if isinstance(d, int):
        return str(d)
    return expr_text(d.node.expr, names)


def type_text(val, names):
    if not isinstance(val, torch.Tensor):
        raise ValueError("not a tensor: %r" % (val,))
    dims = ", ".join(dim_text(d, names) for d in val.shape)
    return "tensor<[%s], %s>" % (dims, DTYPES[val.dtype])


def attr_text(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"' + v + '"'
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(attr_text(x) for x in v) + "]"
    if isinstance(v, torch.dtype):
        return DTYPES[v]
    raise ValueError("attribute value Brrr cannot hold: %r" % (v,))


# ---- the lowering ----------------------------------------------------------------------

def symbol_names(ep, dynamic):
    """Name each shape symbol after the torch Dim that introduced it."""
    names = {}
    user_inputs = {s.arg.name for s in ep.graph_signature.input_specs if s.kind.name == "USER_INPUT"}
    for n in ep.graph.nodes:
        if n.op != "placeholder" or n.name not in user_inputs:
            continue
        for axis, dim in dynamic.get(n.name, {}).items():
            root = getattr(dim, "root", dim)
            free = n.meta["val"].shape[axis].node.expr.free_symbols
            if len(free) == 1:
                names[next(iter(free))] = root.__name__
    return names


def lower(ep, dynamic):
    names = symbol_names(ep, dynamic)
    value = {}  # fx node name -> Brrr value name
    params, inputs, body, kinds = [], [], [], set()

    for spec in ep.graph_signature.input_specs:
        name = spec.arg.name
        value[name] = name[2:] if spec.kind.name == "PARAMETER" and name.startswith("p_") else name

    result_type = None
    for n in ep.graph.nodes:
        if n.op == "placeholder":
            decl = "%" + value[n.name] + ": " + type_text(n.meta["val"], names)
            (inputs if n.name in dynamic else params).append(decl)
        elif n.op == "call_function":
            kind = kind_of(n.target)
            kinds.add(kind)
            operands, attrs = [], []
            schema_args = n.target._schema.arguments
            for i, a in enumerate(n.args):
                if isinstance(a, torch.fx.Node):
                    operands.append("%" + value[a.name])
                else:
                    attrs.append("%s=%s" % (schema_args[i].name, attr_text(a)))
            for k, a in n.kwargs.items():
                if isinstance(a, torch.fx.Node):
                    operands.append("%" + value[a.name])
                else:
                    attrs.append("%s=%s" % (k, attr_text(a)))
            value[n.name] = n.name
            line = "  %" + n.name + " = " + kind + " " + ", ".join(operands)
            if attrs:
                line += " " + " ".join(attrs)
            line += " : " + type_text(n.meta["val"], names)
            body.append(line)
        elif n.op == "output":
            (out,) = n.args[0]
            body.append("  return %" + value[out.name])
            result_type = type_text(out.meta["val"], names)

    # Range constraints hold for symbols (s60) and for derived dims (8*s60); each
    # free symbol is an index argument, each constraint a fact (a precondition).
    free = set()
    for key in ep.range_constraints:
        free |= sympy.sympify(key).free_symbols
    symbols = ["%" + names.setdefault(s, str(s)) + ": index" for s in sorted(free, key=str)]
    facts = []
    for key, r in sorted(ep.range_constraints.items(), key=lambda kv: (len(str(kv[0])), str(kv[0]))):
        e = expr_text(key, names)
        if str(r.upper) in ("oo", "int_oo"):
            facts.append("  fact %d <= %s" % (int(r.lower), e))
        else:
            facts.append("  fact %d <= %s <= %d" % (int(r.lower), e, int(r.upper)))

    header = [
        "# A transformer feed-forward block, y = x + fc2(relu(fc1(x))), exported from",
        "# PyTorch by examples/kernel.py (torch.export, no MLIR). Do not edit: rerun",
        "#   python examples/kernel.py",
        "#   brrr check examples/kernel.br --schemas=examples/aten.bs",
        "#",
        "# torch's dynamic token count 8 * Dim(\"t\") is the expression 8 * %t, and its",
        "# range is a fact. Brrr re-proves every shape torch inferred (aten.bs rules).",
    ]
    args = ",\n    ".join(symbols + params + inputs)
    func = ["func @forward(\n    " + args + ")\n    -> " + result_type + " {"] + facts + body + ["}"]
    return "\n".join(header + func) + "\n", kinds


def dump_data(outdir, t, seed=0):
    """Parameters, an input of 8 * t tokens, and torch's output, as raw f32 files
    (row-major, the layout `forward`'s buffers use). Returns the module and input."""
    import array

    torch.manual_seed(seed)
    model = FeedForward().eval()
    x = torch.randn(8 * t, 64)
    with torch.no_grad():
        y = model(x)
    os.makedirs(outdir, exist_ok=True)
    tensors = [("fc1_weight", model.fc1.weight), ("fc1_bias", model.fc1.bias),
               ("fc2_weight", model.fc2.weight), ("fc2_bias", model.fc2.bias), ("x", x), ("y", y)]
    for name, a in tensors:
        with open(os.path.join(outdir, name + ".bin"), "wb") as f:
            array.array("f", a.detach().contiguous().view(-1).tolist()).tofile(f)
    return model, x


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else HERE  # optional output directory
    ep = export_kernel()
    dynamic = {"x": {0: 8 * Dim("t", min=1, max=512)}}
    br, kinds = lower(ep, dynamic)
    with open(os.path.join(out, "kernel.br"), "w", newline="\n") as f:
        f.write(br)
    with open(os.path.join(out, "aten.bs"), "w", newline="\n") as f:
        f.write(schema_text(kinds))
    print("wrote kernel.br and aten.bs to %s (%d ATen kinds)" % (out, len(kinds)))


if __name__ == "__main__":
    sys.exit(main())
