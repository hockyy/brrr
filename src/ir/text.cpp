#include "ir/text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "symbolic/expr_io.h"
#include "symbolic/facts.h"

namespace brrr {
namespace {

// ===========================================================================
// Lexer
// ===========================================================================

struct Token {
  enum class K { Eof, Newline, Ident, Int, Float, String, Value, Label, Symbol, Punct };
  K k = K::Eof;
  std::string s;
  size_t begin = 0, end = 0;  // source offsets; a Newline begins at its comment
  int line = 1;
};

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Newlines end statements, except inside ( ) and [ ].
bool lex(const std::string& src, std::vector<Token>* out, std::string* err,
         int* err_line) {
  size_t i = 0;
  int line = 1;
  int depth = 0;
  size_t comment_at = std::string::npos;
  auto push = [&](Token::K k, size_t b, size_t e) {
    out->push_back({k, src.substr(b, e - b), b, e, line});
  };
  auto newline = [&](size_t at) {
    if (depth == 0 && !out->empty() && out->back().k != Token::K::Newline) {
      Token t;
      t.k = Token::K::Newline;
      t.begin = comment_at != std::string::npos ? comment_at : at;
      t.end = at;
      t.line = line;
      out->push_back(t);
    }
    comment_at = std::string::npos;
  };
  const size_t n = src.size();
  while (i < n) {
    const char c = src[i];
    if (c == '\n') {
      newline(i);
      ++line;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '#') {
      comment_at = i;
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    const size_t b = i;
    if (c == '%') {
      ++i;
      while (i < n && identChar(src[i])) ++i;
      if (i < n && src[i] == '#') {
        ++i;
        while (i < n && isDigit(src[i])) ++i;
      }
      if (i == b + 1) {
        *err = "'%' must be followed by a value name";
        *err_line = line;
        return false;
      }
      push(Token::K::Value, b, i);
      continue;
    }
    if (c == '^' || c == '@') {
      ++i;
      while (i < n && identChar(src[i])) ++i;
      if (i == b + 1) {
        *err = std::string("'") + c + "' must be followed by a name";
        *err_line = line;
        return false;
      }
      push(c == '^' ? Token::K::Label : Token::K::Symbol, b, i);
      continue;
    }
    if (c == '"') {
      ++i;
      while (i < n && src[i] != '"') {
        if (src[i] == '\\') ++i;
        if (i < n && src[i] == '\n') break;
        ++i;
      }
      if (i >= n || src[i] != '"') {
        *err = "unterminated string";
        *err_line = line;
        return false;
      }
      ++i;
      push(Token::K::String, b, i);
      continue;
    }
    if (isDigit(c)) {
      while (i < n && isDigit(src[i])) ++i;
      bool is_float = false;
      if (i + 1 < n && src[i] == '.' && isDigit(src[i + 1])) {
        is_float = true;
        ++i;
        while (i < n && isDigit(src[i])) ++i;
      }
      if (i < n && (src[i] == 'e' || src[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (src[j] == '+' || src[j] == '-')) ++j;
        if (j < n && isDigit(src[j])) {
          is_float = true;
          i = j;
          while (i < n && isDigit(src[i])) ++i;
        }
      }
      push(is_float ? Token::K::Float : Token::K::Int, b, i);
      continue;
    }
    if (identStart(c)) {
      while (i < n && identChar(src[i])) ++i;
      push(Token::K::Ident, b, i);
      continue;
    }
    static const char* const kTwo[] = {"->", "<=", ">=", "=="};
    bool two = false;
    for (const char* t : kTwo) {
      if (src.compare(i, 2, t) == 0) {
        push(Token::K::Punct, i, i + 2);
        i += 2;
        two = true;
        break;
      }
    }
    if (two) continue;
    if (std::strchr("()[]{}<>,:=+-*?", c)) {
      if (c == '(' || c == '[') ++depth;
      if ((c == ')' || c == ']') && depth > 0) --depth;
      push(Token::K::Punct, i, i + 1);
      ++i;
      continue;
    }
    *err = c == '/' ? "integer division must say how it rounds: use floordiv or ceildiv"
                    : std::string("unexpected character '") + c + "'";
    *err_line = line;
    return false;
  }
  depth = 0;
  newline(n);
  Token eof;
  eof.begin = eof.end = n;
  eof.line = line;
  out->push_back(eof);
  return true;
}

std::string unescape(const std::string& quoted) {
  std::string v;
  for (size_t i = 1; i + 1 < quoted.size(); ++i) {
    if (quoted[i] == '\\' && i + 2 < quoted.size()) ++i;
    v += quoted[i];
  }
  return v;
}

bool isIdToken(const Token& t, NodeId* id) {
  return t.k == Token::K::Ident && parseNodeIdToken(t.s, id);
}

// `%12` / `%12#1` result spellings fix a node id.
bool numericValue(const std::string& tok, NodeId* id, uint32_t* port) {
  ValueRef v;
  if (!parseValueAtom(tok, &v)) return false;
  *id = v.id;
  *port = v.port;
  return true;
}

// ===========================================================================
// Parser
// ===========================================================================

struct PendingSucc {
  NodeId node = kInvalidId;
  size_t index = 0;
  std::string label;
  std::vector<symbolic::Expr> args;
  int line = 0;
};

class Parser {
 public:
  Parser(const std::string& src, std::vector<Token> toks)
      : src_(src), toks_(std::move(toks)) {}

  ParseResult run() {
    res_.ok = true;
    // Reserve every id the text mentions, so fresh ids never collide with one
    // that appears later.
    for (const Token& t : toks_) {
      NodeId id = kInvalidId;
      uint32_t port = 0;
      if (isIdToken(t, &id) ||
          (t.k == Token::K::Value && numericValue(t.s, &id, &port))) {
        m().reserveId(id);
      }
    }
    if (parseBody(kInvalidId, /*braced=*/false)) resolve();
    return std::move(res_);
  }

 private:
  Module& m() { return res_.module; }
  const Token& peek(size_t k = 0) const {
    return toks_[std::min(p_ + k, toks_.size() - 1)];
  }
  const Token& next() {
    const Token& t = toks_[p_];
    if (p_ + 1 < toks_.size()) ++p_;
    return t;
  }
  static bool isP(const Token& t, const char* s) {
    return t.k == Token::K::Punct && t.s == s;
  }
  bool accept(const char* s) {
    if (!isP(peek(), s)) return false;
    next();
    return true;
  }
  bool fail(const std::string& msg, int line = 0) {
    if (res_.ok) {
      res_.ok = false;
      res_.error = msg;
      res_.line = line ? line : peek().line;
    }
    return false;
  }
  bool expect(const char* s, const std::string& what) {
    if (accept(s)) return true;
    return fail("expected '" + std::string(s) + "' " + what + ", got '" +
                (peek().k == Token::K::Newline ? "end of line" : peek().s) + "'");
  }
  void skipNewlines() {
    while (peek().k == Token::K::Newline) next();
  }
  bool atStatementEnd() const {
    const Token& t = peek();
    return t.k == Token::K::Newline || t.k == Token::K::Eof || isP(t, "}");
  }
  bool attrStart() const {
    return peek().k == Token::K::Ident && isP(peek(1), "=");
  }

  NodeId freshOr(NodeId explicit_id) {
    return explicit_id != kInvalidId ? explicit_id : m().allocId();
  }
  bool claim(NodeId id) {
    if (m().contains(id) || claimed_.count(id)) {
      return fail("duplicate node id n" + std::to_string(id));
    }
    claimed_.insert(id);
    return true;
  }
  Node& add(Node n, int line) {
    lines_[n.id] = line;
    return m().append(std::move(n));
  }

  // Optional `nID` prefix (stored form).
  NodeId takeId() {
    NodeId id = kInvalidId;
    if (isIdToken(peek(), &id) && peek(1).k != Token::K::Newline &&
        !isP(peek(1), "=") && !isP(peek(1), ",")) {
      next();
      return id;
    }
    return kInvalidId;
  }

  // ---- bodies --------------------------------------------------------------

  bool parseBody(NodeId owner, bool braced) {
    NodeId cur = owner;  // ops before the first label belong to the owner
    const Node* on = owner ? m().find(owner) : nullptr;
    const bool in_function = on && on->kind != "module";
    while (res_.ok) {
      skipNewlines();
      if (isP(peek(), "}")) {
        if (!braced) return fail("unmatched '}'");
        next();
        return true;
      }
      if (peek().k == Token::K::Eof) {
        if (braced) return fail("unterminated '{' (missing '}')");
        return true;
      }
      const int line = peek().line;
      const NodeId id = takeId();
      const Token& t = peek();
      if (t.k == Token::K::Ident && t.s == "fact" && id == kInvalidId) {
        parseFact(cur);
        continue;
      }
      if (t.k == Token::K::Ident && (t.s == "func" || t.s == "module")) {
        if (in_function) return fail("'" + t.s + "' is only allowed at module level");
        if (t.s == "func" ? !parseFunc(owner, id, line) : !parseModuleItem(owner, id, line)) {
          return false;
        }
        continue;
      }
      if (t.k == Token::K::Label) {
        if (!in_function) return fail("a block label is only allowed inside a function or region");
        NodeId block = kInvalidId;
        if (!parseLabel(owner, id, line, &block)) return false;
        cur = block;
        continue;
      }
      if (!parseOp(cur, id, line)) return false;
    }
    return false;
  }

  void parseFact(NodeId scope) {
    const Token& kw = next();
    size_t q = p_;
    while (toks_[q].k != Token::K::Newline && toks_[q].k != Token::K::Eof) ++q;
    const std::string text = src_.substr(kw.end, toks_[q].begin - kw.end);
    symbolic::ParsedFact pf;
    std::string err;
    if (!symbolic::parseFact(text, &pf, &err)) {
      fail("bad fact: " + err, kw.line);
      return;
    }
    m().facts.push_back({scope, text});
    fact_lines_.push_back(kw.line);
    p_ = q;
  }

  bool parseFunc(NodeId scope, NodeId id, int line) {
    next();  // func
    if (peek().k != Token::K::Symbol) return fail("expected @name after 'func'");
    Node f;
    f.kind = "func";
    f.symbol = next().s.substr(1);
    f.scope = scope;
    f.is_scope = true;
    f.id = freshOr(id);
    if (!claim(f.id)) return false;
    std::vector<std::pair<Node, int>> args;
    if (!parseArgs(&args)) return false;
    skipNewlines();
    if (accept("->")) {
      do {
        Type t;
        if (!parseType(&t)) return false;
        f.types.push_back(t);
      } while (accept(","));
    }
    if (!expect("{", "to open the function body")) return false;
    const NodeId fid = f.id;
    add(std::move(f), line);
    for (auto& [a, aline] : args) {
      a.scope = fid;
      add(std::move(a), aline);
    }
    return parseBody(fid, /*braced=*/true);
  }

  bool parseModuleItem(NodeId scope, NodeId id, int line) {
    next();  // module
    if (peek().k != Token::K::Symbol) return fail("expected @name after 'module'");
    Node mod;
    mod.kind = "module";
    mod.symbol = next().s.substr(1);
    mod.scope = scope;
    mod.is_scope = true;
    if (!expect("{", "to open the module body")) return false;
    mod.id = freshOr(id);
    if (!claim(mod.id)) return false;
    const NodeId mid = mod.id;
    add(std::move(mod), line);
    return parseBody(mid, /*braced=*/true);
  }

  // `(` [nID] %x [: T] (, ...)* `)` — arguments of a function or block.
  bool parseArgs(std::vector<std::pair<Node, int>>* out) {
    if (!expect("(", "to open the argument list")) return false;
    if (accept(")")) return true;
    do {
      const int line = peek().line;
      NodeId id = takeId();
      if (peek().k != Token::K::Value) return fail("expected an argument like %x: i64");
      const std::string v = next().s;
      Node a;
      a.kind = "arg";
      NodeId nid = kInvalidId;
      uint32_t port = 0;
      if (numericValue(v, &nid, &port)) {
        if (port != 0) return fail("an argument has a single value; '" + v + "' selects a port");
        if (id != kInvalidId && id != nid) return fail("id n" + std::to_string(id) + " disagrees with " + v);
        id = nid;
        a.names.push_back("");
      } else {
        const std::string name = v.substr(1);
        if (!isValidName(name)) return fail("invalid value name '" + v + "'");
        a.names.push_back(name);
      }
      a.types.push_back(nullptr);
      if (accept(":")) {
        if (!parseType(&a.types[0])) return false;
      }
      a.id = freshOr(id);
      if (!claim(a.id)) return false;
      out->push_back({std::move(a), line});
    } while (accept(","));
    return expect(")", "to close the argument list");
  }

  bool parseLabel(NodeId owner, NodeId id, int line, NodeId* block) {
    Node b;
    b.kind = "block";
    b.symbol = next().s.substr(1);
    b.scope = owner;
    b.is_scope = true;
    b.id = freshOr(id);
    if (!claim(b.id)) return false;
    std::vector<std::pair<Node, int>> args;
    if (isP(peek(), "(") && !parseArgs(&args)) return false;
    if (!expect(":", "after the block label")) return false;
    if (!atStatementEnd()) return fail("a block label must end its line");
    *block = b.id;
    add(std::move(b), line);
    for (auto& [a, aline] : args) {
      a.scope = *block;
      add(std::move(a), aline);
    }
    return true;
  }

  // ---- ops -----------------------------------------------------------------

  bool parseOp(NodeId scope, NodeId id, int line) {
    Node n;
    n.scope = scope;
    // Results: `%a, %12#1 =`
    if (peek().k == Token::K::Value) {
      NodeId from_names = kInvalidId;
      while (true) {
        if (peek().k != Token::K::Value) return fail("expected a result like %x");
        const std::string v = next().s;
        NodeId nid = kInvalidId;
        uint32_t port = 0;
        if (numericValue(v, &nid, &port)) {
          if (port != n.names.size()) {
            return fail("result " + std::to_string(n.names.size()) + " is spelled '" + v + "'");
          }
          if (from_names != kInvalidId && from_names != nid) {
            return fail("results of one op must share an id ('" + v + "')");
          }
          from_names = nid;
          n.names.push_back("");
        } else {
          const std::string name = v.substr(1);
          if (!isValidName(name)) return fail("invalid value name '" + v + "'");
          n.names.push_back(name);
        }
        if (!accept(",")) break;
      }
      if (!expect("=", "after the results")) return false;
      if (from_names != kInvalidId) {
        if (id != kInvalidId && id != from_names) {
          return fail("id n" + std::to_string(id) + " disagrees with the result names");
        }
        id = from_names;
      }
    }
    if (peek().k != Token::K::Ident) return fail("expected an op kind");
    {
      NodeId dummy;
      if (parseNodeIdToken(peek().s, &dummy)) return fail("'" + peek().s + "' is an id, not an op kind");
    }
    n.kind = next().s;

    if (n.kind == "const") {
      AttrValue v;
      if (!parseLiteral(&v)) return false;
      n.attrs["value"] = std::move(v);
    } else if (!atStatementEnd() && !isP(peek(), ":") && !isP(peek(), "{") && !attrStart()) {
      do {
        if (!parseOperand(&n, line)) return false;
      } while (accept(","));
    }
    while (attrStart()) {
      const std::string key = next().s;
      next();  // =
      if (n.attrs.count(key)) return fail("duplicate attribute '" + key + "'");
      AttrValue v;
      if (!parseLiteral(&v)) return false;
      n.attrs[key] = std::move(v);
    }
    if (accept(":")) {
      do {
        Type t;
        if (!parseType(&t)) return false;
        n.types.push_back(t);
      } while (accept(","));
      if (n.types.size() != n.names.size()) {
        return fail(std::to_string(n.types.size()) + " type(s) for " +
                    std::to_string(n.names.size()) + " result(s)");
      }
    }
    const bool region = isP(peek(), "{");
    if (!region && !atStatementEnd()) {
      return fail("unexpected '" + peek().s + "' (attributes come after operands, as key=value)");
    }
    n.id = freshOr(id);
    if (!claim(n.id)) return false;
    n.is_scope = region;
    const NodeId nid = n.id;
    for (PendingSucc& ps : pending_here_) ps.node = nid;
    succ_.insert(succ_.end(), pending_here_.begin(), pending_here_.end());
    pending_here_.clear();
    add(std::move(n), line);
    if (region) {
      next();  // {
      return parseBody(nid, /*braced=*/true);
    }
    return true;
  }

  bool parseOperand(Node* n, int line) {
    if (peek().k == Token::K::Label) {
      PendingSucc ps;
      ps.label = next().s.substr(1);
      ps.index = n->successors.size();
      ps.line = line;
      if (accept("(")) {
        if (!accept(")")) {
          do {
            symbolic::Expr e;
            if (!parseExprUntil(&e)) return false;
            ps.args.push_back(e);
          } while (accept(","));
          if (!expect(")", "to close the successor arguments")) return false;
        }
      }
      n->successors.emplace_back();
      pending_here_.push_back(ps);
      return true;
    }
    if (!n->successors.empty()) return fail("operands come before successors");
    if (accept("[")) {
      std::vector<symbolic::Expr> es;
      if (!accept("]")) {
        do {
          symbolic::Expr e;
          if (!parseExprUntil(&e)) return false;
          es.push_back(e);
        } while (accept(","));
        if (!expect("]", "to close the list")) return false;
      }
      n->operands.push_back(Operand::ofList(std::move(es)));
      return true;
    }
    symbolic::Expr e;
    if (!parseExprUntil(&e)) return false;
    n->operands.push_back(Operand::ofExpr(e));  // single values are unwrapped in resolve()
    return true;
  }

  // An expression runs to the next top-level `,` `]` `)` `:` `{` `}`, end of line,
  // or attribute (`key=`).
  bool parseExprUntil(symbolic::Expr* out) {
    const size_t start = p_;
    int depth = 0;
    while (true) {
      const Token& t = peek();
      if (t.k == Token::K::Newline || t.k == Token::K::Eof) break;
      if (depth == 0 && (isP(t, ",") || isP(t, "]") || isP(t, ")") || isP(t, ":") ||
                         isP(t, "{") || isP(t, "}") || attrStart())) {
        break;
      }
      if (isP(t, "(")) ++depth;
      if (isP(t, ")")) --depth;
      next();
    }
    if (p_ == start) return fail("expected an expression, got '" + peek().s + "'");
    const std::string text = src_.substr(toks_[start].begin, toks_[p_ - 1].end - toks_[start].begin);
    std::string err;
    *out = symbolic::parseExpr(text, &err);
    if (!*out) return fail("bad expression '" + text + "': " + err, toks_[start].line);
    return true;
  }

  // type := ident | ident '<' '[' dims ']' ',' ident '>'
  bool parseType(Type* out) {
    if (peek().k != Token::K::Ident) return fail("expected a type");
    auto t = std::make_shared<TypeTerm>();
    t->name = next().s;
    if (accept("<")) {
      t->shaped = true;
      if (!expect("[", "to open the dims of '" + t->name + "'")) return false;
      if (!accept("]")) {
        do {
          if (isP(peek(), "?") && (isP(peek(1), ",") || isP(peek(1), "]"))) {
            next();
            t->dims.push_back(symbolic::ExprTerm::var("?"));
            continue;
          }
          symbolic::Expr e;
          if (!parseExprUntil(&e)) return false;
          t->dims.push_back(e);
        } while (accept(","));
        if (!expect("]", "to close the dims")) return false;
      }
      if (!expect(",", "before the element type")) return false;
      if (peek().k != Token::K::Ident) return fail("expected an element type");
      t->elem = next().s;
      if (!expect(">", "to close the type")) return false;
    }
    *out = t;
    return true;
  }

  bool parseLiteral(AttrValue* out) {
    const Token& t = peek();
    if (isP(t, "-") && (peek(1).k == Token::K::Int || peek(1).k == Token::K::Float)) {
      next();
      const Token& num = next();
      *out = num.k == Token::K::Int ? AttrValue::fromInt(-std::strtoll(num.s.c_str(), nullptr, 10))
                                    : AttrValue::fromFloat(-std::strtod(num.s.c_str(), nullptr));
      return true;
    }
    switch (t.k) {
      case Token::K::Int:
        *out = AttrValue::fromInt(std::strtoll(next().s.c_str(), nullptr, 10));
        return true;
      case Token::K::Float:
        *out = AttrValue::fromFloat(std::strtod(next().s.c_str(), nullptr));
        return true;
      case Token::K::String:
        *out = AttrValue::fromStr(unescape(next().s));
        return true;
      case Token::K::Ident: {
        if (t.s == "true" || t.s == "false") {
          *out = AttrValue::fromBool(next().s == "true");
          return true;
        }
        if (isP(peek(1), "<")) {
          AttrValue v;
          v.kind = AttrValue::Kind::Type;
          if (!parseType(&v.type)) return false;
          *out = std::move(v);
          return true;
        }
        *out = AttrValue::fromIdent(next().s);
        return true;
      }
      default:
        break;
    }
    if (accept("[")) {
      AttrValue arr;
      arr.kind = AttrValue::Kind::Array;
      if (!accept("]")) {
        do {
          AttrValue item;
          if (!parseLiteral(&item)) return false;
          arr.items.push_back(std::move(item));
        } while (accept(","));
        if (!expect("]", "to close the array")) return false;
      }
      *out = std::move(arr);
      return true;
    }
    if (accept("(")) {
      if (!parseLiteral(out)) return false;
      if (!expect(":", "in '(literal : type)'")) return false;
      if (!parseType(&out->ascribed)) return false;
      return expect(")", "to close '(literal : type)'");
    }
    return fail("expected a literal (number, string, true/false, name, [..], or (v : T))");
  }

  // ---- resolution ----------------------------------------------------------

  // Bind every atom: `%name` -> `%<id>`, check `%<id>` exists, keep module symbols
  // (bound by a module-level fact), reject anything else.
  void resolve() {
    Module& mod = m();
    std::map<NodeId, std::map<std::string, ValueRef>> names;
    std::map<NodeId, std::map<std::string, NodeId>> labels;
    for (const Node& n : mod.nodes) {
      const NodeId ns = mod.namingScope(n.scope);
      if (n.kind == "block" && !labels[ns].emplace(n.symbol, n.id).second) {
        fail("duplicate label '^" + n.symbol + "'", lines_[n.id]);
        return;
      }
      for (uint32_t p = 0; p < n.names.size(); ++p) {
        if (n.names[p].empty()) continue;
        if (!names[ns].emplace(n.names[p], ValueRef{n.id, p}).second) {
          fail("duplicate value name '%" + n.names[p] + "' (names are unique per function)",
               lines_[n.id]);
          return;
        }
      }
    }

    std::set<std::string> symbols;
    for (const Module::FactEntry& f : mod.facts) {
      const Node* s = mod.find(f.scope);
      if (f.scope != kInvalidId && !(s && s->kind == "module")) continue;
      symbolic::ParsedFact pf;
      std::string err;
      if (!symbolic::parseFact(f.text, &pf, &err)) continue;
      std::set<std::string> atoms;
      symbolic::collectFactVars(pf, &atoms);
      for (const std::string& a : atoms) {
        if (a[0] != '%' && a[0] != '?') symbols.insert(a);
      }
    }

    std::string err;
    auto lookup = [&](NodeId scope, const std::string& name, ValueRef* out) {
      for (NodeId ns = mod.namingScope(scope);;) {
        auto it = names.find(ns);
        if (it != names.end()) {
          auto jt = it->second.find(name);
          if (jt != it->second.end()) {
            *out = jt->second;
            return true;
          }
        }
        if (ns == kInvalidId) return false;
        const Node* nn = mod.find(ns);
        ns = nn ? mod.namingScope(nn->scope) : kInvalidId;
      }
    };
    auto canon = [&](NodeId scope, bool allow_fresh) {
      return [&, scope, allow_fresh](const std::string& a) -> std::string {
        if (!err.empty()) return a;
        if (a == "?") {
          if (!allow_fresh) err = "'?' (a fresh dimension) is only allowed in a type";
          return a;
        }
        if (a[0] == '%') {
          ValueRef v;
          if (parseValueAtom(a, &v)) {
            const Node* d = mod.find(v.id);
            if (!d) {
              err = "unknown value '" + a + "'";
            } else if (v.port >= d->numResults()) {
              err = "'" + a + "' does not exist (n" + std::to_string(v.id) + " has " +
                    std::to_string(d->numResults()) + " result(s))";
            }
            return a;
          }
          if (lookup(scope, a.substr(1), &v)) return valueAtom(v);
          err = "unknown value '" + a + "'";
          return a;
        }
        if (symbols.count(a)) return a;
        err = "unbound symbol '" + a + "' (use a %value, or bind it with a module-level fact)";
        return a;
      };
    };
    auto mapType = [&](const Type& t, NodeId scope) -> Type {
      if (!t || !t->shaped) return t;
      auto t2 = std::make_shared<TypeTerm>(*t);
      for (auto& d : t2->dims) d = symbolic::mapVars(d, canon(scope, true));
      return t2;
    };

    for (Node& n : mod.nodes) {
      const int line = lines_[n.id];
      // A function's return types see its arguments.
      const NodeId tscope = n.kind == "func" ? n.id : n.scope;
      for (Type& t : n.types) t = mapType(t, tscope);
      for (auto& [k, v] : n.attrs) {
        if (v.kind == AttrValue::Kind::Type) v.type = mapType(v.type, n.scope);
      }
      for (Operand& o : n.operands) {
        for (auto& e : o.exprs) e = symbolic::mapVars(e, canon(n.scope, false));
        ValueRef v;
        if (o.kind == Operand::Kind::Expr && o.exprs[0]->kind == symbolic::ExprTerm::Kind::Var &&
            parseValueAtom(o.exprs[0]->name, &v)) {
          o = Operand::ofValue(v);
        }
      }
      if (!err.empty()) {
        fail(err, line);
        return;
      }
    }

    for (const PendingSucc& ps : succ_) {
      Node* n = mod.find(ps.node);
      const NodeId ns = mod.namingScope(n->scope);
      auto it = labels[ns].find(ps.label);
      if (it == labels[ns].end()) {
        fail("unknown block '^" + ps.label + "'", ps.line);
        return;
      }
      Successor& s = n->successors[ps.index];
      s.block = it->second;
      for (const auto& e : ps.args) {
        const auto e2 = symbolic::mapVars(e, canon(n->scope, false));
        ValueRef v;
        if (!err.empty() || e2->kind != symbolic::ExprTerm::Kind::Var ||
            !parseValueAtom(e2->name, &v)) {
          fail(err.empty() ? "successor arguments must be values (got '" +
                                 symbolic::printExpr(e) + "')"
                           : err,
               ps.line);
          return;
        }
        s.args.push_back(v);
      }
    }

    for (size_t k = 0; k < mod.facts.size(); ++k) {
      Module::FactEntry& f = mod.facts[k];
      symbolic::ParsedFact pf;
      std::string perr;
      symbolic::parseFact(f.text, &pf, &perr);
      f.text = symbolic::printFact(symbolic::mapFactVars(pf, canon(f.scope, false)));
      if (!err.empty()) {
        fail(err, fact_lines_[k]);
        return;
      }
    }
  }

  const std::string& src_;
  std::vector<Token> toks_;
  size_t p_ = 0;
  ParseResult res_;
  std::set<NodeId> claimed_;
  std::map<NodeId, int> lines_;
  std::vector<int> fact_lines_;
  std::vector<PendingSucc> succ_;
  std::vector<PendingSucc> pending_here_;
};

// ===========================================================================
// Printer
// ===========================================================================

class Printer {
 public:
  Printer(const Module& m, const PrintOptions& o) : m_(m), o_(o) {
    for (const Node& n : m.nodes) {
      const NodeId ns = m.namingScope(n.scope);
      for (uint32_t p = 0; p < n.names.size(); ++p) {
        if (!n.names[p].empty()) names_[ns].emplace(n.names[p], ValueRef{n.id, p});
      }
    }
  }

  std::string run() {
    std::string out;
    facts(kInvalidId, "", &out);
    bool prev_scope = false;
    for (const NodeId id : m_.top_level) {
      const Node* n = m_.find(id);
      if (!n) continue;
      const bool is_def = n->kind == "func" || n->kind == "module";
      if (!out.empty() && (is_def || prev_scope)) out += "\n";
      item(*n, "", &out);
      prev_scope = is_def;
    }
    return out;
  }

 private:
  bool lookup(NodeId scope, const std::string& name, ValueRef* out) const {
    for (NodeId ns = m_.namingScope(scope);;) {
      auto it = names_.find(ns);
      if (it != names_.end()) {
        auto jt = it->second.find(name);
        if (jt != it->second.end()) {
          *out = jt->second;
          return true;
        }
      }
      if (ns == kInvalidId) return false;
      const Node* nn = m_.find(ns);
      ns = nn ? m_.namingScope(nn->scope) : kInvalidId;
    }
  }

  // `%7` prints as `%acc` when that name resolves back to the same value here.
  std::string atomName(NodeId scope, const std::string& atom) const {
    ValueRef v;
    if (!parseValueAtom(atom, &v)) return atom;
    const Node* n = m_.find(v.id);
    if (!n || v.port >= n->names.size() || n->names[v.port].empty()) return atom;
    ValueRef back;
    if (lookup(scope, n->names[v.port], &back) && back == v) return "%" + n->names[v.port];
    return atom;
  }
  AtomNamer namer(NodeId scope) const {
    return [this, scope](const std::string& a) { return atomName(scope, a); };
  }
  std::string value(NodeId scope, ValueRef v) const { return atomName(scope, valueAtom(v)); }
  std::string idPrefix(const Node& n) const {
    return o_.ids ? "n" + std::to_string(n.id) + " " : "";
  }
  std::string result(const Node& n, uint32_t p) const {
    if (!n.names[p].empty()) return "%" + n.names[p];
    return n.numResults() == 1 ? "%" + std::to_string(n.id) : valueAtom({n.id, p}) + (p == 0 ? "#0" : "");
  }

  void facts(NodeId scope, const std::string& indent, std::string* out) const {
    for (const Module::FactEntry& f : m_.facts) {
      if (f.scope != scope) continue;
      symbolic::ParsedFact pf;
      std::string err;
      if (!symbolic::parseFact(f.text, &pf, &err)) continue;
      *out += indent + "fact " + symbolic::printFact(pf, namer(scope)) + "\n";
    }
  }

  std::string args(const Node& owner) const {
    std::vector<const Node*> as;
    for (const NodeId c : m_.childrenOf(owner.id)) {
      const Node* a = m_.find(c);
      if (a && a->kind == "arg") as.push_back(a);
    }
    std::string s = "(";
    for (size_t k = 0; k < as.size(); ++k) {
      if (k) s += ", ";
      s += idPrefix(*as[k]) + result(*as[k], 0);
      if (as[k]->typeOf(0)) s += ": " + printType(as[k]->typeOf(0), namer(as[k]->scope));
    }
    return s + ")";
  }

  void item(const Node& n, const std::string& indent, std::string* out) const {
    if (n.kind == "func") {
      std::string head = indent + idPrefix(n) + "func @" + n.symbol + args(n);
      if (!n.types.empty()) {
        head += " -> ";
        for (size_t k = 0; k < n.types.size(); ++k) {
          if (k) head += ", ";
          head += printType(n.types[k], namer(n.id));
        }
      }
      *out += head + " {\n";
      body(n, indent + "  ", out);
      *out += indent + "}\n";
      return;
    }
    if (n.kind == "module") {
      *out += indent + idPrefix(n) + "module @" + n.symbol + " {\n";
      facts(n.id, indent + "  ", out);
      bool first = true;
      for (const NodeId c : m_.childrenOf(n.id)) {
        const Node* cn = m_.find(c);
        if (!cn) continue;
        if (!first && (cn->kind == "func" || cn->kind == "module")) *out += "\n";
        item(*cn, indent + "  ", out);
        first = false;
      }
      *out += indent + "}\n";
      return;
    }
    op(n, indent, out);
  }

  // The contents of a function or region: facts, entry ops, then labeled blocks.
  void body(const Node& owner, const std::string& indent, std::string* out) const {
    const std::string label_indent = indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    facts(owner.id, indent, out);
    for (const NodeId c : m_.childrenOf(owner.id)) {
      const Node* cn = m_.find(c);
      if (!cn || cn->kind == "arg") continue;
      if (cn->kind != "block") {
        op(*cn, indent, out);
        continue;
      }
      std::string head = label_indent + idPrefix(*cn) + "^" + cn->symbol;
      bool has_args = false;
      for (const NodeId a : m_.childrenOf(cn->id)) {
        const Node* an = m_.find(a);
        if (an && an->kind == "arg") has_args = true;
      }
      if (has_args) head += args(*cn);
      *out += head + ":\n";
      facts(cn->id, indent, out);
      for (const NodeId bc : m_.childrenOf(cn->id)) {
        const Node* bn = m_.find(bc);
        if (bn && bn->kind != "arg") op(*bn, indent, out);
      }
    }
  }

  void op(const Node& n, const std::string& indent, std::string* out) const {
    std::string line = indent + idPrefix(n);
    if (n.numResults() > 0) {
      for (uint32_t p = 0; p < n.numResults(); ++p) {
        if (p) line += ", ";
        line += result(n, p);
      }
      line += " = ";
    }
    line += n.kind;
    const AtomNamer nm = namer(n.scope);
    std::vector<std::string> parts;
    if (n.kind == "const") {
      if (const AttrValue* v = n.findAttr("value")) line += " " + v->toString(nm);
    }
    for (const Operand& o : n.operands) {
      if (o.kind == Operand::Kind::Value) {
        parts.push_back(value(n.scope, o.value));
      } else if (o.kind == Operand::Kind::Expr) {
        parts.push_back(symbolic::printExpr(o.exprs[0], nm));
      } else {
        std::string s = "[";
        for (size_t k = 0; k < o.exprs.size(); ++k) {
          if (k) s += ", ";
          s += symbolic::printExpr(o.exprs[k], nm);
        }
        parts.push_back(s + "]");
      }
    }
    for (const Successor& s : n.successors) {
      const Node* b = m_.find(s.block);
      std::string t = "^" + (b ? b->symbol : "n" + std::to_string(s.block));
      if (!s.args.empty()) {
        t += "(";
        for (size_t k = 0; k < s.args.size(); ++k) {
          if (k) t += ", ";
          t += value(n.scope, s.args[k]);
        }
        t += ")";
      }
      parts.push_back(t);
    }
    for (size_t k = 0; k < parts.size(); ++k) line += (k ? ", " : " ") + parts[k];
    for (const auto& [key, v] : n.attrs) {
      if (n.kind == "const" && key == "value") continue;
      line += " " + key + "=" + v.toString(nm);
    }
    bool typed = !n.types.empty() && n.types.size() == n.numResults();
    for (const Type& t : n.types) typed = typed && t != nullptr;
    if (typed) {
      line += " :";
      for (size_t k = 0; k < n.types.size(); ++k) {
        line += (k ? ", " : " ") + printType(n.types[k], nm);
      }
    }
    if (n.is_scope) {
      *out += line + " {\n";
      body(n, indent + "  ", out);
      *out += indent + "}\n";
      return;
    }
    *out += line + "\n";
  }

  const Module& m_;
  PrintOptions o_;
  std::map<NodeId, std::map<std::string, ValueRef>> names_;
};

}  // namespace

std::string printModule(const Module& m, const PrintOptions& opts) {
  return Printer(m, opts).run();
}

ParseResult parseModule(const std::string& text) {
  std::vector<Token> toks;
  ParseResult res;
  if (!lex(text, &toks, &res.error, &res.line)) return res;
  return Parser(text, std::move(toks)).run();
}

// ===========================================================================
// Identity reconciliation
// ===========================================================================

namespace {

// A key that names a definition independently of its id (see text.h).
std::map<NodeId, std::string> definitionKeys(const Module& m) {
  std::map<NodeId, std::string> key;
  std::map<std::pair<NodeId, std::string>, int> ordinal;  // (scope, kind) -> count
  std::map<NodeId, int> arg_index;
  auto keyOf = [&](NodeId id) -> std::string {
    auto it = key.find(id);
    return it == key.end() ? std::string() : it->second;
  };
  for (const Node& n : m.nodes) {  // program order: scopes precede their contents
    const std::string parent = keyOf(n.scope);
    if (n.kind == "func" || n.kind == "module") {
      key[n.id] = parent + "/@" + n.symbol;
    } else if (n.kind == "block") {
      key[n.id] = parent + "/^" + n.symbol;
    } else if (n.kind == "arg") {
      key[n.id] = parent + "/arg" + std::to_string(arg_index[n.scope]++);
    } else if (!n.names.empty() && !n.names[0].empty()) {
      key[n.id] = keyOf(m.namingScope(n.scope)) + "/%" + n.names[0];
    } else if (!n.names.empty()) {
      key[n.id] = "#" + std::to_string(n.id);  // `%<id>`: the name is the id
    } else {
      const int k = ordinal[{n.scope, n.kind}]++;
      key[n.id] = parent + "/" + n.kind + "@" + std::to_string(k);
    }
  }
  return key;
}

std::string remapAtoms(const std::string& text, const std::map<NodeId, NodeId>& to) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    if (text[i] != '%') {
      out += text[i++];
      continue;
    }
    size_t j = i + 1;
    while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) ||
                               text[j] == '#' || text[j] == '_' || text[j] == '.')) {
      ++j;
    }
    ValueRef v;
    const std::string atom = text.substr(i, j - i);
    if (parseValueAtom(atom, &v) && to.count(v.id)) {
      v.id = to.at(v.id);
      out += valueAtom(v);
    } else {
      out += atom;
    }
    i = j;
  }
  return out;
}

}  // namespace

