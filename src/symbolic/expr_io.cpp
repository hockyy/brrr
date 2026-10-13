#include "symbolic/expr_io.h"

#include <cctype>
#include <cstdlib>

namespace bridgeir::symbolic {
namespace {

struct Tok {
  enum class Kind { End, Int, Ident, Plus, Star, LParen, RParen, Bad };
  Kind kind = Kind::End;
  int64_t i = 0;
  std::string s;
  size_t pos = 0;
};

class Lexer {
 public:
  explicit Lexer(const std::string& text) : text_(text) {}

  Tok next() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
    if (pos_ >= text_.size()) return Tok{Tok::Kind::End, 0, "", pos_};
    const char c = text_[pos_];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const size_t start = pos_;
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
      Tok t;
      t.kind = Tok::Kind::Int;
      t.i = std::strtoll(text_.substr(start, pos_ - start).c_str(), nullptr, 10);
      t.pos = start;
      return t;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t start = pos_;
      while (pos_ < text_.size() &&
             (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
              text_[pos_] == '_')) {
        ++pos_;
      }
      Tok t;
      t.kind = Tok::Kind::Ident;
      t.s = text_.substr(start, pos_ - start);
      t.pos = start;
      return t;
    }
    ++pos_;
    Tok t;
    t.pos = pos_ - 1;
    switch (c) {
      case '+': t.kind = Tok::Kind::Plus; return t;
      case '*': t.kind = Tok::Kind::Star; return t;
      case '(': t.kind = Tok::Kind::LParen; return t;
      case ')': t.kind = Tok::Kind::RParen; return t;
      default: return t;  // Bad
    }
  }

 private:
  const std::string& text_;
  size_t pos_ = 0;
};

class Parser {
 public:
  explicit Parser(const std::string& text) : lex_(text) { tok_ = lex_.next(); }

  Expr parse(std::string* err) {
    Expr e = expr(err);
    if (!e) return nullptr;
    if (tok_.kind != Tok::Kind::End) {
      *err = "unexpected trailing input at offset " + std::to_string(tok_.pos);
      return nullptr;
    }
    return e;
  }

 private:
  void advance() { tok_ = lex_.next(); }

  Expr expr(std::string* err) {
    std::vector<Expr> terms;
    Expr first = term(err);
    if (!first) return nullptr;
    terms.push_back(first);
    while (tok_.kind == Tok::Kind::Plus) {
      advance();
      Expr t = term(err);
      if (!t) return nullptr;
      terms.push_back(t);
    }
    return terms.size() == 1 ? terms[0] : ExprTerm::add(std::move(terms));
  }

  Expr term(std::string* err) {
    std::vector<Expr> factors;
    Expr first = factor(err);
    if (!first) return nullptr;
    factors.push_back(first);
    while (tok_.kind == Tok::Kind::Star) {
      advance();
      Expr f = factor(err);
      if (!f) return nullptr;
      factors.push_back(f);
    }
    return factors.size() == 1 ? factors[0] : ExprTerm::mul(std::move(factors));
  }

  Expr factor(std::string* err) {
    switch (tok_.kind) {
      case Tok::Kind::Int: {
        const int64_t v = tok_.i;
        advance();
        return ExprTerm::constant(v);
      }
      case Tok::Kind::Ident: {
        std::string name = tok_.s;
        advance();
        return ExprTerm::var(std::move(name));
      }
      case Tok::Kind::LParen: {
        advance();
        Expr e = expr(err);
        if (!e) return nullptr;
        if (tok_.kind != Tok::Kind::RParen) {
          *err = "expected ')' at offset " + std::to_string(tok_.pos);
          return nullptr;
        }
        advance();
        return e;
      }
      default:
        *err = tok_.kind == Tok::Kind::End
                   ? "unexpected end of expression"
                   : "expected a number, symbol, or '(' at offset " +
                         std::to_string(tok_.pos);
        return nullptr;
    }
  }

  Lexer lex_;
  Tok tok_;
};

}  // namespace

Expr parseExpr(const std::string& text, std::string* err) {
  Parser p(text);
  return p.parse(err);
}

namespace {
std::string trimForSplit(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}
}  // namespace

std::vector<std::string> splitTopLevel(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (const char c : s) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == delim && depth == 0) {
      out.push_back(trimForSplit(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(trimForSplit(cur));
  return out;
}

}  // namespace bridgeir::symbolic
