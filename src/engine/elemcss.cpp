// Per-element cascade. See elemcss.h.
#include "elemcss.h"

#include "csslex.h"
#include "../url.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <tuple>

namespace quill::engine {
using namespace lex;

namespace {

// ---------------------------------------------------------------- selectors

struct Complex;

struct Compound {
    std::string              tag; // empty = any
    std::vector<std::string> ids, classes;
    struct Attr {
        std::string name, op, val;
        bool        icase = false;
    };
    std::vector<Attr> attrs;
    struct Pseudo {
        std::string                          name;
        int                                  a = 0, b = 0; // nth-*: a n + b
        std::vector<std::shared_ptr<Complex>> args;        // :not / :is / :where
    };
    std::vector<Pseudo> pseudos;
    bool                never = false; // contains something that can never match a static document
};

struct Complex {
    std::vector<Compound> parts;
    std::vector<char>     comb; // comb[i]: combinator between parts[i-1] and parts[i]
    int                   a = 0, b = 0, c = 0;
    std::string           pseudo_el; // "before" / "after" / ""
    bool                  entry = false; // (state selectors only) the rule also carries var()/extra declarations
    int                   spec() const { return std::min(a, 99) * 10000 + std::min(b, 99) * 100 + std::min(c, 99); }
};

// Which interaction states (1 hover, 2 active, 4 focus, 8 focus-within) a compound (or anything nested in its :not/:is) tests.
int state_bits(const Compound& c) {
    int bits = 0;
    for (auto& p : c.pseudos) {
        if (p.name == "hover") bits |= 1;
        else if (p.name == "active") bits |= 2;
        else if (p.name == "focus" || p.name == "focus-visible") bits |= 4;
        else if (p.name == "focus-within") bits |= 8;
        for (auto& a : p.args)
            for (auto& pc : a->parts) bits |= state_bits(pc);
    }
    return bits;
}


bool is_id_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-' || static_cast<unsigned char>(c) >= 0x80 || c == '\\'; }
bool is_id_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || static_cast<unsigned char>(c) >= 0x80 || c == '\\'; }

struct SelParser {
    const std::string& s;
    size_t             i = 0;
    explicit SelParser(const std::string& str) : s(str) {}

    bool at_end() const { return i >= s.size(); }
    bool skip_ws() {
        bool any = false;
        while (i < s.size() && is_ws(s[i])) { ++i; any = true; }
        return any;
    }
    std::string ident() {
        std::string o;
        while (i < s.size() && is_id_char(s[i])) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                if (std::isxdigit(static_cast<unsigned char>(s[i]))) {
                    size_t j = i;
                    unsigned long cp = 0;
                    int n = 0;
                    while (j < s.size() && n < 6 && std::isxdigit(static_cast<unsigned char>(s[j]))) { cp = cp * 16 + static_cast<unsigned long>(std::strtol(std::string(1, s[j]).c_str(), nullptr, 16)); ++j; ++n; }
                    if (j < s.size() && is_ws(s[j])) ++j;
                    i = j;
                    if (cp < 0x80) o += static_cast<char>(cp);
                    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
                } else {
                    o += s[i++];
                }
            } else {
                o += s[i++];
            }
        }
        return o;
    }
    std::string paren_arg() { // at '('; returns the inside, leaves i after ')'
        size_t close = scan_to(s, i + 1, s.size(), ")", false);
        std::string a = s.substr(i + 1, close - i - 1);
        i = close < s.size() ? close + 1 : close;
        return a;
    }

    static bool parse_nth(std::string t, int& a, int& b) {
        t = lower(trim(t));
        std::string u;
        for (char c : t) if (!is_ws(c)) u += c;
        if (u == "odd") { a = 2; b = 1; return true; }
        if (u == "even") { a = 2; b = 0; return true; }
        size_t n = u.find('n');
        if (n == std::string::npos) { a = 0; b = std::atoi(u.c_str()); return !u.empty(); }
        std::string as = u.substr(0, n), bs = u.substr(n + 1);
        a = as.empty() || as == "+" ? 1 : as == "-" ? -1 : std::atoi(as.c_str());
        b = bs.empty() ? 0 : std::atoi(bs.c_str());
        return true;
    }

    bool parse_list(const std::string& text, std::vector<std::shared_ptr<Complex>>& out, int& max_spec_a, int& max_spec_b, int& max_spec_c) {
        for (auto& part : split_commas(text)) {
            auto c = std::make_shared<Complex>();
            SelParser p(part);
            if (!p.parse(*c) || !c->pseudo_el.empty()) return false;
            if (std::make_tuple(c->a, c->b, c->c) > std::make_tuple(max_spec_a, max_spec_b, max_spec_c)) { max_spec_a = c->a; max_spec_b = c->b; max_spec_c = c->c; }
            out.push_back(c);
        }
        return !out.empty();
    }

    bool parse_compound(Complex& cx, Compound& cp) {
        bool any = false;
        if (i < s.size() && s[i] == '*') { ++i; any = true; }
        else if (i < s.size() && is_id_start(s[i])) { cp.tag = lower(ident()); any = true; ++cx.c; }
        else if (i + 1 < s.size() && s[i] == '|') return false;
        while (i < s.size()) {
            char c = s[i];
            if (c == '#') { ++i; cp.ids.push_back(ident()); ++cx.a; any = true; }
            else if (c == '.') { ++i; cp.classes.push_back(ident()); ++cx.b; any = true; }
            else if (c == '[') {
                size_t end = scan_to(s, i + 1, s.size(), "]", false);
                if (end >= s.size()) return false;
                std::string in = s.substr(i + 1, end - i - 1);
                i = end + 1;
                Compound::Attr a;
                size_t k = 0;
                while (k < in.size() && !is_ws(in[k]) && in[k] != '=' && !std::strchr("~|^$*", in[k])) ++k;
                a.name = lower(in.substr(0, k));
                while (k < in.size() && is_ws(in[k])) ++k;
                if (k < in.size()) {
                    if (in[k] == '=') { a.op = "="; ++k; }
                    else { a.op = in.substr(k, 2); k += 2; }
                    while (k < in.size() && is_ws(in[k])) ++k;
                    std::string v = trim(in.substr(k));
                    if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
                        char q = v[0];
                        size_t e2 = v.find(q, 1);
                        std::string rest = e2 == std::string::npos ? "" : trim(v.substr(e2 + 1));
                        a.val = v.substr(1, e2 == std::string::npos ? std::string::npos : e2 - 1);
                        if (lower(rest) == "i") a.icase = true;
                    } else {
                        size_t sp = v.find_first_of(" \t");
                        if (sp != std::string::npos) { if (lower(trim(v.substr(sp))) == "i") a.icase = true; v = v.substr(0, sp); }
                        a.val = v;
                    }
                }
                cp.attrs.push_back(a);
                ++cx.b;
                any = true;
            } else if (c == ':') {
                if (i + 1 < s.size() && s[i + 1] == ':') {
                    i += 2;
                    std::string pe = lower(ident());
                    if (i < s.size() && s[i] == '(') paren_arg();
                    cx.pseudo_el = pe;
                    ++cx.c;
                    any = true;
                    continue;
                }
                ++i;
                std::string name = lower(ident()), arg;
                bool        fn = false;
                if (i < s.size() && s[i] == '(') { fn = true; arg = paren_arg(); }
                any = true;
                if (!fn && (name == "before" || name == "after" || name == "first-line" || name == "first-letter")) { cx.pseudo_el = name; ++cx.c; continue; }
                Compound::Pseudo ps;
                ps.name = name;
                if (name == "not" || name == "is" || name == "where" || name == "matches" || name == "-webkit-any" || name == "-moz-any") {
                    int ma = 0, mb = 0, mc = 0;
                    if (!parse_list(arg, ps.args, ma, mb, mc)) { cp.never = true; }
                    if (name != "where") { cx.a += ma; cx.b += mb; cx.c += mc; }
                } else if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type" || name == "nth-last-of-type") {
                    std::string a2 = arg;
                    size_t of = lower(a2).find(" of ");
                    if (of != std::string::npos) a2 = a2.substr(0, of);
                    if (!parse_nth(a2, ps.a, ps.b)) cp.never = true;
                    ++cx.b;
                } else if (name == "root" || name == "first-child" || name == "last-child" || name == "only-child" || name == "first-of-type" ||
                           name == "last-of-type" || name == "only-of-type" || name == "empty" || name == "checked" || name == "disabled" ||
                           name == "enabled" || name == "link" || name == "any-link" || name == "required" || name == "optional" ||
                           name == "read-only" || name == "placeholder-shown" || name == "hover" || name == "active" || name == "focus" ||
                           name == "focus-within" || name == "focus-visible") {
                    ++cx.b;
                } else {
                    ++cx.b;
                    cp.never = true; // :hover, :focus, :has(), :lang(), ... never match a static render
                }
                cp.pseudos.push_back(ps);
            } else {
                break;
            }
        }
        return any;
    }

    bool parse(Complex& cx) {
        skip_ws();
        for (;;) {
            Compound cp;
            if (!parse_compound(cx, cp)) return false;
            cx.parts.push_back(std::move(cp));
            bool  ws = skip_ws();
            if (at_end()) break;
            char comb = ' ';
            char c = s[i];
            if (c == '>' || c == '+' || c == '~') { comb = c; ++i; skip_ws(); }
            else if (!ws) return false;
            cx.comb.push_back(comb);
        }
        // comb[k] sits between parts[k] and parts[k+1]; shift so comb[i] precedes parts[i]
        cx.comb.insert(cx.comb.begin(), ' ');
        return !cx.parts.empty();
    }
};

