// CSS custom property pre-pass. See cssvars.h.
#include "cssvars.h"
#include "csslex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

namespace quill::engine {
namespace {

using namespace lex;
namespace {

// ---- compound selector matching against one element
bool attr_matches(const std::string& content, const ElementView& el) {
    size_t k = 0;
    while (k < content.size() && content[k] != '=' && !std::strchr("~|^$*", content[k])) ++k;
    std::string name = lower(trim(content.substr(0, k)));
    std::string have;
    bool        present = el.attr && el.attr(name, have);
    if (k >= content.size()) return present;
    char op = content[k];
    if (op != '=') ++k; // skip the '='
    ++k;
    std::string want = trim(content.substr(k));
    bool        icase = false;
    if (want.size() > 2 && (want.back() == 'i' || want.back() == 'I') && is_ws(want[want.size() - 2])) {
        icase = true;
        want  = trim(want.substr(0, want.size() - 1));
    }
    if (want.size() >= 2 && (want[0] == '"' || want[0] == '\'') && want.back() == want[0]) want = want.substr(1, want.size() - 2);
    if (!present) return false;
    if (icase) { have = lower(have); want = lower(want); }
    switch (op) {
    case '=': return have == want;
    case '^': return !want.empty() && have.compare(0, want.size(), want) == 0;
    case '$': return !want.empty() && have.size() >= want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0;
    case '*': return !want.empty() && have.find(want) != std::string::npos;
    case '|': return have == want || have.compare(0, want.size() + 1, want + "-") == 0;
    case '~': {
        size_t i = 0;
        while (i < have.size()) {
            while (i < have.size() && is_ws(have[i])) ++i;
            size_t j = i;
            while (j < have.size() && !is_ws(have[j])) ++j;
            if (j > i && have.substr(i, j - i) == want) return true;
            i = j;
        }
        return false;
    }
    }
    return false;
}

bool match_compound(const std::string& s, const ElementView& el, bool is_root, int& spec) {
    size_t i = 0, n = s.size();
    if (i < n && s[i] == '*') ++i;
    else if (i < n && is_ident(s[i])) {
        size_t j = i;
        while (j < n && is_ident(s[j])) ++j;
        if (lower(s.substr(i, j - i)) != el.tag) return false;
        spec += 1;
        i = j;
    }
    std::string v;
    while (i < n) {
        char c = s[i];
        if (c == '#' || c == '.') {
            size_t j = ++i;
            while (j < n && is_ident(s[j])) ++j;
            std::string w = s.substr(i, j - i);
            i             = j;
            if (c == '#') {
                if (!(el.attr && el.attr("id", v) && v == w)) return false;
                spec += 10000;
            } else {
                bool has = false;
                if (el.attr && el.attr("class", v)) {
                    size_t p = 0;
                    while (p < v.size()) {
                        while (p < v.size() && is_ws(v[p])) ++p;
                        size_t q = p;
                        while (q < v.size() && !is_ws(v[q])) ++q;
                        if (q > p && v.substr(p, q - p) == w) has = true;
                        p = q;
                    }
                }
                if (!has) return false;
                spec += 100;
            }
        } else if (c == '[') {
            size_t end = scan_to(s, i + 1, n, "]", false);
            if (end >= n) return false;
            if (!attr_matches(s.substr(i + 1, end - i - 1), el)) return false;
            spec += 100;
            i = end + 1;
        } else if (c == ':') {
            if (i + 1 < n && s[i + 1] == ':') return false;
            size_t j = ++i;
            while (j < n && is_ident(s[j])) ++j;
            std::string name = lower(s.substr(i, j - i)), arg;
            i                = j;
            if (i < n && s[i] == '(') {
                size_t close = scan_to(s, i + 1, n, ")", false);
                if (close >= n) return false;
                arg = trim(s.substr(i + 1, close - i - 1));
                i   = close + 1;
            }
            if (name == "root") {
                if (!is_root) return false;
                spec += 100;
            } else if (name == "not") {
                int sp = 0;
                if (match_compound(arg, el, is_root, sp)) return false;
                spec += sp;
            } else {
                return false;
            }
        } else {
            return false; // whitespace / combinator / anything we don't model
        }
    }
    return true;
}


} // namespace
} // namespace

// ------------------------------------------------------------------ media queries

bool CssVars::eval_feature(const std::string& inner_in) const {
    std::string inner = trim(inner_in);
    if (inner.empty() || inner[0] == '(') return false;
    auto num_of = [&](const std::string& tok, double& v) {
        std::string t = lower(trim(tok));
        if (t == "width" || t == "device-width") { v = env_.width; return true; }
        if (t == "height" || t == "device-height") { v = env_.height; return true; }
        return parse_len(t, v);
    };
    bool has_cmp = inner.find_first_of("<>") != std::string::npos;
    if (has_cmp) { // range syntax: (400px <= width <= 700px), (width >= 640px)
        std::vector<std::string> operands, ops;
        std::string cur;
        for (size_t i = 0; i < inner.size(); ++i) {
            char c = inner[i];
            if (c == '<' || c == '>' || c == '=') {
                std::string op(1, c);
                if (i + 1 < inner.size() && inner[i + 1] == '=' && c != '=') { op += '='; ++i; }
                operands.push_back(cur);
                cur.clear();
                ops.push_back(op);
            } else {
                cur += c;
            }
        }
        operands.push_back(cur);
        for (size_t k = 0; k < ops.size(); ++k) {
            double a, b;
            if (!num_of(operands[k], a) || !num_of(operands[k + 1], b)) return false;
            const std::string& op = ops[k];
            bool               ok = op == "<" ? a < b : op == "<=" ? a <= b : op == ">" ? a > b : op == ">=" ? a >= b : a == b;
            if (!ok) return false;
        }
        return true;
    }
    size_t colon = inner.find(':');
    if (colon == std::string::npos) {
        std::string f = lower(inner);
        return f == "color" || f == "hover" || f == "any-hover" || f == "pointer" || f == "any-pointer";
    }
    std::string name = lower(trim(inner.substr(0, colon))), val = lower(trim(inner.substr(colon + 1)));
    int         mode = 0; // -1 min, +1 max, 0 exact
    if (starts_with(name, "min-")) { mode = -1; name = name.substr(4); }
    else if (starts_with(name, "max-")) { mode = 1; name = name.substr(4); }
    auto cmp = [&](double have, double want) { return mode < 0 ? have >= want : mode > 0 ? have <= want : have == want; };
    if (name == "width" || name == "device-width" || name == "height" || name == "device-height") {
        double want;
        if (!parse_len(val, want)) return false;
        return cmp(name.find("width") != std::string::npos ? env_.width : env_.height, want);
    }
    if (name == "resolution") {
        char*  end = nullptr;
        double v   = std::strtod(val.c_str(), &end);
        std::string u = trim(end);
        double dppx = u == "dpi" ? v / 96.0 : u == "dpcm" ? v * 2.54 / 96.0 : v;
        return cmp(1.0, dppx);
    }
    if (name == "-webkit-device-pixel-ratio" || name == "device-pixel-ratio") return cmp(1.0, std::strtod(val.c_str(), nullptr));
    if (name == "orientation") return val == (env_.width >= env_.height ? "landscape" : "portrait");
    if (name == "prefers-color-scheme") return val == "light";
    if (name == "prefers-reduced-motion" || name == "prefers-contrast" || name == "prefers-reduced-transparency") return val == "no-preference";
    if (name == "hover" || name == "any-hover") return val == "hover";
    if (name == "pointer" || name == "any-pointer") return val == "fine";
    if (name == "forced-colors") return val == "none";
    if (name == "scripting") return val == "none";
    if (name == "display-mode") return val == "browser";
    if (name == "color") return mode == 0 ? val == "8" : cmp(8, std::strtod(val.c_str(), nullptr));
    if (name == "monochrome") return cmp(0, std::strtod(val.c_str(), nullptr));
    return false;
}

bool CssVars::media_one(const std::string& q_in) const {
    std::string s = lower(trim(q_in));
    if (s.empty()) return true;
    bool neg = false;
    if (starts_with(s, "not ")) { neg = true; s = trim(s.substr(4)); }
    else if (starts_with(s, "only ")) s = trim(s.substr(5));
    std::vector<std::string> parts;
    size_t start = 0;
    int    depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        else if (depth == 0 && is_ws(s[i]) && s.compare(i + 1, 3, "and") == 0 && i + 4 < s.size() && is_ws(s[i + 4])) {
            parts.push_back(trim(s.substr(start, i - start)));
            start = i + 4;
            i += 3;
        }
    }
    parts.push_back(trim(s.substr(start)));
    bool all = true;
    for (auto& p : parts) {
        if (p.empty()) continue;
        if (p[0] == '(') {
            size_t close = scan_to(p, 1, p.size(), ")", false);
            if (!eval_feature(p.substr(1, close - 1))) all = false;
        } else if (!(p == "all" || p == "screen")) {
            all = false;
        }
    }
    return neg ? !all : all;
}

