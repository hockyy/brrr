#include "passes/demo_fuse.h"

#include <string>
#include <vector>

namespace bridgeir {

void demoInsertReluAfterAdd(Module& m) {
  std::vector<NodeId> rets;
  for (const Node& n : m.nodes) {
    if (n.kind == "ret") rets.push_back(n.id);
  }

  for (NodeId rid : rets) {
    const Node* ret = m.find(rid);
    if (!ret || ret->operands.size() != 1) continue;
    const ValueRef vr = ret->operands[0];
    const Node* producer = m.find(vr.id);
    if (!producer || producer->kind != "add" || producer->scope != ret->scope) {
      continue;
    }

    const NodeId pid = producer->id;
    const NodeId scope = producer->scope;

    Node& relu = m.insertNodeAfter(pid, "relu", scope);
    const NodeId relu_id = relu.id;
    relu.operands.push_back({pid, vr.port});

    // Re-find `ret`: inserting into the node vector may have moved it.
    m.find(rid)->operands[0] = {relu_id, 0};

    m.log("insert", relu_id, "kind=relu reason=demo-fuse-after-ret");
    m.log("retarget", rid,
          "operand0: n" + std::to_string(pid) + " -> n" + std::to_string(relu_id));
  }
}

}  // namespace bridgeir