// ---------------------------------------------------------------- colours / lengths

struct NamedColor { const char* n; uint32_t c; };
const NamedColor kNamed[] = {
    {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff}, {"yellow", 0xffff00},
    {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xc0c0c0}, {"maroon", 0x800000}, {"purple", 0x800080}, {"fuchsia", 0xff00ff},
    {"magenta", 0xff00ff}, {"lime", 0x00ff00}, {"olive", 0x808000}, {"navy", 0x000080}, {"teal", 0x008080}, {"aqua", 0x00ffff},
    {"cyan", 0x00ffff}, {"orange", 0xffa500}, {"pink", 0xffc0cb}, {"brown", 0xa52a2a}, {"gold", 0xffd700}, {"darkgray", 0xa9a9a9},
    {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3}, {"dimgray", 0x696969}, {"darkred", 0x8b0000},
    {"darkblue", 0x00008b}, {"darkgreen", 0x006400}, {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90}, {"indigo", 0x4b0082},
    {"violet", 0xee82ee}, {"crimson", 0xdc143c}, {"coral", 0xff7f50}, {"tomato", 0xff6347}, {"salmon", 0xfa8072}, {"khaki", 0xf0e68c},
    {"beige", 0xf5f5dc}, {"ivory", 0xfffff0}, {"lavender", 0xe6e6fa}, {"gainsboro", 0xdcdcdc}, {"whitesmoke", 0xf5f5f5},
    {"steelblue", 0x4682b4}, {"royalblue", 0x4169e1}, {"dodgerblue", 0x1e90ff}, {"skyblue", 0x87ceeb}, {"slategray", 0x708090},
};

double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

uint32_t pack(double r, double g, double b, double a) {
    auto c8 = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
    return (c8(a * 255) << 24) | (c8(r) << 16) | (c8(g) << 8) | c8(b);
}

std::vector<std::string> split_args(const std::string& in) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : in) {
        if (c == ',' || c == '/' || is_ws(c)) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

double num_or_pct(const std::string& t, double full) {
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end && *end == '%') return v * full / 100.0;
    return v;
}

} // namespace

bool parse_css_color(const std::string& text, uint32_t& argb, bool& current) {
    std::string t = lower(trim(text));
    current = false;
    if (t.empty()) return false;
    if (t == "transparent") { argb = 0; return true; }
    if (t == "currentcolor") { current = true; return true; }
    if (t[0] == '#') {
        std::string h = t.substr(1);
        for (char c : h) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        auto hx = [&](size_t i, size_t n) { return static_cast<unsigned>(std::strtoul(h.substr(i, n).c_str(), nullptr, 16)); };
        if (h.size() == 3 || h.size() == 4) {
            unsigned r = hx(0, 1) * 17, g = hx(1, 1) * 17, b = hx(2, 1) * 17, a = h.size() == 4 ? hx(3, 1) * 17 : 255;
            argb = (a << 24) | (r << 16) | (g << 8) | b;
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            unsigned r = hx(0, 2), g = hx(2, 2), b = hx(4, 2), a = h.size() == 8 ? hx(6, 2) : 255;
            argb = (a << 24) | (r << 16) | (g << 8) | b;
            return true;
        }
        return false;
    }
    size_t p = t.find('(');
    if (p != std::string::npos && t.back() == ')') {
        std::string fn = t.substr(0, p);
        auto args = split_args(t.substr(p + 1, t.size() - p - 2));
        if ((fn == "rgb" || fn == "rgba") && args.size() >= 3) {
            double a = args.size() >= 4 ? num_or_pct(args[3], 1.0) : 1.0;
            argb = pack(num_or_pct(args[0], 255), num_or_pct(args[1], 255), num_or_pct(args[2], 255), a);
            return true;
        }
        if ((fn == "hsl" || fn == "hsla") && args.size() >= 3) {
            double h = std::fmod(std::strtod(args[0].c_str(), nullptr), 360.0);
            if (h < 0) h += 360;
            h /= 360;
            double s = num_or_pct(args[1], 1.0), l = num_or_pct(args[2], 1.0);
            if (args[1].find('%') != std::string::npos) s = std::strtod(args[1].c_str(), nullptr) / 100;
            if (args[2].find('%') != std::string::npos) l = std::strtod(args[2].c_str(), nullptr) / 100;
            double a = args.size() >= 4 ? num_or_pct(args[3], 1.0) : 1.0;
            double r, g, b;
            if (s == 0) r = g = b = l;
            else {
                double q = l < 0.5 ? l * (1 + s) : l + s - l * s, pp = 2 * l - q;
                r = hue2rgb(pp, q, h + 1.0 / 3);
                g = hue2rgb(pp, q, h);
                b = hue2rgb(pp, q, h - 1.0 / 3);
            }
            argb = pack(r * 255, g * 255, b * 255, a);
            return true;
        }
        return false;
    }
    for (auto& nc : kNamed)
        if (t == nc.n) { argb = 0xff000000u | nc.c; return true; }
    return false;
}

namespace {

bool parse_length(const std::string& in, Length& out) {
    std::string t = lower(trim(in));
    if (t.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    std::string u = trim(end);
    if (u == "px" || (u.empty() && v == 0)) out = Length::px(static_cast<float>(v));
    else if (u == "em" || u == "rem") out = Length::px(static_cast<float>(v * 16));
    else if (u == "pt") out = Length::px(static_cast<float>(v * 4 / 3));
    else if (u == "%") out = Length::pct(static_cast<float>(v));
    else return false;
    return true;
}

std::vector<std::string> split_top_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (is_ws(c) && depth == 0) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------- the cascade

enum Kind { K_CUSTOM, K_RADIUS_ALL, K_TL, K_TR, K_BR, K_BL, K_SHADOW, K_MASK_IMG, K_MASK_ALL, K_MASK_SIZE, K_MASK_REPEAT, K_MASK_POS, K_BG_SIZE, K_BG_ALL, K_SVGPROP, K_GRID, K_TRANSFORM, K_APPEARANCE, K_BG_IMAGE, K_TRANSFORM_ORIGIN, K_ROTATE, K_SCALE, K_TRANSLATE, K_DEFERRED };

struct CDecl {
    std::string name, value;
    bool        important = false;
    int         kind = K_CUSTOM;
};

struct Entry {
    Complex            sel;
    size_t             order = 0;
    std::vector<CDecl> decls;
    bool               custom = false, extra = false, deferred = false;
};

using Prio = std::tuple<int, int, size_t>; // important, specificity, order

int extra_kind(const std::string& n) {
    if (n == "border-radius") return K_RADIUS_ALL;
    if (n == "border-top-left-radius") return K_TL;
    if (n == "border-top-right-radius") return K_TR;
    if (n == "border-bottom-right-radius") return K_BR;
    if (n == "border-bottom-left-radius") return K_BL;
    if (n == "box-shadow") return K_SHADOW;
    if (n == "mask-image" || n == "-webkit-mask-image") return K_MASK_IMG;
    if (n == "mask" || n == "-webkit-mask") return K_MASK_ALL;
    if (n == "mask-size" || n == "-webkit-mask-size") return K_MASK_SIZE;
    if (n == "mask-repeat" || n == "-webkit-mask-repeat") return K_MASK_REPEAT;
    if (n == "mask-position" || n == "-webkit-mask-position") return K_MASK_POS;
    for (const char* sp : {"fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-opacity", "fill-opacity", "fill-rule",
                           "stop-color", "stop-opacity", "stroke-dasharray", "stroke-miterlimit"})
        if (n == sp) return K_SVGPROP;
    if (n == "transform" || n == "-webkit-transform" || n == "-ms-transform") return K_TRANSFORM;
    if (n == "transform-origin" || n == "-webkit-transform-origin") return K_TRANSFORM_ORIGIN;
    if (n == "rotate") return K_ROTATE;
    if (n == "scale") return K_SCALE;
    if (n == "translate") return K_TRANSLATE;
    if (n == "background-image") return K_BG_IMAGE;
    if (n == "appearance" || n == "-webkit-appearance" || n == "-moz-appearance") return K_APPEARANCE;
    if (n == "background-size") return K_BG_SIZE;
    if (n == "background") return K_BG_ALL;
    for (const char* g : {"grid-template-columns", "grid-template-rows", "grid-template-areas", "grid-template", "grid", "grid-column", "grid-column-start",
                          "grid-column-end", "grid-row", "grid-row-start", "grid-row-end", "grid-area", "grid-auto-flow", "grid-auto-columns", "grid-auto-rows",
                          "justify-items", "justify-self", "place-items", "place-self", "place-content", "align-items", "align-self", "align-content",
                          "justify-content"})
        if (n == g) return K_GRID;
    return -1;
}

// Reads the argument of url( starting at `a` (just after the paren): quoted strings may contain ')' and backslash escapes.
bool read_url_arg(const std::string& v, size_t a, std::string& out, size_t& end) {
    size_t i = a;
    while (i < v.size() && is_ws(v[i])) ++i;
    out.clear();
    if (i < v.size() && (v[i] == '"' || v[i] == '\'')) {
        char q = v[i++];
        while (i < v.size() && v[i] != q) {
            if (v[i] == '\\' && i + 1 < v.size()) { ++i; if (v[i] == '\n') { ++i; continue; } }
            out += v[i++];
        }
        if (i >= v.size()) return false;
        ++i;
        while (i < v.size() && is_ws(v[i])) ++i;
        if (i >= v.size() || v[i] != ')') return false;
        end = i + 1;
        return true;
    }
    size_t e = v.find(')', i);
    if (e == std::string::npos) return false;
    out = trim(v.substr(i, e - i));
    end = e + 1;
    return true;
}

struct Env {
    const Env*                                   parent = nullptr;
    std::unordered_map<std::string, std::string> vars;
};
const char kInvalid[] = "\x01";

} // namespace

struct ElementCascade::Impl {
    const CssVars&                   vars;
    std::vector<ENode>               nodes;
    std::vector<std::vector<int>>    kids;
    std::vector<int>                 prev, index, count;
    std::vector<std::vector<std::string>> classes;
    std::vector<std::string>         ids;
    std::vector<Entry>               entries;
    std::vector<Complex>             hover_sels; // every selector (of any rule) that mentions an interaction state
    bool                             state_entries = false; // some var()/extra rule depends on an interaction state
    mutable bool                     ignore_state = false; // pseudo_ok treats :hover/:active/:focus* as satisfied
    std::unordered_map<std::string, std::vector<int>> by_id, by_class, by_tag;
    std::vector<int>                 universal;
    size_t                           order = 0;

