#pragma once

#include "ir/ir.h"

namespace bridgeir {

// Demo transform: for every `ret` whose operand is produced by `add` in the same
// scope, insert a `relu` between them. Demonstrates stable identity: existing nodes
// keep their ids; only the new node gets a fresh one, and mutations are journaled.
void demoInsertReluAfterAdd(Module& m);

}  // namespace bridgeir
