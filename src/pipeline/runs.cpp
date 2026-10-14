#include "pipeline/runs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <set>
#include <sstream>

#include "ir/text.h"
#include "passes/pass.h"
#include "sema/sema.h"
#include "symbolic/expr_io.h"
#include "symbolic/symbolic.h"
#include "symbolic/z3_backend.h"

namespace brrr::pipeline {
namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

bool readText(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

void writeText(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

// FNV-1a, 64 bits: enough to name states in one store.
std::string hashText(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// ---- pipelines ----------------------------------------------------------------

struct Stage {
  std::string pass;
  std::map<std::string, std::string> options;
};

std::string stageLine(const Stage& s) {
  std::string out = s.pass;
  for (const auto& [k, v] : s.options) out += " " + k + "=" + v;
  return out;
}

bool parseStage(const std::string& line, Stage* out, std::string* err) {
  std::istringstream in(line);
  std::string word;
  in >> out->pass;
  while (in >> word) {
    const size_t eq = word.find('=');
    if (eq == std::string::npos || eq == 0) {
      *err = "pass options are key=value, got '" + word + "' in '" + line + "'";
      return false;
    }
    out->options[word.substr(0, eq)] = word.substr(eq + 1);
  }
  return true;
}

// One pass per line (or per comma for --passes); `#` starts a comment.
bool parseStages(const std::string& text, char sep, std::vector<Stage>* out, std::string* err) {
  std::string cur;
  auto flush = [&]() {
    std::string line = cur;
    cur.clear();
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) return true;
    Stage s;
    if (!parseStage(line, &s, err)) return false;
    out->push_back(s);
    return true;
  };
  for (const char c : text) {
    if (c == sep || c == '\n') {
      if (!flush()) return false;
    } else {
      cur += c;
    }
  }
  return flush();
}

// ---- the store ----------------------------------------------------------------

struct StageRec {
  size_t index = 0;
  std::string status;  // ok | failed
  std::string before, after;
  double ms = 0;
  size_t added = 0, removed = 0, changed = 0;
  std::string what;  // `fold-reshape`, `input <file>`, `edit <file>`, `from r1@2`
};

struct Run {
  std::string id;
  std::string parent = "-";
  std::string input;
  std::string status;  // done | paused | failed
  std::string state;   // current state hash
  std::vector<StageRec> stages;
  std::vector<Stage> pending;
};

class Store {
 public:
  explicit Store(fs::path root) : root_(std::move(root)) {}

  std::string put(const std::string& text) {
    const std::string h = hashText(text);
    const fs::path p = objectPath(h);
    if (!fs::exists(p)) writeText(p, text);
    return h;
  }
  bool get(const std::string& h, std::string* text) const { return readText(objectPath(h), text); }
  fs::path objectPath(const std::string& h) const { return root_ / "objects" / (h + ".br"); }
  fs::path runDir(const std::string& id) const { return root_ / "runs" / id; }

  std::vector<std::string> runIds() const {
    std::vector<std::pair<long, std::string>> ids;
    const fs::path runs = root_ / "runs";
    if (fs::exists(runs)) {
      for (const auto& e : fs::directory_iterator(runs)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 1 && name[0] == 'r') ids.emplace_back(std::atol(name.c_str() + 1), name);
      }
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::string> out;
    for (auto& [n, s] : ids) out.push_back(s);
    return out;
  }
  std::string newRunId() const {
    long next = 1;
    for (const std::string& id : runIds()) next = std::max(next, std::atol(id.c_str() + 1) + 1);
    return "r" + std::to_string(next);
  }

  void save(const Run& r) const {
    std::string t = "run " + r.id + "\nparent " + r.parent + "\ninput " + r.input + "\nstatus " +
                    r.status + "\nstate " + r.state + "\n";
    for (const StageRec& s : r.stages) {
      char ms[32];
      std::snprintf(ms, sizeof ms, "%.3f", s.ms);
      t += "stage " + std::to_string(s.index) + " " + s.status + " " + s.before + " " + s.after +
           " " + ms + " +" + std::to_string(s.added) + " -" + std::to_string(s.removed) + " ~" +
           std::to_string(s.changed) + " " + s.what + "\n";
    }
    writeText(runDir(r.id) / "run.txt", t);
    std::string p = "# passes still to run, one per line; edit, then `brrr resume " + r.id + "`\n";
    for (const Stage& s : r.pending) p += stageLine(s) + "\n";
    writeText(runDir(r.id) / "pipeline.txt", p);
  }

  bool load(const std::string& id, Run* r, std::string* err) const {
    std::string t;
    if (!readText(runDir(id) / "run.txt", &t)) {
      *err = "no such run '" + id + "' in " + root_.string();
      return false;
    }
    std::istringstream in(t);
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream ls(line);
      std::string key;
      ls >> key;
      if (key == "stage") {
        StageRec s;
        std::string ms, a, rm, c;
        ls >> s.index >> s.status >> s.before >> s.after >> ms >> a >> rm >> c;
        s.ms = std::atof(ms.c_str());
        s.added = std::strtoul(a.c_str() + 1, nullptr, 10);
        s.removed = std::strtoul(rm.c_str() + 1, nullptr, 10);
        s.changed = std::strtoul(c.c_str() + 1, nullptr, 10);
        std::getline(ls, s.what);
        s.what = trim(s.what);
        r->stages.push_back(s);
        continue;
      }
      std::string rest;
      std::getline(ls, rest);
      rest = trim(rest);
      if (key == "run") r->id = rest;
      if (key == "parent") r->parent = rest;
      if (key == "input") r->input = rest;
      if (key == "status") r->status = rest;
      if (key == "state") r->state = rest;
    }
    std::string p;
    readText(runDir(id) / "pipeline.txt", &p);
    return parseStages(p, '\n', &r->pending, err);
  }

 private:
  fs::path root_;
};

// ---- stages -------------------------------------------------------------------

std::string nodeSignature(const Node& n) {
  std::string s = n.kind + "|" + n.symbol + "|" + std::to_string(n.scope) + "|";
  for (const auto& x : n.names) s += x + ",";
  for (const auto& t : n.types) s += (t ? printType(t) : "-") + ",";
  for (const Operand& o : n.operands) {
    std::vector<ValueRef> us;
    o.uses(&us);
    s += std::to_string(static_cast<int>(o.kind)) + ":";
    for (const auto& u : us) s += valueAtom(u);
    for (const auto& e : o.exprs) s += symbolic::printExpr(e);
    s += ",";
  }
  for (const Successor& x : n.successors) {
    s += std::to_string(x.block) + "(";
    for (const auto& a : x.args) s += valueAtom(a);
    s += ")";
  }
  for (const auto& [k, v] : n.attrs) s += k + "=" + v.toString() + ",";
  return s;
}

void diff(const Module& a, const Module& b, StageRec* rec) {
  std::map<NodeId, std::string> sa, sb;
  for (const Node& n : a.nodes) sa[n.id] = nodeSignature(n);
  for (const Node& n : b.nodes) sb[n.id] = nodeSignature(n);
  for (const auto& [id, sig] : sb) {
    auto it = sa.find(id);
    if (it == sa.end()) ++rec->added;
    else if (it->second != sig) ++rec->changed;
  }
  for (const auto& [id, sig] : sa) rec->removed += !sb.count(id);
}

std::string issueLine(const verify::Issue& is) {
  return std::string(is.error ? "error" : "warning") + " n" + std::to_string(is.node) + " (" +
         is.kind + "): " + is.message;
}

class Runner {
 public:
  Runner(Store& store, const schema::Registry& reg, const Options& opts, std::ostream& out)
      : store_(store), reg_(reg), opts_(opts), out_(out) {}

  // Verify `after`, record the stage, and advance the run when it holds.
  bool finish(Run& run, StageRec rec, const Module& before, const Module& after, bool pass_ok,
              const std::string& pass_err) {
    std::vector<verify::Issue> issues = sema::verifyModule(after, reg_);
    const std::string text = printModule(after, {true});
    const std::string hash = store_.put(text);
    std::string failure = pass_ok ? "" : "the pass failed: " + pass_err;
    if (failure.empty()) {
      ParseResult back = parseModule(text);
      if (!back.ok || printModule(back.module, {true}) != text) {
        failure = "the state does not round-trip through text (a printer/parser bug)";
      }
    }
    size_t errors = 0, warnings = 0;
    for (const auto& is : issues) (is.error ? errors : warnings)++;
    if (failure.empty() && errors) failure = std::to_string(errors) + " verification error(s)";

    diff(before, after, &rec);
    rec.status = failure.empty() ? "ok" : "failed";
    rec.after = failure.empty() ? hash : rec.before;
    if (failure.empty()) run.state = hash;
    run.stages.push_back(rec);

    char num[8];
    std::snprintf(num, sizeof num, "%02zu", rec.index);
    std::string name = rec.what.substr(0, rec.what.find(' '));
    std::string t = "stage " + std::to_string(rec.index) + "\nwhat " + rec.what + "\nstatus " +
                    rec.status + "\nbefore " + rec.before + "\nafter " + rec.after + "\n";
    if (!failure.empty()) t += "rejected " + hash + "\nreason " + failure + "\n";
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.3f", rec.ms);
    t += "time_ms " + std::string(ms) + "\ndiff +" + std::to_string(rec.added) + " -" +
         std::to_string(rec.removed) + " ~" + std::to_string(rec.changed) + "\n";
    for (const JournalEntry& j : after.journal) {
      t += "journal " + j.action + " n" + std::to_string(j.id) + ": " + j.detail + "\n";
    }
    for (const auto& is : issues) t += "issue " + issueLine(is) + "\n";
    writeText(store_.runDir(run.id) / (std::string(num) + "-" + name + ".txt"), t);

    out_ << "  [" << rec.index << "] " << rec.what << "  " << rec.status << "  " << ms << " ms  +"
         << rec.added << " -" << rec.removed << " ~" << rec.changed;
    if (failure.empty()) {
      out_ << "  state " << hash.substr(0, 8);
      if (warnings) out_ << "  (" << warnings << " warning(s))";
      out_ << "\n";
    } else {
      out_ << "  rejected " << hash.substr(0, 8) << ": " << failure << "\n";
    }
    for (const auto& is : issues) {
      if (is.error || !failure.empty()) out_ << "      " << issueLine(is) << "\n";
    }
    last_warnings_ = warnings;
    return failure.empty();
  }

  bool edit(Run& run, const std::string& file) {
    std::string base_text, edited_text;
    store_.get(run.state, &base_text);
    if (!readText(file, &edited_text)) {
      out_ << "error: cannot read " << file << "\n";
      return false;
    }
    ParseResult base = parseModule(base_text);
    ParseResult edited = parseModule(edited_text);
    if (!edited.ok) {
      out_ << file << ":" << edited.line << ": error: " << edited.error << "\n";
      return false;
    }
    std::string report;
    Module m = reconcileIds(base.module, edited.module, &report);
    m.log("edit", kInvalidId, file + ": " + report);
    StageRec rec;
    rec.index = run.stages.size();
    rec.before = run.state;
    rec.what = "edit " + file;
    return finish(run, rec, base.module, m, true, "");
  }

  // Run pending passes (up to --steps); stop on failure or a requested pause.
  void execute(Run& run) {
    int done = 0;
    bool failed = false;
    while (!run.pending.empty() && (opts_.steps < 0 || done < opts_.steps)) {
      const Stage st = run.pending.front();
      std::string text;
      store_.get(run.state, &text);
      ParseResult pr = parseModule(text);
      Module m = pr.module;
      StageRec rec;
      rec.index = run.stages.size();
      rec.before = run.state;
      rec.what = stageLine(st);
      std::string perr;
      bool ok = false;
      const auto t0 = std::chrono::steady_clock::now();
      if (const passes::PassInfo* p = passes::find(st.pass)) {
        ok = p->run(m, passes::Context{reg_, st.options}, &perr);
      } else {
        perr = "unknown pass '" + st.pass + "' (see `brrr passes`)";
      }
      rec.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!finish(run, rec, pr.module, m, ok, perr)) {
        failed = true;
        break;
      }
      run.pending.erase(run.pending.begin());
      ++done;
      if (opts_.pause_on_warning && last_warnings_ > 0) break;
    }
    run.status = failed ? "failed" : run.pending.empty() ? "done" : "paused";
  }

  void summary(const Run& run) {
    const size_t at = run.stages.empty() ? 0 : run.stages.back().index;
    if (run.status == "done") {
      out_ << "status: done — final state " << run.id << "@" << at << " ("
           << store_.objectPath(run.state).generic_string() << ")\n";
      return;
    }
    const std::string dir = store_.runDir(run.id).generic_string();
    if (run.status == "failed") {
      out_ << "status: failed at stage " << at << "; the state is unchanged (" << run.id << "@"
           << at << " = " << run.id << "@" << (at ? at - 1 : 0) << "). Next:\n";
    } else {
      out_ << "status: paused after stage " << at << ", " << run.pending.size()
           << " pass(es) pending. Next:\n";
    }
    out_ << "  brrr show " << run.id << "@" << at << "          # look at the state\n"
         << "  edit " << dir << "/pipeline.txt, then: brrr resume " << run.id << "\n"
         << "  brrr resume " << run.id << " --state=<edited.br>   # change the IR by hand\n"
         << "  brrr branch " << run.id << "@<k> --passes=<a,b>     # try another way\n";
  }

 private:
  Store& store_;
  const schema::Registry& reg_;
  const Options& opts_;
  std::ostream& out_;
  size_t last_warnings_ = 0;
};

bool pendingFromOptions(const Options& o, std::vector<Stage>* out, bool* given, std::string* err) {
  *given = false;
  if (!o.pipeline_file.empty()) {
    std::string text;
    if (!readText(o.pipeline_file, &text)) {
      *err = "cannot read " + o.pipeline_file;
      return false;
    }
    *given = true;
    return parseStages(text, '\n', out, err);
  }
  if (!o.passes.empty()) {
    *given = true;
    return parseStages(o.passes, ',', out, err);
  }
  return true;
}

bool parseRef(const std::string& s, std::string* run, long* stage) {
  const size_t at = s.find('@');
  *run = s.substr(0, at);
  *stage = -1;
  if (at == std::string::npos) return true;
  const std::string k = s.substr(at + 1);
  if (k.empty() || k.find_first_not_of("0123456789") != std::string::npos) return false;
  *stage = std::atol(k.c_str());
  return true;
}

}  // namespace