    std::vector<std::vector<int>>    marks;
    std::vector<Extras>              extras;
    std::vector<char>                has_extras;
    std::vector<Extras>              pextras[2];   // ::before, ::after
    std::vector<char>                has_pextras[2];
    struct Variant { int id; int k; std::string pseudo, text; size_t order; };
    std::vector<Variant>             variants;
    std::map<std::string, int>       variant_ids;
    std::vector<std::unique_ptr<Env>> envs;
    std::vector<const Env*>          node_env;

    Impl(const CssVars& v, std::vector<ENode> n) : vars(v), nodes(std::move(n)) {
        size_t N = nodes.size();
        kids.assign(N, {});
        prev.assign(N, -1);
        index.assign(N, 1);
        count.assign(N, 1);
        classes.assign(N, {});
        ids.assign(N, "");
        marks.assign(N, {});
        extras.assign(N, Extras{});
        has_extras.assign(N, 0);
        for (int k = 0; k < 2; ++k) { pextras[k].assign(N, Extras{}); has_pextras[k].assign(N, 0); }
        node_env.assign(N, nullptr);
        for (size_t i = 0; i < N; ++i) {
            int p = nodes[i].parent;
            if (p >= 0) {
                if (!kids[static_cast<size_t>(p)].empty()) prev[i] = kids[static_cast<size_t>(p)].back();
                kids[static_cast<size_t>(p)].push_back(static_cast<int>(i));
            }
            for (auto& a : nodes[i].attrs) {
                if (a.first == "id") ids[i] = a.second;
                else if (a.first == "class") {
                    size_t k = 0;
                    const std::string& v2 = a.second;
                    while (k < v2.size()) {
                        while (k < v2.size() && is_ws(v2[k])) ++k;
                        size_t j = k;
                        while (j < v2.size() && !is_ws(v2[j])) ++j;
                        if (j > k) classes[i].push_back(v2.substr(k, j - k));
                        k = j;
                    }
                }
            }
        }
        for (auto& ks : kids)
            for (size_t j = 0; j < ks.size(); ++j) {
                index[static_cast<size_t>(ks[j])] = static_cast<int>(j) + 1;
                count[static_cast<size_t>(ks[j])] = static_cast<int>(ks.size());
            }
    }

    // ---- matching
    const std::string* attr(int n, const std::string& name) const {
        for (auto& a : nodes[static_cast<size_t>(n)].attrs)
            if (a.first == name) return &a.second;
        return nullptr;
    }
    static bool nth_ok(int a, int b, int pos) {
        if (a == 0) return pos == b;
        int diff = pos - b;
        return diff % a == 0 && diff / a >= 0;
    }
    int type_index(int n, bool from_end) const {
        int p = nodes[static_cast<size_t>(n)].parent, pos = 0;
        if (p < 0) return 1;
        const auto& ks = kids[static_cast<size_t>(p)];
        const std::string& tg = nodes[static_cast<size_t>(n)].tag;
        if (!from_end) {
            for (int k : ks) { if (nodes[static_cast<size_t>(k)].tag == tg) ++pos; if (k == n) break; }
        } else {
            for (size_t j = ks.size(); j-- > 0;) { if (nodes[static_cast<size_t>(ks[j])].tag == tg) ++pos; if (ks[j] == n) break; }
        }
        return pos;
    }
    int type_count(int n) const {
        int p = nodes[static_cast<size_t>(n)].parent, c = 0;
        if (p < 0) return 1;
        for (int k : kids[static_cast<size_t>(p)]) if (nodes[static_cast<size_t>(k)].tag == nodes[static_cast<size_t>(n)].tag) ++c;
        return c;
    }
    bool attr_ok(const Compound::Attr& a, int n) const {
        const std::string* v = attr(n, a.name);
        if (!v) return false;
        if (a.op.empty()) return true;
        std::string have = a.icase ? lower(*v) : *v, want = a.icase ? lower(a.val) : a.val;
        char op = a.op[0];
        if (a.op == "=") return have == want;
        switch (op) {
        case '^': return !want.empty() && have.compare(0, want.size(), want) == 0;
        case '$': return !want.empty() && have.size() >= want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0;
        case '*': return !want.empty() && have.find(want) != std::string::npos;
        case '|': return have == want || have.compare(0, want.size() + 1, want + "-") == 0;
        case '~': {
            size_t k = 0;
            while (k < have.size()) {
                while (k < have.size() && is_ws(have[k])) ++k;
                size_t j = k;
                while (j < have.size() && !is_ws(have[j])) ++j;
                if (j > k && have.substr(k, j - k) == want) return true;
                k = j;
            }
            return false;
        }
        }
        return false;
    }
    bool pseudo_ok(const Compound::Pseudo& ps, int n) const {
        const std::string& nm = ps.name;
        size_t un = static_cast<size_t>(n);
        if (nm == "root") return n == 0;
        if (nm == "first-child") return index[un] == 1;
        if (nm == "last-child") return index[un] == count[un];
        if (nm == "only-child") return count[un] == 1;
        if (nm == "nth-child") return nth_ok(ps.a, ps.b, index[un]);
        if (nm == "nth-last-child") return nth_ok(ps.a, ps.b, count[un] - index[un] + 1);
        if (nm == "nth-of-type") return nth_ok(ps.a, ps.b, type_index(n, false));
        if (nm == "nth-last-of-type") return nth_ok(ps.a, ps.b, type_index(n, true));
        if (nm == "first-of-type") return type_index(n, false) == 1;
        if (nm == "last-of-type") return type_index(n, true) == 1;
        if (nm == "only-of-type") return type_count(n) == 1;
        if (nm == "empty") return kids[un].empty() && !attr(n, "data-qv-text");
        if (nm == "hover") return ignore_state || nodes[un].hover;
        if (nm == "active") return ignore_state || nodes[un].active;
        if (nm == "focus" || nm == "focus-visible") return ignore_state || nodes[un].focus;
        if (nm == "focus-within") return ignore_state || nodes[un].focus_within;
        if (nm == "checked") return attr(n, "checked") || attr(n, "selected");
        if (nm == "disabled") return attr(n, "disabled") != nullptr;
        if (nm == "enabled") return attr(n, "disabled") == nullptr && (nodes[un].tag == "input" || nodes[un].tag == "button" || nodes[un].tag == "select" || nodes[un].tag == "textarea");
        if (nm == "link" || nm == "any-link") return (nodes[un].tag == "a" || nodes[un].tag == "area") && attr(n, "href");
        if (nm == "required") return attr(n, "required") != nullptr;
        if (nm == "optional") return attr(n, "required") == nullptr;
        if (nm == "read-only") return attr(n, "readonly") != nullptr;
        if (nm == "placeholder-shown") return attr(n, "placeholder") && (!attr(n, "value") || attr(n, "value")->empty());
        if (nm == "not") {
            for (auto& a : ps.args) if (match(*a, n)) return false;
            return true;
        }
        if (nm == "is" || nm == "where" || nm == "matches" || nm == "-webkit-any" || nm == "-moz-any") {
            for (auto& a : ps.args) if (match(*a, n)) return true;
            return false;
        }
        return false;
    }
    bool compound_ok(const Compound& c, int n) const {
        if (c.never) return false;
        size_t un = static_cast<size_t>(n);
        if (!c.tag.empty() && c.tag != nodes[un].tag) return false;
        for (auto& id : c.ids) if (ids[un] != id) return false;
        for (auto& cl : c.classes)
            if (std::find(classes[un].begin(), classes[un].end(), cl) == classes[un].end()) return false;
        for (auto& a : c.attrs) if (!attr_ok(a, n)) return false;
        for (auto& p : c.pseudos) if (!pseudo_ok(p, n)) return false;
        return true;
    }
    bool match_from(const Complex& c, int i, int n) const {
        if (!compound_ok(c.parts[static_cast<size_t>(i)], n)) return false;
        if (i == 0) return true;
        char comb = c.comb[static_cast<size_t>(i)];
        size_t un = static_cast<size_t>(n);
        if (comb == '>') { int p = nodes[un].parent; return p >= 0 && match_from(c, i - 1, p); }
        if (comb == ' ') {
            for (int p = nodes[un].parent; p >= 0; p = nodes[static_cast<size_t>(p)].parent)
                if (match_from(c, i - 1, p)) return true;
            return false;
        }
        if (comb == '+') { int s = prev[un]; return s >= 0 && match_from(c, i - 1, s); }
        for (int s = prev[un]; s >= 0; s = prev[static_cast<size_t>(s)])
            if (match_from(c, i - 1, s)) return true;
        return false;
    }
    bool match(const Complex& c, int n) const { return match_from(c, static_cast<int>(c.parts.size()) - 1, n); }

