#include "symbolic/expr_io.h"

#include <cctype>
#include <cstdlib>

namespace brrr::symbolic {
namespace {

bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

struct Tok {
  enum class Kind { End, Int, Atom, Plus, Minus, Star, Slash, LParen, RParen, Comma, Bad };
  Kind kind = Kind::End;
  int64_t i = 0;
  std::string s;
  size_t pos = 0;
};

class Lexer {
 public:
  explicit Lexer(const std::string& text) : text_(text) {}

  Tok next() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
    Tok t;
    t.pos = pos_;
    if (pos_ >= text_.size()) return t;
    const char c = text_[pos_];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const size_t start = pos_;
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
      t.kind = Tok::Kind::Int;
      t.i = std::strtoll(text_.substr(start, pos_ - start).c_str(), nullptr, 10);
      return t;
    }
    // Atoms: symbols (`tile`), values (`%acc`, `%12`, `%12#1`), fresh dims (`?`).
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '%' ||
        c == '?') {
      const size_t start = pos_++;
      if (c != '?') {
        while (pos_ < text_.size() && isIdentChar(text_[pos_])) ++pos_;
        if (c == '%' && pos_ < text_.size() && text_[pos_] == '#') {
          ++pos_;
          while (pos_ < text_.size() &&
                 std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
          }
        }
        if (c == '%' && pos_ == start + 1) {
          t.kind = Tok::Kind::Bad;
          return t;
        }
      }
      t.kind = Tok::Kind::Atom;
      t.s = text_.substr(start, pos_ - start);
      return t;
    }
    ++pos_;
    switch (c) {
      case '+': t.kind = Tok::Kind::Plus; return t;
      case '-': t.kind = Tok::Kind::Minus; return t;
      case '*': t.kind = Tok::Kind::Star; return t;
      case '/': t.kind = Tok::Kind::Slash; return t;
      case '(': t.kind = Tok::Kind::LParen; return t;
      case ')': t.kind = Tok::Kind::RParen; return t;
      case ',': t.kind = Tok::Kind::Comma; return t;
      default: t.kind = Tok::Kind::Bad; return t;
    }
  }

 private:
  const std::string& text_;
  size_t pos_ = 0;
};

}  // namespace

bool splitDimAtom(const std::string& atom, std::string* value, size_t* k) {
  if (atom.size() < 4 || atom[0] != '?' || atom[1] != '%') return false;
  const size_t dot = atom.rfind('.');
  if (dot == std::string::npos || dot + 1 >= atom.size()) return false;
  for (size_t i = dot + 1; i < atom.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(atom[i]))) return false;
  }
  *value = atom.substr(1, dot - 1);
  *k = std::strtoul(atom.c_str() + dot + 1, nullptr, 10);
  return true;
}

namespace {

Expr negate(Expr e) {
  if (e->kind == ExprTerm::Kind::Const) return ExprTerm::constant(-e->c);
  return ExprTerm::mul({ExprTerm::constant(-1), std::move(e)});
}

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

  // expr := term (('+'|'-') term)*   —  `a - b` is stored as a + (-1 * b).
  Expr expr(std::string* err) {
    std::vector<Expr> terms;
    Expr first = term(err);
    if (!first) return nullptr;
    terms.push_back(first);
    while (tok_.kind == Tok::Kind::Plus || tok_.kind == Tok::Kind::Minus) {
      const bool minus = tok_.kind == Tok::Kind::Minus;
      advance();
      Expr t = term(err);
      if (!t) return nullptr;
      terms.push_back(minus ? negate(t) : t);
    }
    return terms.size() == 1 ? terms[0] : ExprTerm::add(std::move(terms));
  }

  static bool divisionWord(const Tok& t, ExprTerm::Kind* kind) {
    if (t.kind != Tok::Kind::Atom) return false;
    if (t.s == "floordiv") *kind = ExprTerm::Kind::FloorDiv;
    else if (t.s == "ceildiv") *kind = ExprTerm::Kind::CeilDiv;
    else if (t.s == "mod") *kind = ExprTerm::Kind::Mod;
    else return false;
    return true;
  }

