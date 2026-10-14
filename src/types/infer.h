// Schema-driven type inference: typing rules are DATA (one Signature per kind),
// solving is unification. Adding typing for an op = adding a record, not code.
#pragma once

#include <string>
#include <vector>

#include "types/types.h"

namespace brrr::types {

// A typing rule for a kind. Variables 0..type_params-1 are fresh per instantiation
// (e.g. mm : tensor<A,M,K> x tensor<A,K,N> -> tensor<f32,M,N> has 4).
struct Signature {
  std::string kind;
  size_t type_params = 0;
  std::vector<TypeRef> operand_types;
  TypeRef result_type;
};

// A node in the inference graph; operands index earlier nodes (topological order).
struct InferNode {
  size_t id = 0;                 // display id (e.g. brrr node id)
  std::string kind;
  std::vector<size_t> operands;  // indices into the node list
  TypeRef given;                 // optional declared type (from attrs); may be null
};

struct InferResult {
  bool ok = true;
  std::string error;               // the failing equation, with a path
  std::vector<TypeRef> types;      // resolved type per node
  std::vector<std::string> equations;  // pretty transcript, for feedback
};

InferResult infer(const std::vector<InferNode>& nodes,
                  const std::vector<Signature>& signatures);

}  // namespace brrr::types