    // ---- rules
    std::string cur_base;
    // Rewrites url(...) tokens to absolute url("...") using the current sheet's base.
    std::string absolutize(const std::string& v) const {
        std::string o;
        size_t i = 0;
        while (i < v.size()) {
            size_t u = v.find("url(", i);
            if (u == std::string::npos) { o += v.substr(i); break; }
            o += v.substr(i, u - i);
            std::string in;
            size_t end = 0;
            if (!read_url_arg(v, u + 4, in, end)) { o += v.substr(u); break; }
            std::string abs = (in.rfind("data:", 0) == 0 || cur_base.empty()) ? in : resolve_url(cur_base, in);
            // re-emit as a double-quoted string
            std::string q;
            for (char c : abs) { if (c == '"' || c == '\\') q += '\\'; q += c; }
            o += "url(\"" + q + "\")";
            i = end;
        }
        return o;
    }

    bool kind_of_decl(const Decl& d, CDecl& out) {
        if (!d.valid) return false;
        out.name = d.name;
        out.value = d.value;
        out.important = d.important;
        if (d.name.size() > 2 && d.name[0] == '-' && d.name[1] == '-') { out.kind = K_CUSTOM; return true; }
        int ek = extra_kind(lower(d.name));
        // shorthands libcss also needs (background) go the deferred route when they use scoped variables
        if ((ek == K_BG_ALL || ek == K_BG_IMAGE) && d.value.find("var(") != std::string::npos && vars.references_tainted(d.value) &&
            lower(d.value).find("gradient(") == std::string::npos)
            ek = -1;
        if (ek >= 0) {
            out.kind = ek;
            out.name = lower(d.name);
            if (ek == K_MASK_IMG || ek == K_MASK_ALL) out.value = absolutize(d.value);
            return true;
        }
        if (d.value.find("var(") != std::string::npos && vars.references_tainted(d.value)) { out.kind = K_DEFERRED; return true; }
        return false;
    }

    void add_rule(const Rule& r, const std::string& s) {
        std::vector<CDecl> decls;
        for (auto& d : parse_decls(s, r.bb, r.be)) {
            CDecl cd;
            if (kind_of_decl(d, cd)) decls.push_back(cd);
        }
        {
            std::string lp = lower(r.prelude);
            if (lp.find(":hover") != std::string::npos || lp.find(":active") != std::string::npos || lp.find(":focus") != std::string::npos)
                for (auto& sel : split_commas(r.prelude)) {
                    std::string ls = lower(sel);
                    SelParser   hp(sel);
                    Complex     hc;
                    if ((ls.find(":hover") != std::string::npos || ls.find(":active") != std::string::npos || ls.find(":focus") != std::string::npos) &&
                        hp.parse(hc) && hover_sels.size() < 2000) {
                        hc.entry = !decls.empty();
                        hover_sels.push_back(std::move(hc));
                    }
                }
        }
        if (decls.empty()) return;
        for (auto& sel : split_commas(r.prelude)) {
            Entry e;
            SelParser p(sel);
            if (!p.parse(e.sel)) continue;
            if (!e.sel.pseudo_el.empty() && e.sel.pseudo_el != "before" && e.sel.pseudo_el != "after") continue;
            e.order = ++order;
            for (auto& d : decls) {
                if (!e.sel.pseudo_el.empty() && (d.kind == K_CUSTOM)) continue;
                e.decls.push_back(d);
                if (d.kind == K_CUSTOM) e.custom = true;
                else if (d.kind == K_DEFERRED) e.deferred = true;
                else e.extra = true;
            }
            if (e.decls.empty()) continue;
            for (auto& pt : e.sel.parts) if (state_bits(pt)) state_entries = true;
            int idx = static_cast<int>(entries.size());
            const Compound& last = e.sel.parts.back();
            if (!last.ids.empty()) by_id[last.ids[0]].push_back(idx);
            else if (!last.classes.empty()) by_class[last.classes[0]].push_back(idx);
            else if (!last.tag.empty()) by_tag[last.tag].push_back(idx);
            else universal.push_back(idx);
            entries.push_back(std::move(e));
        }
    }

    void rules_in(const std::string& s, size_t b, size_t e) {
        for (const Rule& r : parse_rules(s, b, e)) {
            if (!r.has_block) continue;
            if (r.at) {
                if (r.name == "media") { if (vars.media_matches(r.prelude)) rules_in(s, r.bb, r.be); }
                else if (r.name == "supports") { if (!starts_with(lower(r.prelude), "not")) rules_in(s, r.bb, r.be); }
                else if (r.name == "layer") rules_in(s, r.bb, r.be);
                continue;
            }
            add_rule(r, s);
        }
    }

