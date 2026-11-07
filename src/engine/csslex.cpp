// Shared CSS text helpers (tokenising rules/declarations, calc folding). See csslex.h.
#include "csslex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace quill::engine::lex {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) // ASCII only: CSS keywords and names fold that way (and it skips a locale call per byte)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}
bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

size_t skip_string(const std::string& s, size_t i, size_t e) {
    char q = s[i++];
    while (i < e) {
        if (s[i] == '\\') i += 2;
        else if (s[i] == q || s[i] == '\n') return i + 1;
        else ++i;
    }
    return e;
}

std::string strip_comments(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        // copy runs of ordinary text in one go: only quotes and "/*" need looking at
        size_t j = s.find_first_of("\"'/", i);
        if (j == std::string::npos) { o.append(s, i, std::string::npos); break; }
        if (j > i) { o.append(s, i, j - i); i = j; }
        char c = s[i];
        if (c == '"' || c == '\'') {
            size_t e = skip_string(s, i, s.size());
            o.append(s, i, e - i);
            i = e;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            i        = e == std::string::npos ? s.size() : e + 2;
            o += ' ';
        } else {
            o += c;
            ++i;
        }
    }
    return o;
}

// First char from `stops` at nesting depth 0 (outside strings), or `e`. ( and [ always nest; { only if `braces`.
size_t scan_to(const std::string& s, size_t i, size_t e, const char* stops, bool braces) {
    bool stop[256] = {};
    stop[0] = true; // (as strchr() would: it finds the terminator)
    for (const char* p = stops; *p; ++p) stop[static_cast<unsigned char>(*p)] = true;
    int depth = 0;
    while (i < e) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_string(s, i, e); continue; }
        if (c == '\\') { i += 2; continue; }
        if (depth == 0 && stop[static_cast<unsigned char>(c)]) return i;
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') { if (depth > 0) --depth; }
        else if (braces) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
        }
        ++i;
    }
    return e;
}

// s[i] == '{'. Returns the index just past the matching '}' (or e).
size_t block_end(const std::string& s, size_t i, size_t e) {
    int br = 0, par = 0;
    while (i < e) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_string(s, i, e); continue; }
        if (c == '\\') { i += 2; continue; }
        if (c == '(') ++par;
        else if (c == ')') { if (par > 0) --par; }
        else if (par == 0) {
            if (c == '{') ++br;
            else if (c == '}' && --br == 0) return i + 1;
        }
        ++i;
    }
    return e;
}


std::vector<Rule> parse_rules(const std::string& s, size_t b, size_t e) {
    std::vector<Rule> out;
    size_t i = b;
    while (i < e) {
        if (is_ws(s[i])) { ++i; continue; }
        if (s.compare(i, 4, "<!--") == 0) { i += 4; continue; }
        if (s.compare(i, 3, "-->") == 0) { i += 3; continue; }
        Rule r;
        r.rb = i;
        size_t p = i;
        if (s[i] == '@') {
            r.at = true;
            ++p;
            size_t q = p;
            while (q < e && is_ident(s[q])) ++q;
            r.name = lower(s.substr(p, q - p));
            p      = q;
        }
        size_t stop = scan_to(s, p, e, "{;", false);
        r.prelude   = trim(s.substr(p, stop - p));
        if (stop >= e) break;
        if (s[stop] == ';') {
            if (r.at) { r.re = stop + 1; out.push_back(r); }
            i = stop + 1;
            continue;
        }
        size_t end = block_end(s, stop, e);
        r.has_block = true;
        r.bb        = stop + 1;
        r.be        = end > r.bb && s[end - 1] == '}' ? end - 1 : end;
        r.re        = end;
        out.push_back(r);
        i = end;
    }
    return out;
}


std::vector<Decl> parse_decls(const std::string& s, size_t b, size_t e) {
    std::vector<Decl> out;
    size_t i = b;
    while (i < e) {
        size_t end = scan_to(s, i, e, ";", true);
        std::string seg = trim(s.substr(i, end - i));
        i               = end + 1;
        if (seg.empty()) continue;
        Decl d;
        d.raw         = seg;
        size_t colon  = scan_to(seg, 0, seg.size(), ":", false);
        if (colon < seg.size()) {
            d.name = trim(seg.substr(0, colon));
            d.valid = !d.name.empty() && std::all_of(d.name.begin(), d.name.end(), is_ident);
            d.value = trim(seg.substr(colon + 1));
            size_t bang = d.value.rfind('!');
            if (bang != std::string::npos && lower(trim(d.value.substr(bang + 1))) == "important") {
                d.important = true;
                d.value     = trim(d.value.substr(0, bang));
            }
        }
        out.push_back(d);
    }
    return out;
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = scan_to(s, i, s.size(), ",", false);
        out.push_back(trim(s.substr(i, e - i)));
        i = e + 1;
    }
    return out;
}

bool parse_len(const std::string& t, double& px) {
    const char* b = t.c_str();
    char*       end = nullptr;
    double      v   = std::strtod(b, &end);
    if (end == b) return false;
    std::string u = lower(trim(end));
    if (u == "px" || (u.empty() && v == 0)) px = v;
    else if (u == "em" || u == "rem") px = v * 16;
    else return false;
    return true;
}



