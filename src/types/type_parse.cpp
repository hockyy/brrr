#include "types/type_parse.h"

#include <cctype>

namespace brrr::types {
namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : t_(text) {}

  bool parse(TypeRef* out, std::string* err) {
    if (!term(out, err)) return false;
    skip();
    if (i_ != t_.size()) {
      *err = "unexpected trailing input at offset " + std::to_string(i_) +
             " in type '" + t_ + "'";
      return false;
    }
    return true;
  }

 private:
  void skip() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t')) ++i_;
  }

  bool ident(std::string* out) {
    skip();
    const size_t b = i_;
    if (i_ >= t_.size() ||
        !(std::isalpha(static_cast<unsigned char>(t_[i_])) || t_[i_] == '_')) {
      return false;
    }
    while (i_ < t_.size() &&
           (std::isalnum(static_cast<unsigned char>(t_[i_])) || t_[i_] == '_')) {
      ++i_;
    }
    *out = t_.substr(b, i_ - b);
    return true;
  }

  bool digits(std::string* out) {
    skip();
    const size_t b = i_;
    while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
      ++i_;
    }
    if (i_ == b) return false;
    *out = t_.substr(b, i_ - b);
    return true;
  }

  bool term(TypeRef* out, std::string* err) {
    std::string name;
    if (!ident(&name)) {
      *err = "expected a type name at offset " + std::to_string(i_) +
             " in type '" + t_ + "'";
      return false;
    }
    skip();
    if (i_ < t_.size() && t_[i_] == '<') {
      ++i_;
      std::vector<TypeRef> args;
      while (true) {
        TypeRef arg;
        std::string num;
        if (digits(&num)) {
          arg = TypeTerm::makeName(num);
        } else if (!term(&arg, err)) {
          return false;
        }
        args.push_back(arg);
        skip();
        if (i_ < t_.size() && t_[i_] == ',') {
          ++i_;
          continue;
        }
        break;
      }
      skip();
      if (i_ >= t_.size() || t_[i_] != '>') {
        *err = "expected '>' at offset " + std::to_string(i_) + " in type '" +
               t_ + "'";
        return false;
      }
      ++i_;
      *out = TypeTerm::makeName(name, std::move(args));
      return true;
    }
    *out = TypeTerm::makeName(name);
    return true;
  }

  const std::string& t_;
  size_t i_ = 0;
};

}  // namespace

bool parseTypeTerm(const std::string& text, TypeRef* out, std::string* err) {
  Parser p(text);
  return p.parse(out, err);
}

}  // namespace brrr::types