    // ---- values
    bool lookup_env(const Env* e, const std::string& name, std::string& out) const {
        for (; e; e = e->parent) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) {
                if (it->second == kInvalid) return false;
                out = it->second;
                return true;
            }
        }
        return false;
    }

    using Look = std::function<bool(const std::string&, std::string&)>;
    bool subst(const std::string& in, const Look& look, std::string& out, int depth) const {
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
            std::string name = trim(inner.substr(0, comma));
            bool        has_fb = comma < inner.size();
            std::string val;
            bool        ok = false;
            if (name.size() > 2 && name[0] == '-' && name[1] == '-') ok = look(name, val);
            if (!ok && has_fb) ok = subst(trim(inner.substr(comma + 1)), look, val, depth + 1);
            if (!ok) return false;
            out += val;
            i = close + 1;
        }
        return true;
    }

    std::string resolve_value(const std::string& v, const Env* env) const {
        std::string out;
        if (v.find("var(") == std::string::npos) return fold_calc(v);
        if (!subst(v, [&](const std::string& n, std::string& o) { return lookup_env(env, n, o); }, out, 0)) return std::string(kInvalid);
        return fold_calc(out);
    }

    // ---- per-node computation
    // Angles: deg, rad, grad, turn (a bare 0 is 0).
    static bool parse_angle(const std::string& a, float& deg) {
        std::string s = lower(trim(a));
        if (s.empty()) return false;
        char* end = nullptr;
        float f = std::strtof(s.c_str(), &end);
        if (end == s.c_str()) return false;
        std::string u = trim(std::string(end));
        if (u == "deg") deg = f;
        else if (u == "rad") deg = f * 57.2957795f;
        else if (u == "grad") deg = f * 0.9f;
        else if (u == "turn") deg = f * 360.f;
        else if (u.empty() && f == 0) deg = 0;
        else return false;
        return true;
    }
    static void mul(float m[4], const float f[4]) { // m = m * f (f applied first)
        float a = m[0] * f[0] + m[2] * f[1], b = m[1] * f[0] + m[3] * f[1];
        float c = m[0] * f[2] + m[2] * f[3], d = m[1] * f[2] + m[3] * f[3];
        m[0] = a; m[1] = b; m[2] = c; m[3] = d;
    }
    // translate()/translateX/Y/3d become a visual offset (applied in layout); rotate / scale / skew / matrix become the
    // linear part applied when painting. Functions compose left to right (translations are summed apart from it).
    void parse_transform(const std::string& v, Extras& ex) {
        ex.has_transform = false;
        ex.has_xform = false;
        ex.xf[0] = 1; ex.xf[1] = 0; ex.xf[2] = 0; ex.xf[3] = 1;
        ex.tx_px = ex.tx_em = ex.tx_pct = ex.ty_px = ex.ty_em = ex.ty_pct = 0;
        std::string t = lower(trim(v));
        size_t i = 0;
        auto part = [](const std::string& a, float& px, float& em, float& pct) {
            std::string s = trim(a);
            if (s.empty()) return;
            char* end = nullptr;
            float f = std::strtof(s.c_str(), &end);
            std::string u = end ? trim(std::string(end)) : "";
            if (u == "%") pct += f;
            else if (u == "em") em += f;
            else if (u == "rem") px += f * 16.f;
            else if (u == "px" || u.empty()) px += f;
        };
        auto num = [](const std::string& a, float& out) {
            std::string s = trim(a);
            char* end = nullptr;
            float f = std::strtof(s.c_str(), &end);
            if (end == s.c_str()) return false;
            std::string u = trim(std::string(end));
            if (u == "%") f /= 100.f;
            else if (!u.empty()) return false;
            out = f;
            return true;
        };
        float m[4] = {1, 0, 0, 1};
        while (i < t.size()) {
            size_t p = t.find('(', i);
            if (p == std::string::npos) break;
            size_t q = t.find(')', p);
            if (q == std::string::npos) break;
            std::string name = trim(t.substr(i, p - i)), args = t.substr(p + 1, q - p - 1);
            auto a = split_args(args);
            if (name == "translate" || name == "translate3d") {
                if (!a.empty()) part(a[0], ex.tx_px, ex.tx_em, ex.tx_pct);
                if (a.size() > 1) part(a[1], ex.ty_px, ex.ty_em, ex.ty_pct);
                ex.has_transform = true;
            } else if (name == "translatex") {
                if (!a.empty()) part(a[0], ex.tx_px, ex.tx_em, ex.tx_pct);
                ex.has_transform = true;
            } else if (name == "translatey") {
                if (!a.empty()) part(a[0], ex.ty_px, ex.ty_em, ex.ty_pct);
                ex.has_transform = true;
            } else if (name == "rotate" || name == "rotatez") {
                float d;
                if (!a.empty() && parse_angle(a[0], d)) {
                    float r = d * 0.0174532925f, c = std::cos(r), s2 = std::sin(r);
                    float f[4] = {c, s2, -s2, c};
                    mul(m, f);
                }
            } else if (name == "scale" || name == "scale3d" || name == "scalex" || name == "scaley") {
                float sx = 1, sy = 1;
                if (name == "scaley") { if (!a.empty()) num(a[0], sy); }
                else {
                    if (!a.empty() && num(a[0], sx)) sy = sx;
                    if (name == "scalex") sy = 1;
                    else if (a.size() > 1) num(a[1], sy);
                }
                float f[4] = {sx, 0, 0, sy};
                mul(m, f);
            } else if (name == "skew" || name == "skewx" || name == "skewy") {
                float ax = 0, ay = 0;
                if (name == "skewy") { if (!a.empty()) parse_angle(a[0], ay); }
                else {
                    if (!a.empty()) parse_angle(a[0], ax);
                    if (name == "skew" && a.size() > 1) parse_angle(a[1], ay);
                }
                float f[4] = {1, std::tan(ay * 0.0174532925f), std::tan(ax * 0.0174532925f), 1};
                mul(m, f);
            } else if (name == "matrix" && a.size() == 6) {
                float v6[6];
                bool  ok = true;
                for (int k = 0; k < 6; k++) ok = ok && num(a[static_cast<size_t>(k)], v6[k]);
                if (ok) {
                    float f[4] = {v6[0], v6[1], v6[2], v6[3]};
                    mul(m, f);
                    ex.tx_px += v6[4];
                    ex.ty_px += v6[5];
                    ex.has_transform = true;
                }
            }
            i = q + 1;
        }
        if (std::fabs(m[0] - 1) > 1e-4f || std::fabs(m[1]) > 1e-4f || std::fabs(m[2]) > 1e-4f || std::fabs(m[3] - 1) > 1e-4f) {
            ex.has_xform = true;
            for (int k = 0; k < 4; k++) ex.xf[k] = m[k];
        }
        if (ex.has_xform) ex.has_transform = true;
        if (t == "none") ex.has_transform = true; // an explicit none still overrides
    }

    // transform-origin: one or two of left / center / right / top / bottom, lengths, percentages
    void parse_origin(const std::string& v, Extras& ex) {
        auto toks = split_top_ws(lower(trim(v)));
        Length x = Length::pct(50), y = Length::pct(50);
        int    n = 0;
        bool   xset = false;
        for (auto& tk : toks) {
            if (n >= 2) break;
            if (tk == "left") { x = Length::pct(0); xset = true; }
            else if (tk == "right") { x = Length::pct(100); xset = true; }
            else if (tk == "top") y = Length::pct(0);
            else if (tk == "bottom") y = Length::pct(100);
            else if (tk == "center") { if (!xset && n == 0) xset = true; }
            else {
                Length l;
                if (!parse_length(tk, l)) break;
                if (!xset) { x = l; xset = true; } else y = l;
            }
            n++;
        }
        ex.origin_x = x;
        ex.origin_y = y;
        ex.has_origin = true;
    }

    // ---- gradients
    static void parse_stops(const std::vector<std::string>& args, size_t from, Gradient& g, bool angles) {
        for (size_t k = from; k < args.size() && g.stops.size() < 64; k++) {
            auto toks = split_top_ws(trim(args[k]));
            if (toks.empty()) continue;
            GradStop st;
            uint32_t c;
            bool     cur;
            if (!parse_css_color(toks[0], c, cur)) continue; // a colour hint (lone position): ignored
            st.color   = c;
            st.current = cur;
            std::vector<Length> pos;
            for (size_t j = 1; j < toks.size() && pos.size() < 2; j++) {
                Length l;
                float  d;
                if (angles && parse_angle(toks[j], d)) pos.push_back(Length::pct(d / 3.6f));
                else if (parse_length(toks[j], l)) pos.push_back(l);
            }
            if (pos.empty()) g.stops.push_back(st);
            for (auto& p : pos) { GradStop s2 = st; s2.has_pos = true; s2.pos = p; g.stops.push_back(s2); }
        }
    }
    static bool parse_position(const std::vector<std::string>& toks, size_t from, Length& x, Length& y) {
        std::vector<std::string> t(toks.begin() + static_cast<long>(from), toks.end());
        if (t.empty()) return true;
        auto kw = [](const std::string& s, Length& out, bool& is_y) {
            is_y = false;
            if (s == "left") { out = Length::pct(0); return true; }
            if (s == "right") { out = Length::pct(100); return true; }
            if (s == "center") { out = Length::pct(50); return true; }
            if (s == "top") { out = Length::pct(0); is_y = true; return true; }
            if (s == "bottom") { out = Length::pct(100); is_y = true; return true; }
            return false;
        };
        Length a, b;
        bool   ay = false, by = false;
        if (!kw(t[0], a, ay) && !parse_length(t[0], a)) return false;
        if (t.size() == 1) {
            if (ay) { y = a; x = Length::pct(50); } else { x = a; y = Length::pct(50); }
            return true;
        }
        if (!kw(t[1], b, by) && !parse_length(t[1], b)) return false;
        if (ay && !by) { x = b; y = a; } else { x = a; y = b; }
        return true;
    }
    static bool parse_gradient(const std::string& fn_in, const std::string& args_s, Gradient& g) {
        std::string fn = fn_in;
        bool legacy = false;
        for (const char* pre : {"-webkit-", "-moz-", "-o-", "-ms-"})
            if (fn.compare(0, std::strlen(pre), pre) == 0) { fn = fn.substr(std::strlen(pre)); legacy = true; }
        if (fn.compare(0, 10, "repeating-") == 0) { g.repeating = true; fn = fn.substr(10); }
        auto args = split_commas(args_s); // (split_args also splits at spaces)
        if (args.empty()) return false;
        size_t first = 0;
        std::string a0 = lower(trim(args[0]));
        uint32_t c;
        bool     cur;
        auto     a0t = split_top_ws(a0);
        bool     a0_is_stop = !a0t.empty() && parse_css_color(a0t[0], c, cur);
        if (fn == "linear-gradient") {
            g.kind = 0;
            if (!a0_is_stop) {
                first = 1;
                float d;
                if (parse_angle(a0, d)) g.angle = legacy ? 90 - d : d;
                else {
                    auto toks = a0t;
                    bool to = !toks.empty() && toks[0] == "to";
                    if (to) toks.erase(toks.begin());
                    int sx = 0, sy = 0;
                    for (auto& tk : toks) {
                        if (tk == "left") sx = -1; else if (tk == "right") sx = 1;
                        else if (tk == "top") sy = -1; else if (tk == "bottom") sy = 1;
                    }
                    if (legacy && !to) { sx = -sx; sy = -sy; } // -webkit-linear-gradient(top, ...) starts at the top
                    if (sx && sy) { g.corner_x = static_cast<int8_t>(sx); g.corner_y = static_cast<int8_t>(sy); }
                    else if (sx) g.angle = sx > 0 ? 90.f : 270.f;
                    else if (sy) g.angle = sy > 0 ? 180.f : 0.f;
                }
            }
            parse_stops(args, first, g, false);
        } else if (fn == "radial-gradient") {
            g.kind = 1;
            if (!a0_is_stop) {
                first = 1;
                auto toks = a0t;
                size_t at = toks.size();
                for (size_t k = 0; k < toks.size(); k++) if (toks[k] == "at") { at = k; break; }
                std::vector<Length> lens;
                for (size_t k = 0; k < at; k++) {
                    const std::string& tk = toks[k];
                    if (tk == "circle") g.circle = true;
                    else if (tk == "ellipse") g.circle = false;
                    else if (tk == "closest-side" || tk == "contain") g.size_kw = 0;
                    else if (tk == "closest-corner") g.size_kw = 1;
                    else if (tk == "farthest-side") g.size_kw = 2;
                    else if (tk == "farthest-corner" || tk == "cover") g.size_kw = 3;
                    else { Length l; if (parse_length(tk, l)) lens.push_back(l); }
                }
                if (!lens.empty()) {
                    g.size_kw = 4;
                    g.rx = lens[0];
                    g.ry = lens.size() > 1 ? lens[1] : lens[0];
                    if (lens.size() == 1) g.circle = true;
                }
                if (at < toks.size()) parse_position(toks, at + 1, g.cx, g.cy);
                else if (legacy) parse_position(toks, 0, g.cx, g.cy);
            }
            parse_stops(args, first, g, false);
        } else if (fn == "conic-gradient") {
            g.kind = 2;
            g.angle = 0;
            if (!a0_is_stop) {
                first = 1;
                auto toks = a0t;
                for (size_t k = 0; k < toks.size(); k++) {
                    if (toks[k] == "from" && k + 1 < toks.size()) { parse_angle(toks[k + 1], g.angle); k++; }
                    else if (toks[k] == "at") { parse_position(toks, k + 1, g.cx, g.cy); break; }
                }
            }
            parse_stops(args, first, g, true);
        } else {
            return false;
        }
        return g.stops.size() >= 1;
    }
    // background-image / background: the gradients among its layers (url() layers are libcss's)
    void parse_bg_image(const std::string& v, bool shorthand, Extras& ex) {
        std::string t = trim(v), lt = lower(t);
        if (lt == "inherit" || lt == "initial" || lt == "unset") return;
        ex.has_gradients = true;
        ex.gradients.clear();
        ex.gradient_over_image = false;
        ex.gradient_hides_url = false;
        if (lt.find("gradient(") == std::string::npos) return; // no gradient layer: nothing (resets earlier ones)
        auto layers = split_commas(t);
        bool seen_url = false;
        for (size_t li = 0; li < layers.size(); li++) {
            std::string L = trim(layers[li]), ll = lower(L);
            size_t g = ll.find("gradient(");
            if (ll.find("url(") != std::string::npos) seen_url = true;
            if (g == std::string::npos) continue;
            size_t fs = g;
            while (fs > 0 && (std::isalnum(static_cast<unsigned char>(ll[fs - 1])) || ll[fs - 1] == '-')) --fs;
            size_t open = g + 8, depth = 0, close = std::string::npos;
            for (size_t k = open; k < L.size(); k++) {
                if (L[k] == '(') depth++;
                else if (L[k] == ')' && --depth == 0) { close = k; break; }
            }
            if (close == std::string::npos) continue;
            Gradient gr;
            if (parse_gradient(ll.substr(fs, g + 8 - fs), L.substr(open + 1, close - open - 1), gr)) {
                if (!seen_url && ex.gradients.empty()) ex.gradient_over_image = true;
                ex.gradients.push_back(std::move(gr));
            }
        }
        if (shorthand && !layers.empty()) { // the colour of a shorthand sits in its last layer
            std::string L = trim(layers.back()), ll = lower(L), rest;
            int depth = 0; // the layer without its function calls
            for (char ch : L) {
                if (ch == '(') depth++;
                if (depth == 0) rest += ch;
                if (ch == ')') { depth--; rest += ' '; }
            }
            for (auto& tk : split_top_ws(rest)) {
                uint32_t c;
                bool     cur;
                if (parse_css_color(tk, c, cur)) { ex.has_bg_color = true; ex.bg_color = c; ex.bg_color_current = cur; }
            }
            for (auto& tk : split_top_ws(L)) { // rgb(...) / hsl(...) colours are function calls themselves
                std::string lt2 = lower(tk);
                if (lt2.compare(0, 3, "rgb") == 0 || lt2.compare(0, 3, "hsl") == 0) {
                    uint32_t c;
                    bool     cur;
                    if (parse_css_color(tk, c, cur)) { ex.has_bg_color = true; ex.bg_color = c; ex.bg_color_current = false; }
                }
            }
        }
        // the declaration lists no url(): libcss dropped it, so an earlier background-image must not show through
        if (!seen_url && !ex.gradients.empty()) ex.gradient_hides_url = true;
    }

    void parse_radius(const std::string& v, int kind, Extras& ex) {
        std::string t = trim(v);
        if (lower(t) == "inherit" || lower(t) == "initial" || lower(t) == "unset") return;
        if (kind == K_RADIUS_ALL) {
            size_t slash = t.find('/');
            std::vector<std::string> hx = split_top_ws(t.substr(0, slash)), hy = slash == std::string::npos ? hx : split_top_ws(t.substr(slash + 1));
            auto expand = [](const std::vector<std::string>& in, std::vector<Length>& out) {
                std::vector<Length> l;
                for (auto& s : in) { Length x; if (!parse_length(s, x)) return false; l.push_back(x); }
                if (l.empty() || l.size() > 4) return false;
                if (l.size() == 1) out = {l[0], l[0], l[0], l[0]};
                else if (l.size() == 2) out = {l[0], l[1], l[0], l[1]};
                else if (l.size() == 3) out = {l[0], l[1], l[2], l[1]};
                else out = {l[0], l[1], l[2], l[3]};
                return true;
            };
            std::vector<Length> lx, ly;
            if (!expand(hx, lx) || !expand(hy, ly)) return;
            for (int k = 0; k < 4; ++k) { ex.radius[k].x = lx[static_cast<size_t>(k)]; ex.radius[k].y = ly[static_cast<size_t>(k)]; }
            ex.has_radius = true;
        } else {
            auto toks = split_top_ws(t);
            if (toks.empty() || toks.size() > 2) return;
            Length x, y;
            if (!parse_length(toks[0], x)) return;
            y = x;
            if (toks.size() == 2 && !parse_length(toks[1], y)) return;
            ex.radius[kind - K_TL] = {x, y};
            ex.has_radius = true;
        }
    }

    // "<w> [<h>]" | contain | cover | auto  (first comma item)
    static bool parse_size_spec(const std::string& text, SizeSpec& out) {
        std::string lt = lower(trim(text));
        auto cs = split_commas(lt);
        auto tk = split_top_ws(cs.empty() ? lt : trim(cs[0]));
        if (tk.empty()) return false;
        if (tk[0] == "contain") { out = SizeSpec{1, {}, {}}; return true; }
        if (tk[0] == "cover") { out = SizeSpec{2, {}, {}}; return true; }
        Length x, y;
        if (tk[0] != "auto" && !parse_length(tk[0], x)) return false;
        if (tk.size() >= 2 && tk[1] != "auto" && !parse_length(tk[1], y)) return false;
        out = (x.is_auto() && y.is_auto()) ? SizeSpec{} : SizeSpec{3, x, y};
        return true;
    }
    void parse_bg(const std::string& v, int kind, Extras& ex) {
        std::string t = trim(v);
        if (kind == K_BG_SIZE) {
            SizeSpec sp;
            if (parse_size_spec(t, sp)) { ex.bg_size = sp; ex.has_bgsize = true; }
            return;
        }
        // background shorthand: only the size after "/" matters here; a shorthand resets it
        ex.bg_size = SizeSpec{};
        ex.has_bgsize = true;
        int depth = 0;
        char q = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            char c = t[i];
            if (q) { if (c == '\\') ++i; else if (c == q) q = 0; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == '/' && depth == 0) {
                // size tokens: up to two words after the slash that look like sizes
                std::string after = t.substr(i + 1);
                size_t      comma = after.find(','); // the size ends with its layer
                if (comma != std::string::npos) after.erase(comma);
                auto tk = split_top_ws(after);
                std::string sz;
                for (size_t k = 0; k < tk.size() && k < 2; ++k) {
                    Length l;
                    std::string lw = lower(tk[k]);
                    if (lw == "auto" || lw == "contain" || lw == "cover" || parse_length(lw, l)) sz += (sz.empty() ? "" : " ") + lw; else break;
                }
                SizeSpec sp;
                if (!sz.empty() && parse_size_spec(sz, sp)) ex.bg_size = sp;
                break;
            }
        }
    }

    static bool mask_url(const std::string& v, std::string& out) {
        size_t u = v.find("url(");
        size_t end = 0;
        return u != std::string::npos && read_url_arg(v, u + 4, out, end);
    }
    static void mask_repeat_kw(const std::string& t, Mask& m) {
        if (t == "no-repeat") m.repeat = BgRepeat::NoRepeat;
        else if (t == "repeat") m.repeat = BgRepeat::Repeat;
        else if (t == "repeat-x") m.repeat = BgRepeat::RepeatX;
        else if (t == "repeat-y") m.repeat = BgRepeat::RepeatY;
    }
    void parse_mask(const std::string& v, int kind, Extras& ex) {
        std::string t = trim(v), lt = lower(t);
        if (lt == "inherit" || lt == "initial" || lt == "unset") return;
        Mask& m = ex.mask;
        if (kind == K_MASK_IMG || kind == K_MASK_ALL) {
            if (kind == K_MASK_ALL) m = Mask{};
            std::string url;
            if (lt == "none") { m.url.clear(); ex.has_mask = true; return; }
            if (mask_url(t, url)) { m.url = url; ex.has_mask = true; }
            else if (kind == K_MASK_IMG) return;
            if (kind == K_MASK_ALL) {
                std::string rest = lt; // the shorthand without its url(...), so "/" and keywords can be found
                for (size_t u = rest.find("url("); u != std::string::npos; u = rest.find("url(")) {
                    std::string tmp;
                    size_t end = 0;
                    if (!read_url_arg(rest, u + 4, tmp, end)) break;
                    rest.erase(u, end - u);
                }
                size_t slash = rest.find('/');
                for (auto& tok : split_top_ws(slash == std::string::npos ? rest : rest.substr(0, slash))) mask_repeat_kw(tok, m);
                if (slash != std::string::npos) {
                    auto sz = split_top_ws(rest.substr(slash + 1));
                    if (!sz.empty()) parse_mask(sz[0] == "contain" || sz[0] == "cover" ? sz[0] : (sz.size() > 1 && sz[1].find("repeat") == std::string::npos ? sz[0] + " " + sz[1] : sz[0]), K_MASK_SIZE, ex);
                }
            }
            return;
        }
        // single-layer view of the first comma item
        std::string first = trim(split_commas(lt).empty() ? lt : split_commas(lt)[0]);
        if (kind == K_MASK_REPEAT) { for (auto& tok : split_top_ws(first)) { mask_repeat_kw(tok, m); break; } ex.has_mask = true; return; }
        if (kind == K_MASK_SIZE) {
            auto tk = split_top_ws(first);
            if (tk.empty()) return;
            if (tk[0] == "contain") m.size_kind = 1;
            else if (tk[0] == "cover") m.size_kind = 2;
            else if (tk[0] == "auto" && (tk.size() == 1 || tk[1] == "auto")) m.size_kind = 0;
            else {
                Length x, y;
                if (tk[0] == "auto") x = Length{};
                else if (!parse_length(tk[0], x)) return;
                if (tk.size() < 2 || tk[1] == "auto") y = Length{};
                else if (!parse_length(tk[1], y)) return;
                m.size_kind = 3; m.w = x; m.h = y;
            }
            ex.has_mask = true;
            return;
        }
        if (kind == K_MASK_POS) {
            auto tk = split_top_ws(first);
            auto kw = [](const std::string& s, Length& out) {
                if (s == "left" || s == "top") { out = Length::pct(0); return true; }
                if (s == "center") { out = Length::pct(50); return true; }
                if (s == "right" || s == "bottom") { out = Length::pct(100); return true; }
                return parse_length(s, out);
            };
            Length x = Length::pct(50), y = Length::pct(50);
            if (tk.size() == 1) { if (tk[0] == "top" || tk[0] == "bottom") kw(tk[0], y); else kw(tk[0], x); }
            else if (tk.size() >= 2) {
                if (tk[0] == "top" || tk[0] == "bottom" || tk[1] == "left" || tk[1] == "right") { kw(tk[0], y); kw(tk[1], x); }
                else { kw(tk[0], x); kw(tk[1], y); }
            }
            m.pos_x = x; m.pos_y = y;
            ex.has_mask = true;
        }
    }

    void parse_shadow(const std::string& v, Extras& ex) {
        std::string t = trim(v);
        std::string lt = lower(t);
        if (lt == "inherit" || lt == "initial" || lt == "unset") return;
        ex.has_shadow = true;
        ex.shadows.clear();
        if (lt == "none") return;
        std::vector<BoxShadow> out;
        for (auto& part : split_commas(t)) {
            BoxShadow sh;
            std::vector<double> nums;
            bool ok = true, have_color = false;
            for (auto& tok : split_top_ws(part)) {
                if (lower(tok) == "inset") { sh.inset = true; continue; }
                Length l;
                if (parse_length(tok, l) && l.is_px()) { nums.push_back(l.v); continue; }
                uint32_t c; bool cur;
                if (parse_css_color(tok, c, cur)) { sh.color = c; sh.current = cur; have_color = true; continue; }
                ok = false;
            }
            if (!ok || nums.size() < 2 || nums.size() > 4) continue;
            sh.ox = static_cast<float>(nums[0]);
            sh.oy = static_cast<float>(nums[1]);
            if (nums.size() > 2) sh.blur = std::max(0.f, static_cast<float>(nums[2]));
            if (nums.size() > 3) sh.spread = static_cast<float>(nums[3]);
            if (!have_color) sh.current = true;
            out.push_back(sh);
        }
        ex.shadows = std::move(out);
    }

    void compute_node(size_t n, std::vector<int>& stamp, int stamp_id) {
        const ENode& nd = nodes[n];
        const Env* penv = nd.parent >= 0 ? node_env[static_cast<size_t>(nd.parent)] : nullptr;

        // candidate entries
        std::vector<int> cand;
        auto take = [&](const std::vector<int>& v) { for (int e : v) if (stamp[static_cast<size_t>(e)] != stamp_id) { stamp[static_cast<size_t>(e)] = stamp_id; cand.push_back(e); } };
        if (!ids[n].empty()) { auto it = by_id.find(ids[n]); if (it != by_id.end()) take(it->second); }
        for (auto& c : classes[n]) { auto it = by_class.find(c); if (it != by_class.end()) take(it->second); }
        { auto it = by_tag.find(nd.tag); if (it != by_tag.end()) take(it->second); }
        take(universal);

        struct Hit { Prio prio; const CDecl* d; };
        std::vector<Hit> customs, extra_hits, pseudo_hits[2];
        std::vector<const Entry*> deferred_hits;
        std::vector<std::pair<Prio, CDecl>> inline_decls; // owned copies for style=""
        for (int ei : cand) {
            const Entry& e = entries[static_cast<size_t>(ei)];
            if (!match(e.sel, static_cast<int>(n))) continue;
            if (e.deferred) deferred_hits.push_back(&e);
            if (!e.sel.pseudo_el.empty()) {
                int pi = e.sel.pseudo_el == "before" ? 0 : e.sel.pseudo_el == "after" ? 1 : -1;
                if (pi >= 0)
                    for (auto& d : e.decls)
                        if (d.kind != K_DEFERRED && d.kind != K_CUSTOM) pseudo_hits[pi].push_back({Prio{d.important ? 1 : 0, e.sel.spec(), e.order}, &d});
                continue;
            }
            for (auto& d : e.decls) {
                Prio p{d.important ? 1 : 0, e.sel.spec(), e.order};
                if (d.kind == K_CUSTOM) customs.push_back({p, &d});
                else if (d.kind != K_DEFERRED) extra_hits.push_back({p, &d});
            }
        }
        if (const std::string* st = attr(static_cast<int>(n), "style")) {
            if (st->find("--") != std::string::npos || st->find("mask") != std::string::npos || st->find("background") != std::string::npos || st->find("radius") != std::string::npos || st->find("shadow") != std::string::npos || st->find("grid") != std::string::npos || st->find("place-") != std::string::npos || st->find("justify-") != std::string::npos || st->find("align-") != std::string::npos || st->find("appearance") != std::string::npos || st->find("transform") != std::string::npos || st->find("rotate") != std::string::npos || st->find("scale") != std::string::npos || st->find("translate") != std::string::npos) {
                for (auto& d : parse_decls(*st, 0, st->size())) {
                    CDecl cd;
                    if (!kind_of_decl(d, cd) || cd.kind == K_DEFERRED) continue;
                    inline_decls.push_back({Prio{d.important ? 1 : 0, 1000000, 0}, cd});
                }
                for (auto& pd : inline_decls) {
                    if (pd.second.kind == K_CUSTOM) customs.push_back({pd.first, &pd.second});
                    else extra_hits.push_back({pd.first, &pd.second});
                }
            }
        }

        // custom properties: cascade, then resolve against this element's own environment
        const Env* env = penv;
        if (!customs.empty()) {
            std::sort(customs.begin(), customs.end(), [](const Hit& a, const Hit& b) { return a.prio < b.prio; });
            std::unordered_map<std::string, std::string> raw;
            for (auto& h : customs) raw[h.d->name] = h.d->value;
            auto owned = std::make_unique<Env>();
            owned->parent = penv;
            std::unordered_map<std::string, int> state; // 1 = resolving, 2 = done
            std::function<bool(const std::string&, std::string&)> look = [&](const std::string& name, std::string& out) -> bool {
                auto r = raw.find(name);
                if (r == raw.end()) return lookup_env(penv, name, out);
                int& st2 = state[name];
                if (st2 == 1) return false; // cycle
                if (st2 == 2) {
                    const std::string& v = owned->vars[name];
                    if (v == kInvalid) return false;
                    out = v;
                    return true;
                }
                st2 = 1;
                std::string res;
                std::string v = trim(r->second);
                bool ok = v.find("var(") == std::string::npos ? (res = v, true) : subst(v, look, res, 1);
                state[name] = 2;
                owned->vars[name] = ok ? res : std::string(kInvalid);
                if (!ok) return false;
                out = res;
                return true;
            };
            for (auto& kv : raw) { std::string tmp; look(kv.first, tmp); }
            env = owned.get();
            envs.push_back(std::move(owned));
        }
        node_env[n] = env;

        // extras
        auto build = [&](std::vector<Hit>& hits, Extras& ex) {
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.prio < b.prio; });
            std::vector<std::pair<std::string, std::string>> svgp; // later declarations win
            for (auto& h : hits) {
                std::string v = resolve_value(h.d->value, env);
                if (v == kInvalid) continue;
                if (h.d->kind == K_SVGPROP) {
                    std::string val = trim(v);
                    if (val.empty() || lower(val) == "inherit" || lower(val) == "initial" || lower(val) == "unset") continue;
                    bool found = false;
                    for (auto& kv : svgp) if (kv.first == h.d->name) { kv.second = val; found = true; }
                    if (!found) svgp.push_back({h.d->name, val});
                    continue;
                }
                if (h.d->kind == K_GRID) {
                    std::string val = trim(v);
                    if (!val.empty()) ex.grid.push_back({h.d->name, val});
                    continue;
                }
                if (h.d->kind == K_APPEARANCE) {
                    ex.has_appearance  = true;
                    ex.appearance_none = lower(trim(v)) == "none";
                    continue;
                }
                if (h.d->kind == K_TRANSFORM) parse_transform(v, ex);
                else if (h.d->kind == K_TRANSFORM_ORIGIN) parse_origin(v, ex);
                else if (h.d->kind == K_BG_IMAGE) parse_bg_image(v, false, ex);
                else if (h.d->kind == K_ROTATE) {
                    float d;
                    std::string lv = lower(trim(v));
                    if (lv == "none") { ex.has_rotate = true; ex.rotate_deg = 0; }
                    else if (parse_angle(lv, d)) { ex.has_rotate = true; ex.rotate_deg = d; }
                } else if (h.d->kind == K_SCALE) {
                    auto tk = split_top_ws(lower(trim(v)));
                    if (!tk.empty() && tk[0] == "none") { ex.has_scale = true; ex.scale_x = ex.scale_y = 1; }
                    else if (!tk.empty()) {
                        auto rd = [](const std::string& s2, float& o) { char* e = nullptr; float f = std::strtof(s2.c_str(), &e); if (e == s2.c_str()) return false; if (*e == '%') f /= 100; o = f; return true; };
                        float a = 1, b = 1;
                        if (rd(tk[0], a)) { b = a; if (tk.size() > 1) rd(tk[1], b); ex.has_scale = true; ex.scale_x = a; ex.scale_y = b; }
                    }
                } else if (h.d->kind == K_TRANSLATE) {
                    parse_transform("translate(" + v + ")", ex);
                }
                else if (h.d->kind == K_SHADOW) parse_shadow(v, ex);
                else if (h.d->kind >= K_MASK_IMG && h.d->kind <= K_MASK_POS) parse_mask(v, h.d->kind, ex);
                else if (h.d->kind == K_BG_SIZE || h.d->kind == K_BG_ALL) {
                    parse_bg(v, h.d->kind, ex);
                    if (h.d->kind == K_BG_ALL) parse_bg_image(v, true, ex);
                }
                else parse_radius(v, h.d->kind, ex);
            }
            for (auto& kv : svgp) ex.svg_style += kv.first + ":" + kv.second + ";";
            return ex.has_appearance || !ex.svg_style.empty() || !ex.grid.empty() || ex.has_radius || ex.has_shadow || ex.has_transform || (ex.has_mask && !ex.mask.url.empty()) || (ex.has_bgsize && ex.bg_size.kind != 0) || !ex.gradients.empty() || ex.has_origin || ex.has_rotate || ex.has_scale;
        };
        if (!extra_hits.empty()) {
            Extras ex;
            if (build(extra_hits, ex)) { extras[n] = std::move(ex); has_extras[n] = 1; }
        }
        for (int k = 0; k < 2; ++k)
            if (!pseudo_hits[k].empty()) {
                Extras ex;
                if (build(pseudo_hits[k], ex)) { pextras[k][n] = std::move(ex); has_pextras[k][n] = 1; }
            }

        // deferred declarations (use scoped variables): resolve for this element, key a generated rule on a marker
        for (const Entry* e : deferred_hits) {
            for (int imp = 0; imp < 2; ++imp) {
                std::string text;
                for (auto& d : e->decls) {
                    if (d.kind != K_DEFERRED || d.important != (imp == 1)) continue;
                    std::string v = resolve_value(d.value, env);
                    if (v == kInvalid) continue;
                    text += d.name + ":" + v + (imp ? " !important;" : ";");
                }
                if (text.empty()) continue;
                int k = std::clamp(e->sel.a * 10 + e->sel.b, 1, 30);
                std::string key = e->sel.pseudo_el + "|" + std::to_string(k) + "|" + text;
                auto it = variant_ids.find(key);
                int id;
                if (it == variant_ids.end()) {
                    id = static_cast<int>(variants.size());
                    variants.push_back({id, k, e->sel.pseudo_el, text, e->order});
                    variant_ids[key] = id;
                } else {
                    id = it->second;
                }
                auto& mk = marks[n];
                if (std::find(mk.begin(), mk.end(), id) == mk.end()) mk.push_back(id);
            }
        }
    }
};

