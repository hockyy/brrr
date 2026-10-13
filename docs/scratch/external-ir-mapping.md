# Mapping external IRs into BridgeIR

> **2026-10-13 supersede:** the `phi` row and the phi-based `dot` mapping below are
> superseded — `phi` was removed. LLVM/clang output maps onto block arguments fed
> by `edge` values (see `src/examples/dot_llvm.br`). See `design-notes.md`.

Status: design analysis, 2026-10-13. Grounded in real examples taken from this machine:
AscendNPU-IR (`bishengir/test/Conversion/HFusionToHIVM/matmul.mlir`), a live
`torch.fx.symbolic_trace` capture (torch 2.13), and `clang++ -O1 -emit-llvm` output.
The mapped files live in `src/examples/` and are round-trip tested.

## 0. The universal recipe

Every IR maps into BridgeIR through the same small frame — this is the payoff of a
~6-concept core:

| External concept | BridgeIR concept |
|---|---|
| SSA value / temporary / register | result **port** of a node (`nID.port`) |
| operation / instruction / call | **node** (kind = namespaced name, e.g. `hivm.hir.mmadL1`) |
| dialect / opcode table | kind **namespace** (+ future schema registry) |
| function / module / graph | `func` node (a scope), `sym` name |
| region / loop body / basic block | **scope node** (`in="nID"` membership for blocks, v0) |
| branch / call target | **operands referencing target nodes** (control edges later) |
| phi | node with `(value, from-block)` operand pairs |
| type | **type data** (`type=` attr now; first-class Type later) |
| constants / attributes | attr values (data); bulk data goes to side tables |
| debug / metadata | `loc=` attr (future); metadata as data |
| SSA name, FX node name, ONNX name | **BridgeIR name** (editable; ids stay machine-stable) |
| external stable ids (ONNX, FX) | names + `origin=` attrs (provenance of import) |

The four worked examples below are the acceptance test: if these map cleanly, the
concept set is right — and the gaps they expose (§6) are exactly our roadmap.

## 1. MLIR / HIVM (AscendNPU-IR) — the detailed case

Real input (`matmul.mlir`, `@test_MmadL1_real_init`):

```mlir
func.func @test_MmadL1_real_init(%arg1:memref<16x16xf32>) -> tensor<16x16xf32> {
  %ma = memref.alloc() : memref<16x16xf16>
  %ma_tensor = bufferization.to_tensor %ma restrict writable : memref<16x16xf16>
  ...
  %ret = linalg.matmul ins(%ma_tensor, %mb_tensor : ...) outs(%mc: ...) -> tensor<16x16xf32>
  return %ret : tensor<16x16xf32>
}
```

Lowered form (`hivm.hir.mmadL1`, `hivm.hir.vbrc`, `hivm.hir.copy`), mapped in
`src/examples/matmul_hivm.br`:

```
func @test_MmadL1_real_init(n1) {
  n2 mc_arg = param idx=0 type="memref<16x16xf32>"
  n3 cst = arith.constant type="f32" value="0.0"
  n4 mc = bufferization.to_tensor n2 restrict="true" writable="true" type="tensor<16x16xf32>"
  n5 ma = memref.alloc shape="16x16" dtype="f16"
  n6 ma_tensor = bufferization.to_tensor n5 restrict="true" writable="true" type="tensor<16x16xf16>"
  n7 mb = memref.alloc shape="16x16" dtype="f16"
  n8 mb_tensor = bufferization.to_tensor n7 restrict="true" writable="true" type="tensor<16x16xf16>"
  n9 ret = hivm.hir.mmadL1 n6 n8 n4 init="false" type="tensor<16x16xf32>" core="aic"
  n10 = func.return n9
}
```

Mapping decisions:

- `func.func` → one `func` scope; `%ssa_name` → node names where the source name is
  not an identifier (`%ma_tensor` is; `%0` is not).
