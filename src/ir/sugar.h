// C++-shaped surface sugar: `for`, `while`, `if`/`else`.
//
// This is a pure textual lowering step that runs before parsing: the sugar is
// rewritten into canonical IR lines, so the core, the schema, the checks and the
// invariants are untouched. `for`/`while` become `loop` + `param`s +
// `loop.yield`; `if`/`else` becomes `cbr` + branch `block`s + `edge`s + a join
// `block` that carries the node id/name (so the `if` value is the join's ports).
#pragma once

#include <string>
#include <vector>

namespace bridgeir {

// Lower sugar in in-place line form. Returns false with *err set (including the
// offending line text) when a construct is malformed.
bool lowerCppSugar(std::vector<std::string>* lines, std::string* err);

}  // namespace bridgeir
