// C-like view of the IR — a readable skin for humans and agents.
//
// Read-only for now: the canonical `.br` text stays the parse format, and this
// view preserves identities (ids are shown as trailing `// nN` comments). A
// parseable C-like dialect is a candidate third encoding — same term model.
#pragma once

#include <string>

#include "ir/ir.h"

namespace bridgeir::io {

std::string toCLike(const Module& m);

}  // namespace bridgeir::io
