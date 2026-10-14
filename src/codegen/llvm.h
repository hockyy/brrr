// LLVM IR text from Brrr: the codegen target. clang (or llc) turns the text
// into a binary; Brrr itself links nothing.
//
//   index / iN / uN     i64 / iN (signedness picks the instructions)
//   f16 / f32 / f64     half / float / double
//   buf<[dims], e>      ptr: row-major and contiguous, addressed by its dims
//   func @f             define; the ops in the function's own scope are the
//                       entry block, each ^block a basic block
//   block arguments     phi nodes, one incoming value per branch edge
//   expressions         i64 arithmetic: add/sub/mul nsw (index arithmetic never
//                       wraps, by the soundness rule), floordiv/ceildiv/mod by a
//                       constant as sdiv/srem plus a rounding correction
//   load/store          getelementptr on the row-major offset, then load/store
//   guard               a check that branches to llvm.trap when it fails
//
// What has no lowering yet is an error naming the node: tensor-level ops (lower
// them first, e.g. lower-aten), structured loops, unrealized casts, and dims
// with no value. Facts are not emitted yet (later: llvm.assume, nsw, noalias).
#pragma once

#include <string>

#include "ir/ir.h"
#include "schema/schema.h"

namespace brrr::codegen {

// Every function in `m` as one LLVM module. False (with *err) when something
// has no lowering.
bool emitLLVM(const Module& m, const schema::Registry& reg, std::string* out, std::string* err);

}  // namespace brrr::codegen
