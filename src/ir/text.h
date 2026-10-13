// Canonical text format (v0).
//
//   # bridgeir v0
//   func @name(nID) {
//     nID = kind ref* key=value*      // attrs sorted by key; refs are nID[.port]
//   }
//
// Canonical means: printing a parsed module is deterministic, and ids in the text are
// the stored ids — never renumbered.
#pragma once

#include <string>

#include "ir/ir.h"

namespace bridgeir {

struct ParseResult {
  bool ok = false;
  Module module;
  std::string error;
  int line = 0;
};

std::string printModule(const Module& m);
ParseResult parseModule(const std::string& text);

}  // namespace bridgeir