bool isCommand(const std::string& cmd) {
  return cmd == "run" || cmd == "resume" || cmd == "branch" || cmd == "runs" || cmd == "show" ||
         cmd == "passes";
}

int command(const std::string& cmd, const std::vector<std::string>& args, const Options& opts,
            const schema::Registry& reg, const std::string& input_text, std::ostream& out,
            std::ostream& err) {
  Store store(opts.store);
  Runner runner(store, reg, opts, out);
  std::string e;

  if (cmd == "passes") {
    for (const passes::PassInfo& p : passes::registry()) {
      out << p.name << "\n    " << p.summary << "\n";
    }
    out << "(solver: " << symbolic::backendName() << ", " << symbolic::z3::version() << ")\n";
    return 0;
  }

  if (cmd == "runs") {
    for (const std::string& id : store.runIds()) {
      Run r;
      if (!store.load(id, &r, &e)) continue;
      out << r.id << "  " << r.status << "  " << r.stages.size() << " stage(s)  from " << r.parent
          << "  input " << r.input << "  pending " << r.pending.size() << "\n";
    }
    return 0;
  }

  if (cmd == "show") {
    if (args.empty()) {
      err << "usage: brrr show <run>[@stage] [--ids] [--record]\n";
      return 64;
    }
    std::string id;
    long k = -1;
    Run r;
    if (!parseRef(args[0], &id, &k) || !store.load(id, &r, &e)) {
      err << "error: " << (e.empty() ? "bad reference '" + args[0] + "'" : e) << "\n";
      return 1;
    }
    if (k >= static_cast<long>(r.stages.size())) {
      err << "error: " << id << " has stages 0.." << r.stages.size() - 1 << "\n";
      return 1;
    }
    const StageRec& s = r.stages[k < 0 ? r.stages.size() - 1 : k];
    if (opts.record) {
      char num[8];
      std::snprintf(num, sizeof num, "%02zu", s.index);
      std::string text;
      readText(store.runDir(id) / (std::string(num) + "-" + s.what.substr(0, s.what.find(' ')) + ".txt"),
               &text);
      out << text;
      return 0;
    }
    std::string text;
    store.get(s.after, &text);
    if (opts.ids) {
      out << text;
      return 0;
    }
    ParseResult pr = parseModule(text);
    out << printModule(pr.module);
    return 0;
  }

  if (cmd == "run") {
    if (args.empty()) {
      err << "usage: brrr run <file.br> --pipeline=<file> | --passes=<a,b>\n";
      return 64;
    }
    Run r;
    bool given = false;
    if (!pendingFromOptions(opts, &r.pending, &given, &e)) {
      err << "error: " << e << "\n";
      return 1;
    }
    if (!given) {
      err << "error: give a pipeline: --pipeline=<file> or --passes=<a,b> (see `brrr passes`)\n";
      return 64;
    }
    ParseResult pr = parseModule(input_text);
    if (!pr.ok) {
      err << args[0] << ":" << pr.line << ": error: " << pr.error << "\n";
      return 1;
    }
    const auto issues = sema::verifyModule(pr.module, reg);
    if (verify::hasErrors(issues)) {
      for (const auto& is : issues) {
        if (is.error) err << issueLine(is) << "\n";
      }
      err << "error: the input does not verify; nothing was run\n";
      return 1;
    }
    r.id = store.newRunId();
    r.input = args[0];
    r.state = store.put(printModule(pr.module, {true}));
    r.stages.push_back({0, "ok", "-", r.state, 0, pr.module.nodes.size(), 0, 0, "input " + args[0]});
    out << "run " << r.id << "\n  [0] input " << args[0] << "  state " << r.state.substr(0, 8) << "\n";
    runner.execute(r);
    store.save(r);
    runner.summary(r);
    return r.status == "failed" ? 2 : 0;
  }

  if (cmd == "resume" || cmd == "branch") {
    if (args.empty()) {
      err << "usage: brrr " << cmd << (cmd == "branch" ? " <run>@<stage>" : " <run>")
          << " [--pipeline=..|--passes=..] [--state=<edited.br>] [--steps=N]\n";
      return 64;
    }
    std::string id;
    long k = -1;
    Run src;
    if (!parseRef(args[0], &id, &k) || !store.load(id, &src, &e)) {
      err << "error: " << (e.empty() ? "bad reference '" + args[0] + "'" : e) << "\n";
      return 1;
    }
    Run r;
    if (cmd == "resume") {
      if (k >= 0) {
        err << "error: resume continues a run where it stopped; to restart from a stage use "
               "`brrr branch "
            << args[0] << "`\n";
        return 64;
      }
      r = src;
      out << "run " << r.id << " (resumed)\n";
    } else {
      if (k < 0 || k >= static_cast<long>(src.stages.size())) {
        err << "error: branch needs <run>@<stage> with stage in 0.." << src.stages.size() - 1 << "\n";
        return 64;
      }
      r.id = store.newRunId();
      r.parent = id + "@" + std::to_string(k);
      r.input = src.input;
      r.state = src.stages[k].after;
      r.stages.push_back({0, "ok", src.stages[k].after, r.state, 0, 0, 0, 0, "from " + r.parent});
      // By default the branch replays what came after stage k, then the pending passes.
      for (size_t i = k + 1; i < src.stages.size(); ++i) {
        const std::string& w = src.stages[i].what;
        if (w.rfind("edit ", 0) == 0) continue;
        Stage s;
        if (parseStage(w, &s, &e)) r.pending.push_back(s);
      }
      r.pending.insert(r.pending.end(), src.pending.begin(), src.pending.end());
      out << "run " << r.id << " (from " << r.parent << ")\n  [0] from " << r.parent
          << "  state " << r.state.substr(0, 8) << "\n";
    }
    std::vector<Stage> override_pending;
    bool given = false;
    if (!pendingFromOptions(opts, &override_pending, &given, &e)) {
      err << "error: " << e << "\n";
      return 1;
    }
    if (given) r.pending = override_pending;
    if (!opts.state_file.empty() && !runner.edit(r, opts.state_file)) {
      r.status = "failed";
      store.save(r);
      runner.summary(r);
      return 2;
    }
    runner.execute(r);
    store.save(r);
    runner.summary(r);
    return r.status == "failed" ? 2 : 0;
  }

  err << "unknown command '" << cmd << "'\n";
  return 64;
}

}  // namespace brrr::pipeline