bool CssVars::media_matches(const std::string& query) const {
    std::string q = trim(query);
    if (q.empty()) return true;
    for (auto& one : split_commas(q))
        if (media_one(one)) return true;
    return false;
}

// ------------------------------------------------------------------ collection

CssVars::CssVars(MediaEnv env, ElementView root, ElementView body) : env_(env), root_(std::move(root)), body_(std::move(body)) {}

int CssVars::selector_rank(const std::string& selector, int& spec) const {
    std::string s = trim(selector);
    if (s == "*") { spec = 0; return 0; }
    int sp_b = 0, sp_r = 0;
    if (!body_.tag.empty() && match_compound(s, body_, false, sp_b)) { spec = sp_b; return 2; }
    if (match_compound(s, root_, true, sp_r)) { spec = sp_r; return 1; }
    return -1;
}

void CssVars::add_def(const std::string& name, const std::string& value, bool important, int rank, int spec) {
    Def nd{value, important, rank, spec, ++order_};
    auto it = defs_.find(name);
    if (it != defs_.end()) {
        const Def& o = it->second;
        if (std::make_tuple(nd.important, nd.rank, nd.spec) < std::make_tuple(o.important, o.rank, o.spec)) return;
    }
    defs_[name] = nd;
}

static const char kDashDash[] = "--";