  // term := unary (('*' | 'floordiv' | 'ceildiv' | 'mod') unary)*, left to right.
  Expr term(std::string* err) {
    std::vector<Expr> factors;
    Expr first = unary(err);
    if (!first) return nullptr;
    factors.push_back(first);
    ExprTerm::Kind dk;
    while (true) {
      if (tok_.kind == Tok::Kind::Star) {
        advance();
        Expr f = unary(err);
        if (!f) return nullptr;
        factors.push_back(f);
      } else if (divisionWord(tok_, &dk)) {
        const std::string word = tok_.s;
        advance();
        Expr d = unary(err);
        if (!d) return nullptr;
        if (d->kind != ExprTerm::Kind::Const || d->c <= 0) {
          *err = word + " needs a positive integer constant on its right";
          return nullptr;
        }
        Expr lhs = factors.size() == 1 ? factors[0] : ExprTerm::mul(std::move(factors));
        factors = {ExprTerm::div(dk, lhs, d->c)};
      } else if (tok_.kind == Tok::Kind::Slash) {
        *err = "integer division must say how it rounds: use floordiv or ceildiv";
        return nullptr;
      } else {
        break;
      }
    }
    return factors.size() == 1 ? factors[0] : ExprTerm::mul(std::move(factors));
  }

  Expr unary(std::string* err) {
    if (tok_.kind == Tok::Kind::Minus) {
      advance();
      Expr e = unary(err);
      return e ? negate(e) : nullptr;
    }
    return factor(err);
  }