ElementCascade::ElementCascade(const CssVars& vars, std::vector<ENode> nodes) : d_(std::make_unique<Impl>(vars, std::move(nodes))) {}
ElementCascade::~ElementCascade() = default;

void ElementCascade::add_sheet(const std::string& css, const std::string& base) {
    d_->cur_base = base;
    std::string s = strip_comments(css);
    d_->rules_in(s, 0, s.size());
}

bool ElementCascade::state_dependent_rules() const { return d_->state_entries; }

std::vector<int> ElementCascade::recompute(const std::vector<int>& nodes) {
    Impl& d = *d_;
    std::vector<char> in(d.nodes.size(), 0);
    std::function<void(int)> add = [&](int n) {
        if (in[static_cast<size_t>(n)]) return;
        in[static_cast<size_t>(n)] = 1;
        for (int c : d.kids[static_cast<size_t>(n)]) add(c);
    };
    for (int n : nodes) add(n);
    std::vector<int> all;
    for (size_t i = 0; i < in.size(); ++i) if (in[i]) all.push_back(static_cast<int>(i));
    std::vector<int> stamp(d.entries.size(), 0);
    for (int n : all) {
        size_t u = static_cast<size_t>(n);
        d.marks[u].clear();
        d.extras[u] = Extras{};
        d.has_extras[u] = 0;
        for (int k = 0; k < 2; ++k) { d.pextras[k][u] = Extras{}; d.has_pextras[k][u] = 0; }
        d.compute_node(u, stamp, n + 1);
    }
    return all;
}

