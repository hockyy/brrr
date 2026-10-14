// One parser for every type expression — declared constructors, not per-type code.
//
//   f16                         (nullary: a declared scalar)
//   tensor<f16, 64, 128>        (declared constructor with parameters)
//   buf<tensor<f16, 64>, 8>     (nested constructors)
//
// The grammar is fixed (`name` or `name<arg, ...>` with nested terms); the set of
// legal names, their arity and their parameter kinds come from schema
// declarations — exactly like ops: one node parser, kinds as data.
#pragma once

#include <string>

#include "types/types.h"

namespace brrr::types {

bool parseTypeTerm(const std::string& text, TypeRef* out, std::string* err);

}  // namespace brrr::types
