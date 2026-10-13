// JSON encoding of the IR: same term model, universal notation.
//
// The surface text stays canonical and token-lean; JSON is the wire/interchange
// encoding (agents, connectors, other languages). Meaning still comes from the
// schema, not from the notation.
#pragma once

#include <string>

#include "ir/ir.h"

namespace bridgeir::io {

std::string toJson(const Module& m);

}  // namespace bridgeir::io