void CssVars::collect_rules(const std::string& s, size_t b, size_t e) {
    for (const Rule& r : parse_rules(s, b, e)) {
        if (!r.has_block) continue;
        if (r.at) {
            if (r.name == "media") { if (media_matches(r.prelude)) collect_rules(s, r.bb, r.be); }
            else if (r.name == "supports") { if (!starts_with(lower(r.prelude), "not")) collect_rules(s, r.bb, r.be); }
            else if (r.name == "layer") collect_rules(s, r.bb, r.be);
            continue;
        }
        // no custom properties in this rule (searching the block only: an unbounded find ran on to the next "--"
        // in the sheet for every rule, quadratic on big sheets)
        if (std::search(s.begin() + static_cast<std::ptrdiff_t>(r.bb), s.begin() + static_cast<std::ptrdiff_t>(r.be), kDashDash, kDashDash + 2) ==
            s.begin() + static_cast<std::ptrdiff_t>(r.be))
            continue;
        std::vector<Decl> customs;
        for (auto& d : parse_decls(s, r.bb, r.be))
            if (d.valid && d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') customs.push_back(d);
        if (customs.empty()) continue;
        bool applied = false, scoped = false;
        for (auto& sel : split_commas(r.prelude)) {
            int spec = 0, rank = selector_rank(sel, spec);
            if (rank < 0) { scoped = true; continue; }
            applied = true;
            for (auto& d : customs) add_def(d.name, d.value, d.important, rank, spec);
        }
        if (scoped) {
            if (!applied) ++skipped_scoped;
            for (auto& d : customs) { tainted_.insert(d.name); scoped_defs_.push_back({d.name, d.value}); }
        }
    }
}

void CssVars::collect(const std::string& css) {
    if (css.find("--") == std::string::npos) return;
    std::string s = strip_comments(css);
    collect_rules(s, 0, s.size());
    memo_.clear();
}

static void var_names(const std::string& v, std::vector<std::string>& out) {
    size_t p = 0;
    while ((p = v.find("var(", p)) != std::string::npos) {
        if (p > 0 && is_ident(v[p - 1])) { p += 4; continue; }
        size_t q = p + 4;
        while (q < v.size() && is_ws(v[q])) ++q;
        size_t e = q;
        while (e < v.size() && is_ident(v[e])) ++e;
        if (e > q) out.push_back(v.substr(q, e - q));
        p += 4;
    }
}

void CssVars::finish() {
    // names whose (global or scoped) definition uses a tainted variable are tainted themselves
    for (bool changed = true; changed;) {
        changed = false;
        auto check = [&](const std::string& name, const std::string& val) {
            if (tainted_.count(name) || val.find("var(") == std::string::npos) return;
            std::vector<std::string> refs;
            var_names(val, refs);
            for (auto& r : refs) if (tainted_.count(r)) { tainted_.insert(name); changed = true; return; }
        };
        for (auto& kv : defs_) check(kv.first, kv.second.value);
        for (auto& sd : scoped_defs_) check(sd.first, sd.second);
    }
    memo_.clear();
}

bool CssVars::references_tainted(const std::string& value) const {
    if (tainted_.empty() || value.find("var(") == std::string::npos) return false;
    std::vector<std::string> refs;
    var_names(value, refs);
    for (auto& r : refs) if (tainted_.count(r)) return true;
    return false;
}

std::vector<std::string> CssVars::imports(const std::string& css) {
    std::vector<std::string> out;
    std::string s = strip_comments(css);
    for (const Rule& r : parse_rules(s, 0, s.size())) {
        if (!r.at || r.name != "import") continue;
        std::string p = r.prelude;
        std::string u;
        if (starts_with(lower(p), "url(")) {
            size_t close = scan_to(p, 4, p.size(), ")", false);
            u            = trim(p.substr(4, close - 4));
        } else {
            u = p.substr(0, scan_to(p, 0, p.size(), " \t\r\n", false));
        }
        if (u.size() >= 2 && (u[0] == '"' || u[0] == '\'') && u.back() == u[0]) u = u.substr(1, u.size() - 2);
        if (!u.empty()) out.push_back(u);
    }
    return out;
}

// ------------------------------------------------------------------ substitution

bool CssVars::lookup(const std::string& name, std::string& out, int depth) {
    auto m = memo_.find(name);
    if (m != memo_.end()) {
        if (!m->second.ok) return false;
        out = m->second.val;
        return true;
    }
    auto d = defs_.find(name);
    if (d == defs_.end() || depth > 16 || resolving_.count(name)) return false;
    resolving_.insert(name);
    std::string v;
    bool        ok = subst(d->second.value, v, depth + 1);
    resolving_.erase(name);
    memo_[name] = Memo{ok, v};
    if (ok) out = v;
    return ok;
}

bool CssVars::subst(const std::string& in, std::string& out, int depth) {
    out.clear();
    if (depth > 24) return false;
    size_t i = 0;
    while (i < in.size()) {
        size_t p = in.find("var(", i);
        while (p != std::string::npos && p > 0 && is_ident(in[p - 1])) p = in.find("var(", p + 1);
        if (p == std::string::npos) { out.append(in, i, std::string::npos); break; }
        out.append(in, i, p - i);
        size_t close = scan_to(in, p + 4, in.size(), ")", false);
        if (close >= in.size()) return false;
        std::string inner = in.substr(p + 4, close - p - 4);
        size_t      comma = scan_to(inner, 0, inner.size(), ",", false);
        std::string name  = trim(inner.substr(0, comma));
        bool        has_fb = comma < inner.size();
        std::string val;
        bool        ok = false;
        if (name.size() > 2 && name[0] == '-' && name[1] == '-') ok = lookup(name, val, depth);
        if (!ok && has_fb) ok = subst(trim(inner.substr(comma + 1)), val, depth + 1);
        if (!ok) return false;
        out += val;
        i = close + 1;
    }
    return true;
}

// libcss predates background-size: drop "/ <size>" from a background shorthand so the rest still parses
// (the size itself is handled by ElementCascade).
static std::string strip_bg_size(const std::string& v) {
    int depth = 0;
    char q = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (q) { if (c == '\\') ++i; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == '/' && depth == 0) {
            size_t j = i + 1;
            for (int k = 0; k < 2; ++k) {
                while (j < v.size() && is_ws(v[j])) ++j;
                size_t st = j;
                while (j < v.size() && !is_ws(v[j])) ++j;
                std::string tok = lower(v.substr(st, j - st));
                if (tok == "auto" || tok == "contain" || tok == "cover" || (!tok.empty() && (std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '.' || tok[0] == '-'))) continue;
                j = st;
                break;
            }
            return v.substr(0, i) + " " + v.substr(j);
        }
    }
    return v;
}

