// Text form of the v1 IR (docs/scratch/syntax-v1.md).
//
// Two printings of one module:
//   clean  (default)   — `%y = reshape %x : ...`; unnamed values print as `%<id>`
//   stored (ids=true)  — every definition carries its id: `n4 %y = reshape ...`
// The parser reads both. Stored text round-trips exactly; clean text round-trips as
// text, and ids are recovered by `reconcileIds` when it is an edit of a known state.
#pragma once

#include <string>

#include "ir/ir.h"

namespace brrr {

struct ParseResult {
  bool ok = false;
  Module module;
  std::string error;
  int line = 0;
};

struct PrintOptions {
  bool ids = false;
};

std::string printModule(const Module& m, const PrintOptions& opts = {});
ParseResult parseModule(const std::string& text);

// Give `edited` the ids of `base` wherever a definition matches (functions and
// modules by @symbol, blocks by label, args by position, named values by name,
// unnamed values by their numeric name, result-less ops by kind and position in
// their scope). Unmatched definitions keep fresh ids above everything in `base`.
// Returns the remapped module; `report` lists matched/added/removed counts.
Module reconcileIds(const Module& base, const Module& edited, std::string* report);

}  // namespace brrr