int ElementCascade::set_state_flags(const std::vector<unsigned char>& next, std::vector<int>& affected) {
    Impl& d = *d_;
    const size_t N = d.nodes.size();
    affected.clear();
    auto cur = [&](size_t i) {
        const ENode& n = d.nodes[i];
        return static_cast<unsigned char>((n.hover ? 1 : 0) | (n.active ? 2 : 0) | (n.focus ? 4 : 0) | (n.focus_within ? 8 : 0));
    };
    // Only elements whose own state flipped can change what a selector matches, and only through a compound that tests that
    // state: so look at those elements and ask whether such a compound could match them (ignoring the state itself).
    std::vector<std::pair<size_t, unsigned char>> changed; // node, flipped bits
    for (size_t i = 0; i < N; ++i) {
        unsigned char f = i < next.size() ? next[i] : 0, was = cur(i);
        if (f != was) changed.push_back({i, static_cast<unsigned char>(f ^ was)});
    }
    std::vector<const Complex*> triggered;
    if (!changed.empty() && !d.hover_sels.empty()) {
        d.ignore_state = true;
        for (auto& c : d.hover_sels) {
            bool hit = false;
            for (size_t pi = 0; pi < c.parts.size() && !hit; ++pi) {
                int bits = state_bits(c.parts[pi]);
                if (!bits) continue;
                for (auto& ch : changed)
                    if ((ch.second & bits) && d.compound_ok(c.parts[pi], static_cast<int>(ch.first))) { hit = true; break; }
            }
            if (hit) triggered.push_back(&c);
        }
        d.ignore_state = false;
    }
    if (std::getenv("QUILL_PROF"))
        for (const Complex* c : triggered)
            if (c->entry) {
                std::string t;
                for (auto& pt : c->parts) { t += pt.tag; for (auto& cl : pt.classes) t += "." + cl; for (auto& pp : pt.pseudos) t += ":" + pp.name; t += " "; }
                std::fprintf(stderr, "state entry rule: %s\n", t.c_str());
            }
    std::vector<char> before;
    for (const Complex* c : triggered)
        for (size_t n = 0; n < N; ++n) before.push_back(d.match(*c, static_cast<int>(n)) ? 1 : 0);
    for (auto& ch : changed) {
        unsigned char f = next[ch.first];
        ENode& n = d.nodes[ch.first];
        n.hover = f & 1; n.active = f & 2; n.focus = f & 4; n.focus_within = f & 8;
    }
    int  result = 0;
    size_t k = 0;
    std::vector<char> mark(N, 0);
    for (const Complex* c : triggered)
        for (size_t n = 0; n < N; ++n, ++k)
            if ((d.match(*c, static_cast<int>(n)) ? 1 : 0) != before[k]) {
                result |= c->entry ? 3 : 1;
                mark[n] = 1;
            }
    for (size_t n = 0; n < N; ++n) if (mark[n]) affected.push_back(static_cast<int>(n));
    return result;
}

