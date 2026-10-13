// FoldReshape-style legality, grounded in bishengir's
// CanonicalizeTensorReshape.cpp ("Without divisibility API, parting can only
// assume the value is the same"). A chained reshape pair composes when element
// counts are provably equal and the fused output is provably aligned — decided by
// first-hand queries against the facts carried in the IR document.
#pragma once

#include "ir/ir.h"
#include "symbolic/symbolic.h"

namespace bridgeir {

void foldLooseReshapePairs(Module& m, const symbolic::Solver& facts);

}  // namespace bridgeir
