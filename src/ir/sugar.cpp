#include "ir/sugar.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace bridgeir {
namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> splitChar(const std::string& s, char d) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == d) {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(trim(cur));
  return out;
}

bool isId(const std::string& t, uint64_t* id) {
  if (t.size() < 2 || t[0] != 'n') return false;
  for (size_t i = 1; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
  }
  if (id) *id = std::strtoull(t.c_str() + 1, nullptr, 10);
  return true;
}

bool isName(const std::string& s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) ||
                     s[0] == '_')) {
    return false;
  }
  for (const char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

struct Ctx {
  uint64_t next_id = 1;
  std::string err;
  bool failed = false;

  std::string fresh() { return "n" + std::to_string(next_id++); }
  void fail(const std::string& msg) {
    if (!failed) {
      failed = true;
      err = msg;
    }
  }
};

size_t matchBrace(const std::vector<std::string>& in, size_t open_line, Ctx* ctx) {
  int depth = 0;
  for (size_t i = open_line; i < in.size(); ++i) {
    for (const std::string& t : splitWs(in[i])) {
      if (t == "{") {
        ++depth;
      } else if (t == "}") {
        // A leading `}` (as in `} else {`) closes the *previous* branch and does
        // not participate in this match.
        if (depth == 0) continue;
        --depth;
        if (depth == 0) return i;
      }
    }
  }
  ctx->fail("unterminated block starting at: " + trim(in[open_line]));
  return in.size();
}

bool headerInner(const std::string& hdr, std::string* inner, Ctx* ctx,
                 const std::string& line) {
  const size_t lp = hdr.find('(');
  const size_t rp = hdr.rfind(')');
  if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
    ctx->fail("expected ( ... ) in: " + trim(line));
    return false;
  }
  *inner = trim(hdr.substr(lp + 1, rp - lp - 1));
  // A `for` header legitimately contains ';'-separated parts; otherwise the
  // condition must be a single value (name or id, not an expression).
  if (inner->find(';') == std::string::npos && splitWs(*inner).size() != 1) {
    ctx->fail("condition must be a single value (name or id), not an "
              "expression, in: " + trim(line));
    return false;
  }
  return true;
}

std::vector<std::string> lowerRange(const std::vector<std::string>& in,
                                    size_t begin, size_t end, Ctx* ctx);

// Rewrite a branch's single `yield` line into an `edge` to the join block.
bool rewriteYield(std::vector<std::string>* branch, const std::string& join_id,
                  std::string* edge_id, size_t* nvals, Ctx* ctx) {
  int idx = -1;
  for (size_t k = 0; k < branch->size(); ++k) {
    const std::vector<std::string> bt = splitWs((*branch)[k]);
    bool is_yield = false;
    if (bt.size() >= 4 && isId(bt[0], nullptr)) {
      if (bt[1] == "=" && bt[2] == "yield") {
        is_yield = true;  // anonymous: nID = yield ...
      } else if (bt.size() >= 5 && bt[2] == "=" && bt[3] == "yield") {
        is_yield = true;  // named: nID name = yield ...
      }
    }
    if (!is_yield) continue;
    if (idx >= 0) {
      ctx->fail("if branch has more than one yield");
      return false;
    }
    idx = static_cast<int>(k);
  }
  if (idx < 0) {
    ctx->fail("if branch must contain a yield (value-if only for now)");
    return false;
  }
  const std::vector<std::string> bt = splitWs((*branch)[idx]);
  size_t ypos = 0;
  for (size_t k = 0; k < bt.size(); ++k) {
    if (bt[k] == "yield") ypos = k;
  }
  *edge_id = bt[0];
  std::string line = bt[0];
  if (bt[1] != "=") line += " " + bt[1];
  line += " = edge " + join_id;
  for (size_t k = ypos + 1; k < bt.size(); ++k) line += " " + bt[k];
  (*branch)[idx] = line;
  *nvals = bt.size() - (ypos + 1);
  return true;
}