void ElementCascade::run() {
    Impl& d = *d_;
    std::vector<int> stamp(d.entries.size(), 0);
    for (size_t n = 0; n < d.nodes.size(); ++n) d.compute_node(n, stamp, static_cast<int>(n) + 1);
    rules = d.entries.size();
    variants = d.variants.size();
    for (auto& m : d.marks) deferred_applied += m.size();
}

std::string ElementCascade::variants_css() const {
    std::vector<const Impl::Variant*> vs;
    for (auto& v : d_->variants) vs.push_back(&v);
    std::stable_sort(vs.begin(), vs.end(), [](const Impl::Variant* a, const Impl::Variant* b) { return a->order < b->order; });
    std::string out;
    for (auto* v : vs) {
        std::string sel;
        for (int i = 0; i < v->k; ++i) sel += ".qvx" + std::to_string(v->id);
        out += sel + (v->pseudo.empty() ? "" : "::" + v->pseudo) + "{" + v->text + "}\n";
    }
    return out;
}

const std::vector<int>& ElementCascade::marks(size_t n) const { return d_->marks[n]; }
const Extras* ElementCascade::extras(size_t n, int pseudo) const {
    if (pseudo == 0) return d_->has_extras[n] ? &d_->extras[n] : nullptr;
    int k = pseudo == 1 ? 0 : 1;
    return d_->has_pextras[k][n] ? &d_->pextras[k][n] : nullptr;
}

} // namespace quill::engine