Module reconcileIds(const Module& base, const Module& edited, std::string* report) {
  const auto bk = definitionKeys(base);
  const auto ek = definitionKeys(edited);
  std::map<std::string, NodeId> by_key;
  for (const auto& [id, k] : bk) by_key[k] = id;

  NodeId next = std::max(base.nextId(), edited.nextId());
  std::map<NodeId, NodeId> to;  // edited id -> final id
  std::set<NodeId> used;
  size_t matched = 0, added = 0;
  for (const Node& n : edited.nodes) {
    auto it = by_key.find(ek.at(n.id));
    if (it != by_key.end() && !used.count(it->second)) {
      to[n.id] = it->second;
      used.insert(it->second);
      ++matched;
    }
  }
  for (const Node& n : edited.nodes) {
    if (to.count(n.id)) continue;
    to[n.id] = next++;  // new definitions never reuse an id the base has seen
    ++added;
  }
  const size_t removed = base.nodes.size() - matched;

  auto mapId = [&](NodeId id) { return id == kInvalidId ? id : to.at(id); };
  auto ren = [&](const std::string& a) {
    ValueRef v;
    if (!parseValueAtom(a, &v) || !to.count(v.id)) return a;
    v.id = to.at(v.id);
    return valueAtom(v);
  };
  Module out;
  out.reserveId(next - 1);
  for (const Node& n0 : edited.nodes) {
    Node n = n0;
    n.id = mapId(n.id);
    n.scope = mapId(n.scope);
    for (Operand& o : n.operands) {
      if (o.kind == Operand::Kind::Value) o.value.id = mapId(o.value.id);
      for (auto& e : o.exprs) e = symbolic::mapVars(e, ren);
    }
    for (Successor& s : n.successors) {
      s.block = mapId(s.block);
      for (ValueRef& a : s.args) a.id = mapId(a.id);
    }
    for (Type& t : n.types) {
      if (!t || !t->shaped) continue;
      auto t2 = std::make_shared<TypeTerm>(*t);
      for (auto& d : t2->dims) d = symbolic::mapVars(d, ren);
      t = t2;
    }
    out.append(std::move(n));
  }
  for (const Module::FactEntry& f : edited.facts) {
    out.facts.push_back({mapId(f.scope), remapAtoms(f.text, to)});
  }
  if (report) {
    *report = "matched " + std::to_string(matched) + ", added " + std::to_string(added) +
              ", removed " + std::to_string(removed);
  }
  return out;
}

}  // namespace brrr
