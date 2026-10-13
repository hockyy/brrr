#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ir/ir.h"
#include "ir/text.h"
#include "passes/demo_fuse.h"
#include "passes/fold_reshape.h"
#include "symbolic/facts.h"
#include "symbolic/symbolic.h"

namespace {

std::string readFile(const std::string& path, bool* ok, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *ok = false;
    *err = "cannot open " + path;
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *ok = true;
  return ss.str();
}

int usage() {
  std::cerr << "usage: bridgeir <command> <file.br> [args]\n"
               "  print         parse and print canonical text (stable ids + names)\n"
               "  roundtrip     parse -> print -> parse -> print; verify stability\n"
               "  demo          run the demo transform; print result + journal\n"
               "  rename        rename a node: rename <file> <nID|name> <newname|->\n"
               "  facts         apply and validate `fact` lines from the document\n"
               "  fold-reshape  FoldReshape-style decision from facts (first-hand)\n"
               "\n"
               "Every load validates the document's facts (part of the language).\n";
  return 64;
}

void printJournal(const bridgeir::Module& m) {
  for (const bridgeir::JournalEntry& e : m.journal) {
    std::cerr << "[journal] " << e.action << " n" << e.id << ": " << e.detail
              << "\n";
  }
}

// Facts are part of the language: building the store is consistency checking.
bool applyDocumentFacts(const bridgeir::Module& m, bridgeir::symbolic::Solver* s,
                        const std::string& path) {
  std::string err;
  for (const std::string& line : m.facts) {
    if (!bridgeir::symbolic::applyFact(line, *s, &err)) {
      std::cerr << path << ": error: " << err << "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string cmd = argv[1];
  const std::string path = argv[2];

  bool ok = false;
  std::string err;
  const std::string text = readFile(path, &ok, &err);
  if (!ok) {
    std::cerr << "error: " << err << "\n";
    return 66;
  }

  bridgeir::ParseResult pr = bridgeir::parseModule(text);
  if (!pr.ok) {
    std::cerr << path << ":" << pr.line << ": error: " << pr.error << "\n";
    return 1;
  }

  // Language-level validation: a document with contradictory facts is ill-formed
  // for every command, not just the ones that run passes.
  bridgeir::symbolic::Solver facts;
  if (!applyDocumentFacts(pr.module, &facts, path)) return 1;

  if (cmd == "print") {
    std::cout << bridgeir::printModule(pr.module);
    return 0;
  }

  if (cmd == "roundtrip") {
    const std::string a = bridgeir::printModule(pr.module);
    bridgeir::ParseResult pr2 = bridgeir::parseModule(a);
    if (!pr2.ok) {
      std::cerr << "roundtrip reparse failed: " << pr2.error << "\n";
      return 2;
    }
    const std::string b = bridgeir::printModule(pr2.module);
    if (a != b) {
      std::cerr << "roundtrip mismatch: id/text stability violated\n";
      return 3;
    }
    std::cout << a;
    std::cerr << "roundtrip ok: " << pr2.module.nodes.size()
              << " nodes, ids stable\n";
    return 0;
  }

  if (cmd == "demo") {
    bridgeir::demoInsertReluAfterAdd(pr.module);
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  if (cmd == "rename") {
    if (argc < 5) return usage();
    const std::string target_str = argv[3];
    std::string new_name = argv[4];
    if (new_name == "-") new_name.clear();

    bridgeir::NodeId target = bridgeir::kInvalidId;
    bridgeir::NodeId as_id = 0;
    if (bridgeir::parseNodeIdToken(target_str, &as_id)) {
      target = as_id;
    } else {
      int matches = 0;
      for (const bridgeir::Node& n : pr.module.nodes) {
        if (n.name == target_str) {
          target = n.id;
          ++matches;
        }
      }
      if (matches != 1) {
        std::cerr << "error: " << (matches == 0 ? "unknown" : "ambiguous")
                  << " name '" << target_str << "'\n";
        return 1;
      }
    }

    std::string rerr;
    if (!pr.module.renameNode(target, new_name, &rerr)) {
      std::cerr << "error: " << rerr << "\n";
      return 1;
    }
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  if (cmd == "facts") {
    for (const std::string& line : pr.module.facts) {
      std::cout << "ok: " << line << "\n";
    }
    std::cerr << pr.module.facts.size() << " facts, all consistent\n";
    return 0;
  }

  if (cmd == "fold-reshape") {
    bridgeir::foldLooseReshapePairs(pr.module, facts);
    std::cout << bridgeir::printModule(pr.module);
    printJournal(pr.module);
    return 0;
  }

  return usage();
}
