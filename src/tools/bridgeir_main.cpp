#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ir/ir.h"
#include "ir/text.h"
#include "passes/demo_fuse.h"

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
  std::cerr << "usage: bridgeir <print|roundtrip|demo> <file.br>\n"
               "  print      parse and print canonical text (stable ids)\n"
               "  roundtrip  parse -> print -> parse -> print; verify stability\n"
               "  demo       run the demo transform; print result + journal\n";
  return 64;
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
    for (const bridgeir::JournalEntry& e : pr.module.journal) {
      std::cerr << "[journal] " << e.action << " n" << e.id << ": " << e.detail
                << "\n";
    }
    return 0;
  }

  return usage();
}