// ---- calc() constant folding. libcss cannot evaluate calc() at all, so expressions that reduce to a
// single length/number (same-unit arithmetic) are folded here; mixed ones (100% - 10px) are left as-is
// and will need layout-time support.
struct CalcVal {
    double      v = 0;
    std::string unit;
};
// Makes two values comparable: equal units, or rem converted to px (the root font size is taken as 16px).
static bool calc_unify(CalcVal& a, CalcVal& b) {
    if (a.unit == b.unit) return true;
    auto topx = [](CalcVal& v) { if (v.unit == "rem") { v.v *= 16; v.unit = "px"; } };
    if ((a.unit == "rem" || a.unit == "px") && (b.unit == "rem" || b.unit == "px")) { topx(a); topx(b); return true; }
    return false;
}
struct CalcParser {
    const std::string& s;
    size_t             i = 0;
    explicit CalcParser(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && is_ws(s[i])) ++i; }
    bool expr(CalcVal& out) {
        if (!term(out)) return false;
        for (;;) {
            ws();
            if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return true;
            char op = s[i++];
            CalcVal r;
            if (!term(r)) return false;
            if (!calc_unify(out, r)) return false;
            out.v = op == '+' ? out.v + r.v : out.v - r.v;
        }
    }
    bool term(CalcVal& out) {
        if (!factor(out)) return false;
        for (;;) {
            ws();
            if (i >= s.size() || (s[i] != '*' && s[i] != '/')) return true;
            char op = s[i++];
            CalcVal r;
            if (!factor(r)) return false;
            if (op == '*') {
                if (!out.unit.empty() && !r.unit.empty()) return false;
                if (out.unit.empty()) out.unit = r.unit;
                out.v *= r.v;
            } else {
                if (!r.unit.empty() || r.v == 0) return false;
                out.v /= r.v;
            }
        }
    }
    bool factor(CalcVal& out) {
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '(') {
            ++i;
            if (!expr(out)) return false;
            ws();
            if (i >= s.size() || s[i] != ')') return false;
            ++i;
            return true;
        }
        if (s.compare(i, 5, "calc(") == 0) { i += 4; return factor(out); }
        for (const char* fn : {"max(", "min(", "clamp("}) {
            size_t fl = std::strlen(fn);
            if (s.compare(i, fl, fn) != 0) continue;
            i += fl;
            std::vector<CalcVal> args;
            for (;;) {
                CalcVal a;
                if (!expr(a)) return false;
                args.push_back(a);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ')') { ++i; break; }
                return false;
            }
            for (size_t k = 1; k < args.size(); ++k) if (!calc_unify(args[0], args[k])) return false;
            for (auto& a : args) a.unit = args[0].unit;
            if (fn[0] == 'c') { // clamp(min, val, max)
                if (args.size() != 3) return false;
                out = args[1];
                out.v = std::max(args[0].v, std::min(args[1].v, args[2].v));
                out.unit = args[0].unit;
            } else {
                out = args[0];
                for (auto& a : args) out.v = fn[1] == 'a' ? std::max(out.v, a.v) : std::min(out.v, a.v);
            }
            return true;
        }
        const char* b = s.c_str() + i;
        char*       end = nullptr;
        double      v = std::strtod(b, &end);
        if (end == b) return false;
        i += static_cast<size_t>(end - b);
        size_t u = i;
        while (u < s.size() && (std::isalpha(static_cast<unsigned char>(s[u])) || s[u] == '%')) ++u;
        out.v    = v;
        out.unit = lower(s.substr(i, u - i));
        i        = u;
        return true;
    }
};

std::string fold_calc(const std::string& in) {
    if (in.find("calc(") == std::string::npos && in.find("max(") == std::string::npos && in.find("min(") == std::string::npos &&
        in.find("clamp(") == std::string::npos)
        return in;
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        size_t p = std::string::npos;
        for (const char* fn : {"calc(", "max(", "min(", "clamp("}) {
            size_t q = in.find(fn, i);
            while (q != std::string::npos && q > 0 && (is_ident(in[q - 1]) || in[q - 1] == '-')) q = in.find(fn, q + 1);
            if (q != std::string::npos && (p == std::string::npos || q < p)) p = q;
        }
        if (p == std::string::npos) { out.append(in, i, std::string::npos); break; }
        out.append(in, i, p - i);
        CalcParser cp(in);
        cp.i = p;
        CalcVal r;
        bool    ok = cp.factor(r);
        if (ok) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.4f", r.v);
            std::string num = buf;
            while (!num.empty() && num.back() == '0') num.pop_back();
            if (!num.empty() && num.back() == '.') num.pop_back();
            if (num.empty() || num == "-0") num = "0";
            out += num + r.unit;
            i = cp.i;
        } else {
            // leave this call as written and carry on after its closing paren
            size_t open = in.find('(', p);
            size_t close = open == std::string::npos ? in.size() : scan_to(in, open + 1, in.size(), ")", false);
            if (close >= in.size()) { out.append(in, p, std::string::npos); break; }
            out.append(in, p, close + 1 - p);
            i = close + 1;
        }
    }
    return out;
}

} // namespace quill::engine::lex