- Dialect ops keep full names as kinds (`hivm.hir.mmadL1`) — namespacing is data, the
  future registry validates it.
- `ins(...) outs(...)` **operand groups flatten to ordered operands** (ins first).
  This is lossy-ish: the core needs named port groups (§6.1).
- `restrict`/`writable`, `size_hint`, `a_transpose` etc. → attrs.
- Ascend specifics: core selection (`aic`/`aiv`) and pipe assignment are attrs today;
  they become **effect/managed-resource annotations** once effect edges land — for
  scheduling, who-owns-what must be explicit, not positional.
- Loops (`scf.for ... iter_args`) from the same test map as: loop = scope node,
  `iter_args` = operands, `scf.yield` = result links. This needs block-args/yield
  support (§6.2) — the reason the example here uses the loop-free variant.

## 2. PyTorch FX — the easiest case

Live capture (torch 2.13):

```
graph():
    %x : [num_users=1] = placeholder[target=x]
    %w : [num_users=1] = get_attr[target=w]
    %matmul : [num_users=1] = call_function[target=torch.matmul](args = (%x, %w), ...)
    %add : [num_users=1] = call_function[target=operator.add](args = (%matmul, ...), ...)
    %relu : [num_users=1] = call_function[target=torch.relu](args = (%add,), ...)
    return relu
```

Mapped (`src/examples/fx_mlp.br`):

```
func @mlp(n1) {
  n2 x = placeholder name="x" type="Tensor"
  n3 bias = placeholder name="bias" type="Tensor"
  n4 w = get_attr name="w" module="@M.w"
  n5 mm = call_function target="torch.matmul" x w
  n6 sum = call_function target="operator.add" mm bias
  n7 act = call_function target="torch.relu" sum
  n8 = output act
}
```

Notes: FX is already name-keyed and straight-line — the mapping is nearly the
identity. `target=` (callable identity) is data; Dynamo guards would live as scope
attrs; graph breaks = several scopes. Lowering direction: FX → BridgeIR → schedule
selection (S1/S2) → Triton/LLVM emission.

## 3. C++ → LLVM IR — the control-flow case

Real `clang++ -O1 -emit-llvm` output for `float dot(const float*, const float*, int)`
has three blocks, a loop, phis, and `llvm.fmuladd`. Mapped in
`src/examples/dot_llvm.br`:

```
func @dot(n1) {
  n2 p0 = param idx=0 type="ptr<float>"
  ...
  n5 entry = block
  n6 zero_i32 = arith.constant type="i32" value="0" in="n5"
  n7 cond = icmp n n6 pred="sgt" type="i1" in="n5"
  n8 = br cond header exit in="n5"
  n10 header = block
  n11 span = zext n type="i64" in="n10"
  n12 = br body in="n10"
  n13 exit = block
  n14 res = phi zero_f32 entry fma body in="n13"
  n15 = func.return res in="n13"
  n16 body = block
  n17 idx = phi zero_i32 header next body in="n16"
  n18 acc = phi zero_f32 header fma body in="n16"
  n19 ap0 = gep p0 idx elem="float" in="n16"
  ...load, gep, load...
  n23 fma = llvm.fmuladd av0 av1 acc in="n16"
  n24 one = arith.constant type="i64" value="1" in="n16"
  n25 next = arith.addi idx one in="n16"
  n26 done = icmp next span pred="eq" type="i1" in="n16"
  n27 = br done exit body in="n16"
}
```

Key decisions: **blocks are nodes with stable ids** (not renumbered labels); branch
targets are ordinary operands, so the CFG is queried like any other edge set; phi
carries `(value, from-block)` pairs; `in="nID"` marks membership (v0 stand-in until
scope edges are first-class). Notice what the format makes easy that LLVM text does
not: `dot` nodes keep identity across transformations, and `phi`/`br` targets are
name-resolvable.

