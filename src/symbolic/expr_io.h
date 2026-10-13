// Expression parsing for the IR text format: integers, symbols, `+`, `*`,
// parentheses. Symbols are first-hand language features, so their expressions
// parse straight from the document.
#pragma once

#include <string>
#include <vector>

#include "symbolic/symbolic.h"

namespace bridgeir::symbolic {

// Parse an expression. Returns nullptr and sets *err on malformed input.
Expr parseExpr(const std::string& text, std::string* err);

// Split a top-level comma-separated list (parenthesis-aware), e.g. a `dims` attr.
std::vector<std::string> splitTopLevel(const std::string& s, char delim);

}  // namespace bridgeir::symbolic
