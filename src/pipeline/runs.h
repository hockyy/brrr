// Pipeline runs: compilation as a recorded, branchable history.
//
// A pipeline is data, one pass per line (`fold-reshape`, `dce`, `# comments`).
// Running it creates a run with an id (`r1`, `r2`, ...). Every stage stores the
// state before and after it, content-addressed, so any point can be inspected,
// branched from, or resumed with a different pipeline or an edited IR.
//
//   .brrr/
//     objects/<hash>.br        states, stored form (ids included), deduplicated
//     runs/<id>/run.txt        manifest: parent, input, status, stage lines
//     runs/<id>/pipeline.txt   the passes still to run — edit it, then resume
//     runs/<id>/NN-<pass>.txt  stage record: before/after, diff, journal, issues
//     answers.txt              recorded solver answers, shared by every run
//
// Solver answers are recorded: with Z3 an answer is a function of its question
// (see symbolic::AnswerCache), so a branch or a resumed run replays exactly the
// answers the original run got. The manifest names the solver that answered.
//
// After each stage the state is verified (sema::verifyModule). A stage that
// leaves errors is rolled back: the run stops at the old state, marked failed,
// and the rejected state is kept for inspection.
#pragma once

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "schema/schema.h"

namespace brrr::pipeline {

struct Options {
  std::string store = ".brrr";
  std::string pipeline_file;  // --pipeline=<file>
  std::string passes;         // --passes=a,b
  std::string state_file;     // --state=<edited.br>
  int steps = -1;             // --steps=N (-1 = all)
  bool pause_on_warning = false;
  bool ids = false;
  bool record = false;
  std::vector<std::string> schema_files;
};

// `run <file>` · `resume <run>` · `branch <run>@<k>` · `runs` · `show <run>[@k]`
// · `passes`. Returns a process exit code.
int command(const std::string& cmd, const std::vector<std::string>& args,
            const Options& opts, const schema::Registry& reg, const std::string& input_text,
            std::ostream& out, std::ostream& err);

bool isCommand(const std::string& cmd);

}  // namespace brrr::pipeline