std::string CssVars::transform_decls(const std::string& s, size_t b, size_t e) {
    std::string out;
    for (auto& d : parse_decls(s, b, e)) {
        if (!d.valid) { out += d.raw + ";"; continue; }
        if (d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') continue; // custom property: consumed by collect()
        std::string val = d.value;
        if (val.find("var(") != std::string::npos && references_tainted(val)) { ++deferred; continue; }
        if (val.find("var(") != std::string::npos) {
            std::string r;
            if (!subst(val, r, 0)) { ++unresolved; continue; }
            ++substituted;
            val = r;
        }
        val = fold_calc(val);
        if (lower(d.name) == "background") val = strip_bg_size(val);
        out += d.name + ":" + val + (d.important ? " !important;" : ";");
    }
    return out;
}

std::string CssVars::transform_rules(const std::string& s, size_t b, size_t e) {
    std::string out;
    for (const Rule& r : parse_rules(s, b, e)) {
        if (!r.has_block) { out += s.substr(r.rb, r.re - r.rb) + "\n"; continue; }
        if (r.at) {
            static const char* nested[] = {"media", "supports", "layer", "container", "document", "-moz-document", "scope",
                                           "keyframes", "-webkit-keyframes", "starting-style"};
            static const char* decls[]  = {"font-face", "page", "counter-style", "font-palette-values", "position-try"};
            bool done = false;
            for (auto* n : nested)
                if (r.name == n) { out += "@" + r.name + " " + r.prelude + "{" + transform_rules(s, r.bb, r.be) + "}\n"; done = true; break; }
            if (!done)
                for (auto* n : decls)
                    if (r.name == n) { out += "@" + r.name + " " + r.prelude + "{" + transform_decls(s, r.bb, r.be) + "}\n"; done = true; break; }
            if (!done) out += s.substr(r.rb, r.re - r.rb) + "\n";
        } else {
            out += r.prelude + "{" + transform_decls(s, r.bb, r.be) + "}\n";
        }
    }
    return out;
}

std::string CssVars::apply(const std::string& css) {
    if (css.find("var(") == std::string::npos && css.find("--") == std::string::npos && css.find("calc(") == std::string::npos && css.find("max(") == std::string::npos &&
        css.find("min(") == std::string::npos && css.find("clamp(") == std::string::npos && !(css.find("background") != std::string::npos && css.find("/ ") != std::string::npos)) return css;
    std::string s = strip_comments(css);
    return transform_rules(s, 0, s.size());
}

std::string CssVars::apply_declarations(const std::string& decls) {
    if (decls.find("var(") == std::string::npos && decls.find("--") == std::string::npos && decls.find("calc(") == std::string::npos) return decls;
    std::string s = strip_comments(decls);
    return transform_decls(s, 0, s.size());
}

} // namespace quill::engine