C++ *source-level* frontends (Clang AST, CIR) map with the same frame — statements
are nodes in scope nodes, classes become frontend constructs + type data — but the
core deliberately does not model C++ semantics; C++ enters via its lowered IR.

## 4. Other languages — the recipe table

| Language IR | Form | Mapping recipe (key points) |
|---|---|---|
| Rust MIR | CFG + locals | blocks → `block` nodes; locals → nodes/ports; borrow/place info → attrs; Miri-style execution = future interpreter |
| Swift SIL | SSA + ownership | ownership ops (`load`, `retain`, `release`) → nodes; ownership → effect edges |
| Go SSA | values in blocks | same as LLVM mapping; simple kinds |
| JVM bytecode / CIL | stack VM | **import-time SSA construction**: stack slots → ports; locals → nodes; exceptions → effect edges |
| V8/JSC JIT graphs | sea-of-nodes | direct: they are already nodes+edges; control/effect chains → our edges |
| GHC Core / STG | typed let | bindings → names; thunks/updates → effect edges; laziness → evaluation-order attrs |
| OCaml Lambda/Flambda, SML CPS | ANF / CPS | continuations → scope nodes; our scope/effect edges *are* CPS-friendly (Kelsey) |
| Erlang Core / BEAM | functional → register VM | module/func scopes; message sends → effects; processes → managed resources |
| wasm | structured bytecode | block/loop scopes + import-time stack→SSA; validation = future invariants |
| SPIR-V / PTX | structured SSA / virtual ISA | scopes for structure; capabilities → attrs; memory model → effect classes |
| eBPF | register IR + verifier | nodes + registers; **verifier rules → invariant contracts** (our gates) |
| SQL / query plans | plan tree | nodes = operators; logical vs physical → attrs/kinds; Cascades-style memo → journal |
| TVM TIR / Halide | loop nest + schedule | loop = scope; schedule as separate data attached to scopes; thread bindings → attrs |
| E-graphs (egglog) | equivalence classes | classes as side-tables keyed by anchor node ids |
| Traces (PyPy, LuaJIT) | execution history | observation data feeding S2 artifacts, not core node kinds |

The regularity is the point: after four worked examples, every further language is a
frontend recipe, not a core change. "Adding a language" = writing a small importer +
schema package (extension cost = 0 core files, per our gate).

## 5. Importer / emitter plan

1. **Now**: hand-mapped examples (`src/examples/`) — round-trip tested, proving the
   core can *hold* these IRs today.
2. **Next**: `bridgeir import --from=mlir-text <file>` for the subset used in
   AscendNPU-IR tests (lex + map; fresh ids; names from SSA names; `origin=` attrs),
   and `--from=fx` (consume `torch.fx` JSON/graph dump) — adapters, never core.
3. **Then**: `bridgeir emit --to=mlir-text` and validate by round-tripping through
   `bishengir-opt`/`mlir-opt` on real test files (diff-driven).
4. Provenance: every imported node records `origin=`, so import → transform → emit
   keeps a traceable chain.

## 6. What the mapping demands from the core (priority-ordered gaps)

1. **Named port groups + multiple results** (ins/outs, multi-result ops).
2. **Blocks, block args, and yields** (regions as scope nodes with arguments;
   terminates `scf.for`/`scf.if` and MLIR regions).
3. **Reference-valued attributes** (symbol refs, block refs; today: quoted strings).
4. **Type terms first-class** (currently `type=` attr strings; needed for schemas).
5. **Effect edges** (calls, loads/stores, pipes, messages, core assignment aic/aiv).
6. **Loc/metadata attrs** (debuggability, feedback).
7. **Schema registry** (validate kinds imported from dialects; namespacing rules).
8. **Importers/emitters as adapters** (Phase 2/3 above), with round-trip harness.

None of these break the six-primitive model; they are refinements the real examples
proved necessary. That is exactly what a mapping exercise is for.
