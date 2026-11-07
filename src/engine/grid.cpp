#include "grid.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace quill::engine {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Splits on whitespace at the top level (outside () and []); quoted strings stay one token.
std::vector<std::string> split_ws(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char q = 0;
    for (char c : v) {
        if (q) { cur += c; if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; cur += c; continue; }
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
        if (depth == 0 && c == ']') { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Splits on a separator char at the top level.
std::vector<std::string> split_top(const std::string& v, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char q = 0;
    for (char c : v) {
        if (q) { cur += c; if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; cur += c; continue; }
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (depth == 0 && c == sep) { out.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

bool parse_number(const std::string& s, size_t& end, float& v) {
    const char* b = s.c_str();
    char* e = nullptr;
    v = std::strtof(b, &e);
    if (e == b) return false;
    end = static_cast<size_t>(e - b);
    return true;
}

// <length> | <percentage> | <flex> | auto | min-content | max-content ; false if unusable
bool parse_breadth(const std::string& in, GBreadth& out, bool allow_fr = true) {
    std::string s = lower(trim(in));
    if (s == "auto") { out = {0, 0}; return true; }
    if (s == "min-content") { out = {4, 0}; return true; }
    if (s == "max-content") { out = {5, 0}; return true; }
    size_t e = 0;
    float v = 0;
    if (!parse_number(s, e, v)) return false;
    std::string u = s.substr(e);
    if (u == "fr") { if (!allow_fr) return false; out = {3, v}; return true; }
    if (u == "%") { out = {2, v}; return true; }
    if (u.empty()) { if (v != 0) return false; out = {1, 0}; return true; }
    if (u == "px") { out = {1, v}; return true; }
    if (u == "em" || u == "rem") { out = {1, v * 16.f}; return true; }
    if (u == "pt") { out = {1, v * 4.f / 3.f}; return true; }
    if (u == "ch" || u == "ex") { out = {1, v * 8.f}; return true; }
    if (u == "cm") { out = {1, v * 37.795f}; return true; }
    if (u == "mm") { out = {1, v * 3.7795f}; return true; }
    if (u == "in") { out = {1, v * 96.f}; return true; }
    return false; // vw, vh, calc(): treated as auto by the caller
}

GTrack parse_track(const std::string& tok) {
    GTrack t;
    std::string s = lower(trim(tok));
    if (s.rfind("minmax(", 0) == 0 && s.back() == ')') {
        auto parts = split_top(s.substr(7, s.size() - 8), ',');
        if (parts.size() == 2) {
            GBreadth a, b;
            if (!parse_breadth(parts[0], a, false)) a = {0, 0};
            if (!parse_breadth(parts[1], b)) b = {0, 0};
            t.min = a;
            t.max = b;
            return t;
        }
        return t;
    }
    if (s.rfind("fit-content(", 0) == 0 && s.back() == ')') {
        GBreadth l;
        t.min = {0, 0};
        t.max = {5, 0};
        if (parse_breadth(s.substr(12, s.size() - 13), l) && l.k == 1) t.fit = l.v;
        return t;
    }
    GBreadth b;
    if (!parse_breadth(s, b)) return t;
    if (b.k == 3) { t.min = {0, 0}; t.max = b; }
    else { t.min = b; t.max = b; }
    if (b.k == 0) { t.min = {0, 0}; t.max = {0, 0}; }
    return t;
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

} // namespace

bool parse_grid_track_list(const std::string& value, GTrackList& out) {
    out = GTrackList();
    std::string v = trim(value);
    if (v.empty()) return false;
    std::string lv = lower(v);
    if (lv == "none" || starts(lv, "subgrid") || lv == "masonry") return true;
    // pre-expand repeat(N, ...) so line names merge naturally; keep auto-repeat separate
    std::vector<std::string> toks;
    std::function<bool(const std::string&, int)> expand = [&](const std::string& in, int depth) -> bool {
        if (depth > 4) return false;
        for (auto& t : split_ws(in)) {
            std::string lt = lower(t);
            if (starts(lt, "repeat(") && lt.back() == ')') {
                auto parts = split_top(t.substr(7, t.size() - 8), ',');
                if (parts.size() < 2) return false;
                std::string body = t.substr(7 + parts[0].size() + 1, t.size() - 8 - parts[0].size() - 1);
                std::string cnt = lower(parts[0]);
                if (cnt == "auto-fill" || cnt == "auto-fit") {
                    toks.push_back("\x01" + cnt + "\x02" + body);
                    continue;
                }
                int n = std::atoi(cnt.c_str());
                if (n < 1) return false;
                if (n > 256) n = 256;
                for (int i = 0; i < n; ++i)
                    if (!expand(body, depth + 1)) return false;
                continue;
            }
            toks.push_back(t);
            if (toks.size() > 4000) return false;
        }
        return true;
    };
    if (!expand(v, 0)) return false;

    std::vector<std::string> pending;
    bool names_ok = true;
    for (auto& t : toks) {
        if (t[0] == '\x01') {
            size_t sep = t.find('\x02');
            out.auto_repeat = true;
            out.auto_fit = t.compare(1, 8, "auto-fit") == 0;
            out.auto_at = out.tracks.size();
            for (auto& bt : split_ws(t.substr(sep + 1))) {
                if (bt[0] == '[') continue; // names inside an auto-repeat are ignored
                out.rep.push_back(parse_track(bt));
            }
            if (out.rep.empty()) return false;
            names_ok = false;
            continue;
        }
        if (t[0] == '[') {
            std::string inner = t.substr(1, t.size() - 2);
            for (auto& n : split_ws(inner)) pending.push_back(n);
            continue;
        }
        out.names.push_back(pending);
        pending.clear();
        out.tracks.push_back(parse_track(t));
    }
    out.names.push_back(pending);
    if (!names_ok) out.names.clear();
    return true;
}

namespace {

uint8_t parse_align(const std::string& in, bool content) {
    std::string s = lower(trim(in));
    // "safe center" / "unsafe end": drop the overflow keyword; "first baseline" etc.
    for (const char* kw : {"safe ", "unsafe ", "first ", "last "})
        if (starts(s, kw)) s = trim(s.substr(std::strlen(kw)));
    if (s == "normal" || s == "auto" || s.empty()) return 0;
    if (s == "start" || s == "flex-start" || s == "self-start" || s == "left") return 1;
    if (s == "end" || s == "flex-end" || s == "self-end" || s == "right") return 2;
    if (s == "center") return 3;
    if (s == "stretch") return 4;
    if (s == "baseline") return content ? 1 : 5;
    if (content) {
        if (s == "space-between") return 5;
        if (s == "space-around") return 6;
        if (s == "space-evenly") return 7;
    }
    return 0;
}

bool is_int(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// <grid-line> = auto | <ident> | [<integer> && <ident>?] | [span && [<integer> || <ident>]]
GLine parse_line(const std::string& in) {
    GLine l;
    auto toks = split_ws(trim(in));
    if (toks.empty()) return l;
    if (lower(toks[0]) == "auto") return l;
    bool span = false;
    int n = 0;
    std::string name;
    for (auto& t : toks) {
        if (lower(t) == "span") span = true;
        else if (is_int(t)) n = std::atoi(t.c_str());
        else name = t;
    }
    if (span) {
        l.k = 2;
        l.n = n > 0 ? n : 1;
        l.name = name;
    } else if (!name.empty() && n == 0) {
        l.k = 3;
        l.n = 1;
        l.name = name;
    } else if (n != 0) {
        l.k = name.empty() ? 1 : 3;
        l.n = n;
        l.name = name;
    }
    return l;
}

void place_pair(const std::string& v, GLine& s, GLine& e) {
    auto parts = split_top(v, '/');
    s = parse_line(parts[0]);
    if (parts.size() > 1) e = parse_line(parts[1]);
    else {
        e = GLine();
        if (s.k == 3 && s.n == 1) e = s; // a lone ident names both lines
    }
}

bool parse_areas(const std::string& v, std::vector<std::vector<std::string>>& out) {
    out.clear();
    size_t i = 0;
    while (i < v.size()) {
        if (v[i] == '"' || v[i] == '\'') {
            char q = v[i];
            size_t e = v.find(q, i + 1);
            if (e == std::string::npos) return false;
            auto cells = split_ws(v.substr(i + 1, e - i - 1));
            for (auto& c : cells) if (c.find_first_not_of('.') == std::string::npos) c = ".";
            if (!out.empty() && out[0].size() != cells.size()) { out.clear(); return false; }
            out.push_back(cells);
            i = e + 1;
        } else ++i;
    }
    return !out.empty();
}

} // namespace

void apply_grid_props(Style& st, const std::vector<std::pair<std::string, std::string>>& props) {
    auto gc = st.grid ? std::make_shared<GridContainer>(*st.grid) : std::make_shared<GridContainer>();
    auto gi = st.gitem ? std::make_shared<GridItem>(*st.gitem) : std::make_shared<GridItem>();
    bool cont = false, item = false;
    for (auto& pv : props) {
        const std::string& n = pv.first;
        const std::string  v = pv.second;
        std::string lv = lower(v);
        if (lv == "inherit" || lv == "initial" || lv == "unset" || lv == "revert") {
            continue;
        }
        if (n == "grid-template-columns") { GTrackList l; if (parse_grid_track_list(v, l)) { gc->cols = l; cont = true; } }
        else if (n == "grid-template-rows") { GTrackList l; if (parse_grid_track_list(v, l)) { gc->rows = l; cont = true; } }
        else if (n == "grid-template-areas") {
            if (lv == "none") { gc->areas.clear(); cont = true; }
            else if (parse_areas(v, gc->areas)) cont = true;
        } else if (n == "grid-template" || n == "grid") {
            if (lv.find("auto-flow") != std::string::npos) continue; // grid: auto-flow ... shorthand: not supported
            auto halves = split_top(v, '/');
            std::string rows_part = halves[0];
            if (rows_part.find('"') != std::string::npos || rows_part.find('\'') != std::string::npos) {
                // "a b" 1fr "c d" auto   (rows interleaved with area strings)
                GTrackList rl;
                std::vector<std::vector<std::string>> areas;
                std::string area_text;
                size_t i = 0;
                std::string rest;
                while (i < rows_part.size()) {
                    char c = rows_part[i];
                    if (c == '"' || c == '\'') {
                        size_t e = rows_part.find(c, i + 1);
                        if (e == std::string::npos) break;
                        area_text += rows_part.substr(i, e - i + 1) + " ";
                        // optional size after the string
                        size_t j = e + 1;
                        while (j < rows_part.size() && std::isspace(static_cast<unsigned char>(rows_part[j]))) ++j;
                        size_t k = j;
                        int depth = 0;
                        while (k < rows_part.size() && (depth > 0 || (rows_part[k] != '"' && rows_part[k] != '\''))) {
                            if (rows_part[k] == '(') ++depth;
                            if (rows_part[k] == ')') --depth;
                            ++k;
                        }
                        std::string sz = trim(rows_part.substr(j, k - j));
                        rl.names.push_back({});
                        rl.tracks.push_back(sz.empty() ? GTrack() : parse_track(sz));
                        i = k;
                    } else ++i;
                }
                rl.names.push_back({});
                if (parse_areas(area_text, areas)) { gc->areas = areas; gc->rows = rl; cont = true; }
            } else if (!rows_part.empty()) {
                GTrackList l;
                if (parse_grid_track_list(rows_part, l)) { gc->rows = l; cont = true; }
            }
            if (halves.size() > 1) {
                GTrackList l;
                if (parse_grid_track_list(halves[1], l)) { gc->cols = l; cont = true; }
            }
        } else if (n == "grid-auto-columns" || n == "grid-auto-rows") {
            std::vector<GTrack> ts;
            for (auto& t : split_ws(v)) ts.push_back(parse_track(t));
            if (n == "grid-auto-columns") gc->auto_cols = ts; else gc->auto_rows = ts;
            cont = true;
        } else if (n == "grid-auto-flow") {
            gc->flow_col = lv.find("column") != std::string::npos;
            gc->dense = lv.find("dense") != std::string::npos;
            cont = true;
        } else if (n == "grid-column") { place_pair(v, gi->cs, gi->ce); item = true; }
        else if (n == "grid-row") { place_pair(v, gi->rs, gi->re); item = true; }
        else if (n == "grid-column-start") { gi->cs = parse_line(v); item = true; }
        else if (n == "grid-column-end") { gi->ce = parse_line(v); item = true; }
        else if (n == "grid-row-start") { gi->rs = parse_line(v); item = true; }
        else if (n == "grid-row-end") { gi->re = parse_line(v); item = true; }
        else if (n == "grid-area") {
            auto parts = split_top(v, '/');
            GLine l[4];
            for (size_t i = 0; i < parts.size() && i < 4; ++i) l[i] = parse_line(parts[i]);
            if (parts.size() == 1 && l[0].k == 3) {
                gi->area = l[0].name;
                gi->rs = gi->re = gi->cs = gi->ce = GLine();
            } else {
                gi->area.clear();
                gi->rs = l[0];
                gi->cs = parts.size() > 1 ? l[1] : (l[0].k == 3 ? l[0] : GLine());
                gi->re = parts.size() > 2 ? l[2] : (l[0].k == 3 ? l[0] : GLine());
                gi->ce = parts.size() > 3 ? l[3] : (gi->cs.k == 3 ? gi->cs : GLine());
            }
            item = true;
        } else if (n == "justify-items") { gc->justify_items = parse_align(v, false); cont = true; }
        else if (n == "align-items") { gc->align_items = parse_align(v, false); cont = true; }
        else if (n == "justify-self") { gi->justify_self = parse_align(v, false); item = true; }
        else if (n == "align-self") { gi->align_self = parse_align(v, false); item = true; }
        else if (n == "justify-content") { gc->justify_content = parse_align(v, true); cont = true; }
        else if (n == "align-content") { gc->align_content = parse_align(v, true); cont = true; }
        else if (n == "place-items") {
            auto t = split_ws(v);
            gc->align_items = parse_align(t[0], false);
            gc->justify_items = t.size() > 1 ? parse_align(t[1], false) : gc->align_items;
            cont = true;
        } else if (n == "place-self") {
            auto t = split_ws(v);
            gi->align_self = parse_align(t[0], false);
            gi->justify_self = t.size() > 1 ? parse_align(t[1], false) : gi->align_self;
            item = true;
        } else if (n == "place-content") {
            auto t = split_ws(v);
            gc->align_content = parse_align(t[0], true);
            gc->justify_content = t.size() > 1 ? parse_align(t[1], true) : gc->align_content;
            cont = true;
        }
    }
    if (cont) st.grid = gc;
    if (item) st.gitem = gi;
}

} // namespace quill::engine
