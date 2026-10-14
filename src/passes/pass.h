// Writing a pass: one .cpp file in passes/, against the core API (ir/, schema/,
// symbolic/, sema/), ending with one line that registers it:
//
//   #include "passes/pass.h"
//
//   namespace {
//   bool myPass(brrr::Module& m, const brrr::passes::Context& ctx,
//               std::string* err) {
//     ...                       // edit `m` through Module's journaled methods
//     return true;              // false (with *err) fails the stage
//   }
//   }  // namespace
//
//   BRRR_PASS(my_pass, "my-pass", "what it does, in one line", myPass);
//
// CMake picks up every file in passes/ (no list to edit), and the pass is then
// available by name in pipelines and in `brrr passes`. The run store
// verifies the state after every stage, so a broken pass is rolled back, not
// trusted.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "schema/schema.h"

namespace brrr::passes {

struct Context {
  const schema::Registry& reg;
  std::map<std::string, std::string> options;  // `key=value` from the pipeline line
};

struct PassInfo {
  std::string name;
  std::string summary;
  bool (*run)(Module& m, const Context& ctx, std::string* err);
};

// Every registered pass, sorted by name.
std::vector<PassInfo> registry();
const PassInfo* find(const std::string& name);

struct Registrar {
  explicit Registrar(PassInfo p);
};

// A pure op (trait `pure`, no region) can be removed when unused.
bool isPure(const Context& ctx, const Node& n);

}  // namespace brrr::passes

#define BRRR_PASS(id, name, summary, fn) \
  static const ::brrr::passes::Registrar brrr_pass_##id({name, summary, fn})