  Expr factor(std::string* err) {
    switch (tok_.kind) {
      case Tok::Kind::Int: {
        const int64_t v = tok_.i;
        advance();
        return ExprTerm::constant(v);
      }
      case Tok::Kind::Atom: {
        std::string name = tok_.s;
        advance();
        if (name == "dim" && tok_.kind == Tok::Kind::LParen) {  // dim(%x, k)
          advance();
          if (tok_.kind != Tok::Kind::Atom || tok_.s[0] != '%') {
            *err = "dim(...) takes a %value first";
            return nullptr;
          }
          const std::string value = tok_.s;
          advance();
          if (!expectComma(err)) return nullptr;
          if (tok_.kind != Tok::Kind::Int) {
            *err = "dim(%x, k) takes an integer dimension";
            return nullptr;
          }
          const std::string k = std::to_string(tok_.i);
          advance();
          if (tok_.kind != Tok::Kind::RParen) {
            *err = "expected ')' to close dim(...)";
            return nullptr;
          }
          advance();
          return ExprTerm::var("?" + value + "." + k);
        }
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
                   : "expected a number, %value, symbol, or '(' at offset " +
                         std::to_string(tok_.pos);
        return nullptr;
    }
  }

  bool expectComma(std::string* err) {
    if (tok_.kind != Tok::Kind::Comma) {
      *err = "expected ',' in dim(%x, k)";
      return false;
    }
    advance();
    return true;
  }

  Lexer lex_;
  Tok tok_;
};

// Precedence for printing: 0 = sum, 1 = product or division, 2 = atom.
int precedence(const Expr& e) {
  switch (e->kind) {
    case ExprTerm::Kind::Add: return 0;
    case ExprTerm::Kind::Mul:
    case ExprTerm::Kind::FloorDiv:
    case ExprTerm::Kind::CeilDiv:
    case ExprTerm::Kind::Mod: return 1;
    case ExprTerm::Kind::Const: return e->c < 0 ? 0 : 2;
    case ExprTerm::Kind::Var: return 2;
  }
  return 2;
}

// `-1 * x` (or a negative constant) inside a sum prints as subtraction.
bool asNegated(const Expr& e, Expr* positive) {
  if (e->kind == ExprTerm::Kind::Const && e->c < 0) {
    *positive = ExprTerm::constant(-e->c);
    return true;
  }
  if (e->kind == ExprTerm::Kind::Mul && !e->args.empty() &&
      e->args[0]->kind == ExprTerm::Kind::Const && e->args[0]->c < 0) {
    std::vector<Expr> rest(e->args.begin() + 1, e->args.end());
    if (e->args[0]->c != -1) {
      rest.insert(rest.begin(), ExprTerm::constant(-e->args[0]->c));
    }
    *positive = rest.size() == 1 ? rest[0] : ExprTerm::mul(std::move(rest));
    return true;
  }
  return false;
}

std::string printRec(const Expr& e,
                     const std::function<std::string(const std::string&)>& rn) {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return std::to_string(e->c);
    case ExprTerm::Kind::Var: {
      std::string inner;
      size_t k = 0;
      if (splitDimAtom(e->name, &inner, &k)) {
        return "dim(" + (rn ? rn(inner) : inner) + ", " + std::to_string(k) + ")";
      }
      return rn ? rn(e->name) : e->name;
    }
    case ExprTerm::Kind::Add: {
      std::string out;
      for (size_t i = 0; i < e->args.size(); ++i) {
        Expr pos;
        if (i > 0 && asNegated(e->args[i], &pos)) {
          const std::string s = printRec(pos, rn);
          out += " - " + (precedence(pos) == 0 ? "(" + s + ")" : s);
          continue;
        }
        if (i > 0) out += " + ";
        out += printRec(e->args[i], rn);
      }
      return out;
    }
    case ExprTerm::Kind::FloorDiv:
    case ExprTerm::Kind::CeilDiv:
    case ExprTerm::Kind::Mod: {
      // The left side is parenthesized unless it is an atom: `(%b * 2) floordiv 3`.
      const std::string l = printRec(e->args[0], rn);
      const char* op = e->kind == ExprTerm::Kind::FloorDiv  ? " floordiv "
                       : e->kind == ExprTerm::Kind::CeilDiv ? " ceildiv "
                                                            : " mod ";
      return (precedence(e->args[0]) < 2 ? "(" + l + ")" : l) + op +
             std::to_string(e->args[1]->c);
    }
    case ExprTerm::Kind::Mul: {
      std::string out;
      for (size_t i = 0; i < e->args.size(); ++i) {
        if (i) out += " * ";
        const std::string s = printRec(e->args[i], rn);
        // A division inside a product is parenthesized, as MLIR prints it:
        // `(x floordiv 3) * y` (needed after `*`, clearer before it).
        const bool paren = precedence(e->args[i]) < 1 || e->args[i]->isDivision();
        out += paren ? "(" + s + ")" : s;
      }
      // `-1 * x` alone prints as `-x`.
      Expr pos;
      if (asNegated(e, &pos)) {
        const std::string s = printRec(pos, rn);
        return "-" + (precedence(pos) < 2 ? "(" + s + ")" : s);
      }
      return out;
    }
  }
  return "?";
}

}  // namespace

Expr parseExpr(const std::string& text, std::string* err) {
  Parser p(text);
  return p.parse(err);
}

std::string printExpr(const Expr& e,
                      const std::function<std::string(const std::string&)>& rename) {
  return printRec(e, rename);
}

Expr mapVars(const Expr& e,
             const std::function<std::string(const std::string&)>& f) {
  switch (e->kind) {
    case ExprTerm::Kind::Const:
      return e;
    case ExprTerm::Kind::Var: {
      std::string inner;
      size_t k = 0;
      if (splitDimAtom(e->name, &inner, &k)) {
        return ExprTerm::var("?" + f(inner) + "." + std::to_string(k));
      }
      return ExprTerm::var(f(e->name));
    }
    default: {
      std::vector<Expr> args;
      args.reserve(e->args.size());
      for (const Expr& a : e->args) args.push_back(mapVars(a, f));
      return ExprTerm::rebuild(e, std::move(args));
    }
  }
}

void collectVars(const Expr& e, std::set<std::string>* out) {
  if (e->kind == ExprTerm::Kind::Var) {
    out->insert(e->name);
    return;
  }
  for (const Expr& a : e->args) collectVars(a, out);
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
    if (c == '(' || c == '[') ++depth;
    if (c == ')' || c == ']') --depth;
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

}  // namespace brrr::symbolic
