// Expression parsing for the IR text format: integers, symbols, `+`, `*`,
// parentheses. Symbols are first-hand language features, so their expressions
// parse straight from the document.
#pragma once

#include <string>

#include "symbolic/symbolic.h"

namespace bridgeir::symbolic {

// Parse an expression. Returns nullptr and sets *err on malformed input.
Expr parseExpr(const std::string& text, std::string* err);

}  // namespace bridgeir::symbolic
