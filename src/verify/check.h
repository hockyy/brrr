// Turns the schema into judgments about a module: operand counts, attribute
// policy and types, results policy. Declared vocabulary is strict; foreign kinds
// are reported as warnings (imported vocabulary is expected in a multi-IR
// framework) — the point is that nothing is *silently* accepted.
#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"
#include "schema/schema.h"

namespace bridgeir::verify {

struct Issue {
  bool error = true;
  NodeId node = kInvalidId;
  std::string kind;
  std::string message;
};

std::vector<Issue> checkModule(const Module& m, const schema::Registry& reg);

bool hasErrors(const std::vector<Issue>& issues);

}  // namespace bridgeir::verify