std::vector<std::string> lowerRange(const std::vector<std::string>& in,
                                    size_t begin, size_t end, Ctx* ctx) {
  std::vector<std::string> out;
  size_t i = begin;
  while (i < end && !ctx->failed) {
    const std::string& line = in[i];
    const std::vector<std::string> t = splitWs(line);
    size_t eq = 0;
    while (eq < t.size() && t[eq] != "=") ++eq;
    const bool is_sugar =
        t.size() >= 5 && isId(t[0], nullptr) && eq >= 1 && eq + 1 < t.size() &&
        (t[eq + 1] == "for" || t[eq + 1] == "while" || t[eq + 1] == "if") &&
        !t.empty() && t.back() == "{";
    if (!is_sugar) {
      out.push_back(line);
      ++i;
      continue;
    }

    const std::string kind = t[eq + 1];
    const std::string name = eq == 2 ? t[1] : std::string();
    std::string hdr;
    for (size_t k = eq + 2; k + 1 < t.size(); ++k) {
      if (!hdr.empty()) hdr += " ";
      hdr += t[k];
    }
    std::string inner;
    if (!headerInner(hdr, &inner, ctx, line)) return out;

    const size_t close = matchBrace(in, i, ctx);
    if (ctx->failed) return out;

    if (kind == "for" || kind == "while") {
      std::vector<std::string> body_raw(in.begin() + i + 1, in.begin() + close);
      std::vector<std::string> body =
          lowerRange(body_raw, 0, body_raw.size(), ctx);
      if (ctx->failed) return out;

      std::vector<std::string> carried;
      std::vector<std::string> init_ref;
      std::vector<std::string> next_val;
      std::string cond = inner;

      if (kind == "for") {
        const std::vector<std::string> parts = splitChar(inner, ';');
        if (parts.size() != 3) {
          ctx->fail("for header must be (init; cond; step) in: " + trim(line));
          return out;
        }
        cond = trim(parts[1]);
        for (const std::string& e : splitChar(parts[0], ',')) {
          const size_t p = e.find('=');
          if (p == std::string::npos) {
            ctx->fail("for init must be 'name = value' in: " + trim(line));
            return out;
          }
          const std::string nm = trim(e.substr(0, p));
          if (!isName(nm)) {
            ctx->fail("bad carried name '" + nm + "' in: " + trim(line));
            return out;
          }
          carried.push_back(nm);
          init_ref.push_back(trim(e.substr(p + 1)));
        }
        std::vector<std::string> step_names;
        std::vector<std::string> step_vals;
        for (const std::string& e : splitChar(parts[2], ',')) {
          const size_t p = e.find('=');
          if (p == std::string::npos) {
            ctx->fail("for step must be 'name = value' in: " + trim(line));
            return out;
          }
          step_names.push_back(trim(e.substr(0, p)));
          step_vals.push_back(trim(e.substr(p + 1)));
        }
        for (const std::string& c : carried) {
          std::string v;
          for (size_t k = 0; k < step_names.size(); ++k) {
            if (step_names[k] == c) v = step_vals[k];
          }
          next_val.push_back(v);
        }
      } else {
        // while: carried names come from depth-0 assignments in the body.
        int depth = 0;
        for (const std::string& b : body) {
          const std::vector<std::string> bt = splitWs(b);
          for (const std::string& tk : bt) {
            if (tk == "{") ++depth;
            else if (tk == "}") --depth;
          }
          if (depth != 0 || bt.size() != 3 || bt[1] != "=" || !isName(bt[0])) {
            continue;
          }
          size_t slot = carried.size();
          for (size_t k = 0; k < carried.size(); ++k) {
            if (carried[k] == bt[0]) slot = k;
          }
          if (slot == carried.size()) {
            carried.push_back(bt[0]);
            init_ref.push_back(bt[0]);  // initial value = enclosing binding
            next_val.push_back(bt[2]);
          } else {
            next_val[slot] = bt[2];
          }
        }
      }

      // Drop depth-0 assignment lines; they are the loop-carried updates.
      std::vector<std::string> kept;
      {
        int depth = 0;
        for (const std::string& b : body) {
          const std::vector<std::string> bt = splitWs(b);
          int d = depth;
          for (const std::string& tk : bt) {
            if (tk == "{") ++d;
            else if (tk == "}") --d;
          }
          const bool assign = depth == 0 && bt.size() == 3 && bt[1] == "=" &&
                              isName(bt[0]);
          if (assign) {
            size_t slot = carried.size();
            for (size_t k = 0; k < carried.size(); ++k) {
              if (carried[k] == bt[0]) slot = k;
            }
            if (slot == carried.size()) {
              ctx->fail("assignment to '" + bt[0] +
                        "', which the loop does not carry");
              return out;
            }
            next_val[slot] = bt[2];
          } else {
            kept.push_back(b);
          }
          depth = d;
        }
      }

      for (size_t k = 0; k < carried.size(); ++k) {
        if (next_val[k].empty()) {
          ctx->fail("carried value '" + carried[k] + "' is never updated");
          return out;
        }
      }

      std::string head = t[0] + (name.empty() ? "" : " " + name) + " = loop";
      for (const std::string& v : init_ref) head += " " + v;
      head += " {";
      out.push_back(head);
      for (size_t k = 0; k < carried.size(); ++k) {
        out.push_back(ctx->fresh() + " " + carried[k] +
                      " = param idx=" + std::to_string(k));
      }
      for (const std::string& b : kept) out.push_back(b);
      std::string y = ctx->fresh() + " = loop.yield " + cond;
      for (const std::string& v : next_val) y += " " + v;
      out.push_back(y);
      out.push_back("}");
      i = close + 1;
      continue;
    }

    // if / else: cbr + branch blocks with edges + a join block (the if value).
    const std::vector<std::string> close_tok = splitWs(in[close]);
    if (close_tok.size() < 3 || close_tok[1] != "else" ||
        close_tok.back() != "{") {
      ctx->fail("if requires '} else {' (value-if with both branches) in: " +
                trim(line));
      return out;
    }
    const size_t else_close = matchBrace(in, close, ctx);
    if (ctx->failed) return out;

    std::vector<std::string> then_raw(in.begin() + i + 1, in.begin() + close);
    std::vector<std::string> else_raw(in.begin() + close + 1,
                                      in.begin() + else_close);
    std::vector<std::string> then_branch =
        lowerRange(then_raw, 0, then_raw.size(), ctx);
    if (ctx->failed) return out;
    std::vector<std::string> else_branch =
        lowerRange(else_raw, 0, else_raw.size(), ctx);
    if (ctx->failed) return out;

    const std::string join_id = t[0];
    std::string then_edge;
    std::string else_edge;
    size_t n_then = 0;
    size_t n_else = 0;
    if (!rewriteYield(&then_branch, join_id, &then_edge, &n_then, ctx)) return out;
    if (!rewriteYield(&else_branch, join_id, &else_edge, &n_else, ctx)) return out;
    if (n_then != n_else) {
      ctx->fail("if branches must yield the same number of values (" +
                std::to_string(n_then) + " vs " + std::to_string(n_else) +
                ") in: " + trim(line));
      return out;
    }

    const std::string cbr_id = ctx->fresh();
    const std::string then_blk = ctx->fresh();
    const std::string else_blk = ctx->fresh();
    out.push_back(cbr_id + " = cbr " + inner + " " + then_edge + " " +
                  else_edge);
    out.push_back(then_blk + " = block {");
    for (const std::string& b : then_branch) out.push_back(b);
    out.push_back("}");
    out.push_back(else_blk + " = block {");
    for (const std::string& b : else_branch) out.push_back(b);
    out.push_back("}");
    out.push_back(t[0] + (name.empty() ? "" : " " + name) + " = block {");
    for (size_t k = 0; k < n_then; ++k) {
      out.push_back(ctx->fresh() + " = param idx=" + std::to_string(k));
    }
    out.push_back("}");
    i = else_close + 1;
  }
  return out;
}

}  // namespace

bool lowerCppSugar(std::vector<std::string>* lines, std::string* err) {
  Ctx ctx;
  for (const std::string& l : *lines) {
    for (const std::string& t : splitWs(l)) {
      std::string base = t;
      const size_t dot = base.find('.');
      if (dot != std::string::npos) base = base.substr(0, dot);
      uint64_t id = 0;
      if (isId(base, &id) && id + 1 > ctx.next_id) ctx.next_id = id + 1;
    }
  }
  std::vector<std::string> out = lowerRange(*lines, 0, lines->size(), &ctx);
  if (ctx.failed) {
    *err = ctx.err;
    return false;
  }
  *lines = std::move(out);
  return true;
}

}  // namespace bridgeir
