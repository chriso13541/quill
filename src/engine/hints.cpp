// HTML presentational hints. See hints.h. The mapping follows the HTML standard's rendering section
// (15.3 "Non-replaced elements" and 15.4 "Replaced elements"), plus the table quirk browsers apply to
// pages without a modern doctype.
#include "hints.h"

#include "elemcss.h" // parse_css_color (named colours)

#include <algorithm>
#include <cstdlib>

namespace quill::engine {

using namespace domutil;

namespace {

using Attrs = std::vector<std::pair<std::string, std::string>>;

const std::string* find(const Attrs& a, const char* name) {
    for (auto& kv : a)
        if (kv.first == name) return &kv.second;
    return nullptr;
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string trim_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// "rules for parsing non-negative integers" (a leading '+' is allowed, trailing junk ignored)
bool parse_uint(const std::string& s, int& out) {
    size_t i = 0;
    while (i < s.size() && is_ws(s[i])) ++i;
    if (i < s.size() && s[i] == '+') ++i;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        if (v > 100000) v = 100000;
        ++i;
    }
    out = static_cast<int>(v);
    return true;
}

// Collects hints for one element.
struct Out {
    std::vector<css_hint>& v;

    css_hint& add(uint32_t prop, uint8_t status) {
        css_hint h{};
        h.prop   = prop;
        h.status = status;
        v.push_back(h);
        return v.back();
    }
    void color(uint32_t prop, uint8_t status, uint32_t argb) { add(prop, status).data.color = argb; }
    void length(uint32_t prop, uint8_t status, float value, css_unit unit) {
        css_hint& h          = add(prop, status);
        h.data.length.value  = FLTTOFIX(value);
        h.data.length.unit   = unit;
    }
    void px(uint32_t prop, uint8_t status, float value) { length(prop, status, value, CSS_UNIT_PX); }
    void keyword(uint32_t prop, uint8_t status) { add(prop, status); }

    void text_color(uint32_t argb) { color(CSS_PROP_COLOR, CSS_COLOR_COLOR, argb); }
    void bg_color(uint32_t argb) { color(CSS_PROP_BACKGROUND_COLOR, CSS_BACKGROUND_COLOR_COLOR, argb); }
    void bg_image(const std::string& url) {
        std::string u = trim_ws(url);
        if (u.empty()) return;
        lwc_string* s = intern(u);
        if (!s) return;
        add(CSS_PROP_BACKGROUND_IMAGE, CSS_BACKGROUND_IMAGE_IMAGE).data.string = s;
    }
    void width(float v, bool pct) { length(CSS_PROP_WIDTH, CSS_WIDTH_SET, v, pct ? CSS_UNIT_PCT : CSS_UNIT_PX); }
    void height(float v, bool pct) { length(CSS_PROP_HEIGHT, CSS_HEIGHT_SET, v, pct ? CSS_UNIT_PCT : CSS_UNIT_PX); }
    // sides: bit 0 top, 1 right, 2 bottom, 3 left
    void margin(int sides, float v) {
        for (int i = 0; i < 4; ++i)
            if (sides & (1 << i)) px(CSS_PROP_MARGIN_TOP + i, CSS_MARGIN_SET, v);
    }
    void margin_auto(int sides) {
        for (int i = 0; i < 4; ++i)
            if (sides & (1 << i)) keyword(CSS_PROP_MARGIN_TOP + i, CSS_MARGIN_AUTO);
    }
    void padding(float v) {
        for (int i = 0; i < 4; ++i) px(CSS_PROP_PADDING_TOP + i, CSS_PADDING_SET, v);
    }
    void border_width(int sides, float v) {
        for (int i = 0; i < 4; ++i)
            if (sides & (1 << i)) px(CSS_PROP_BORDER_TOP_WIDTH + i, CSS_BORDER_WIDTH_WIDTH, v);
    }
    void border_style(uint8_t style) {
        for (int i = 0; i < 4; ++i) keyword(CSS_PROP_BORDER_TOP_STYLE + i, style);
    }
    void border_color(uint32_t argb) {
        for (int i = 0; i < 4; ++i) color(CSS_PROP_BORDER_TOP_COLOR + i, CSS_BORDER_COLOR_COLOR, argb);
    }
};

bool attr_color(const Attrs& a, const char* name, uint32_t& c) {
    const std::string* v = find(a, name);
    return v && parse_legacy_color(*v, c);
}

void bgcolor_and_background(const Attrs& a, Out& o) {
    uint32_t c;
    if (attr_color(a, "bgcolor", c)) o.bg_color(c);
    if (const std::string* bg = find(a, "background")) o.bg_image(*bg);
}

// width / height mapped "to the dimension property" (nonzero: zero is ignored, as for table cells)
void dimension(const Attrs& a, const char* name, bool nonzero, Out& o) {
    const std::string* v = find(a, name);
    float f;
    bool  pct;
    if (!v || !parse_html_dimension(*v, f, pct)) return;
    if (nonzero && f <= 0) return;
    if (name[0] == 'w') o.width(f, pct);
    else o.height(f, pct);
}

// align on div / caption / table sections / rows / cells: "center" also centres child blocks (-libcss-center)
void block_align(const Attrs& a, Out& o, bool magic) {
    const std::string* v = find(a, "align");
    if (!v) return;
    std::string s = lower(trim_ws(*v));
    if (s == "left") o.keyword(CSS_PROP_TEXT_ALIGN, magic ? CSS_TEXT_ALIGN_LIBCSS_LEFT : CSS_TEXT_ALIGN_LEFT);
    else if (s == "right") o.keyword(CSS_PROP_TEXT_ALIGN, magic ? CSS_TEXT_ALIGN_LIBCSS_RIGHT : CSS_TEXT_ALIGN_RIGHT);
    else if (s == "center" || s == "middle") o.keyword(CSS_PROP_TEXT_ALIGN, magic ? CSS_TEXT_ALIGN_LIBCSS_CENTER : CSS_TEXT_ALIGN_CENTER);
    else if (s == "justify") o.keyword(CSS_PROP_TEXT_ALIGN, CSS_TEXT_ALIGN_JUSTIFY);
}

void valign(const Attrs& a, Out& o) {
    const std::string* v = find(a, "valign");
    if (!v) return;
    std::string s = lower(trim_ws(*v));
    if (s == "top") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_TOP);
    else if (s == "middle" || s == "center") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_MIDDLE);
    else if (s == "bottom") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_BOTTOM);
    else if (s == "baseline") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_BASELINE);
}

// <img align>, <object align>, <input type=image align>, ...
void replaced_align(const Attrs& a, Out& o) {
    const std::string* v = find(a, "align");
    if (!v) return;
    std::string s = lower(trim_ws(*v));
    if (s == "left") o.keyword(CSS_PROP_FLOAT, CSS_FLOAT_LEFT);
    else if (s == "right") o.keyword(CSS_PROP_FLOAT, CSS_FLOAT_RIGHT);
    else if (s == "top") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_TOP);
    else if (s == "middle" || s == "absmiddle" || s == "abscenter" || s == "center")
        o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_MIDDLE);
    else if (s == "bottom" || s == "baseline") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_BASELINE);
    else if (s == "texttop") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_TEXT_TOP);
    else if (s == "absbottom") o.keyword(CSS_PROP_VERTICAL_ALIGN, CSS_VERTICAL_ALIGN_BOTTOM);
}

void replaced_common(const Attrs& a, Out& o) {
    dimension(a, "width", false, o);
    dimension(a, "height", false, o);
    int n;
    if (const std::string* v = find(a, "hspace"); v && parse_uint(*v, n)) o.margin(2 | 8, static_cast<float>(n));
    if (const std::string* v = find(a, "vspace"); v && parse_uint(*v, n)) o.margin(1 | 4, static_cast<float>(n));
    replaced_align(a, o);
}

dom_node* ancestor(dom_node* n, const char* tag, const char* stop = nullptr) {
    for (dom_node* p = parent_element(n); p; p = parent_element(p)) {
        std::string t = tag_of(p);
        if (t == tag) return p;
        if (stop && t == stop) return nullptr;
    }
    return nullptr;
}

// Effective border of a <table border=...>: -1 = no border attribute. Present but unparsable (border="") = 1.
int table_border(const Attrs& a) {
    const std::string* v = find(a, "border");
    if (!v) return -1;
    int n;
    return parse_uint(*v, n) ? n : 1;
}

const uint32_t kGray = 0xff808080;

void font_face(const std::string& face, Out& o) {
    std::vector<std::string> names;
    uint8_t                  generic = CSS_FONT_FAMILY_SANS_SERIF;
    bool                     have_generic = false;
    size_t                   pos = 0;
    while (pos <= face.size() && names.size() < 16) {
        size_t comma = face.find(',', pos);
        if (comma == std::string::npos) comma = face.size();
        std::string t = trim_ws(face.substr(pos, comma - pos));
        pos           = comma + 1;
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) t = t.substr(1, t.size() - 2);
        if (t.empty()) continue;
        std::string l = lower(t);
        if (!have_generic) {
            have_generic = true;
            if (l == "serif") generic = CSS_FONT_FAMILY_SERIF;
            else if (l == "sans-serif") generic = CSS_FONT_FAMILY_SANS_SERIF;
            else if (l == "monospace") generic = CSS_FONT_FAMILY_MONOSPACE;
            else if (l == "cursive") generic = CSS_FONT_FAMILY_CURSIVE;
            else if (l == "fantasy") generic = CSS_FONT_FAMILY_FANTASY;
            else have_generic = false;
            if (have_generic) continue;
        }
        names.push_back(t);
    }
    if (names.empty() && !have_generic) return;
    auto** arr = static_cast<lwc_string**>(std::malloc((names.size() + 1) * sizeof(lwc_string*)));
    if (!arr) return;
    size_t k = 0;
    for (auto& nm : names)
        if (lwc_string* s = intern(nm)) arr[k++] = s;
    arr[k] = nullptr;
    o.add(CSS_PROP_FONT_FAMILY, generic).data.strings = arr;
}

void font_size(const std::string& raw, Out& o) {
    std::string s = trim_ws(raw);
    size_t      i = 0;
    int         sign = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) sign = s[i++] == '+' ? 1 : -1;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return;
    int v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && v < 100) v = v * 10 + (s[i++] - '0');
    if (sign) v = 3 + sign * v;
    v = std::clamp(v, 1, 7);
    static const uint8_t kw[] = {0, CSS_FONT_SIZE_X_SMALL, CSS_FONT_SIZE_SMALL, CSS_FONT_SIZE_MEDIUM, CSS_FONT_SIZE_LARGE,
                                 CSS_FONT_SIZE_X_LARGE, CSS_FONT_SIZE_XX_LARGE};
    if (v == 7) o.px(CSS_PROP_FONT_SIZE, CSS_FONT_SIZE_DIMENSION, 48); // xxx-large
    else o.keyword(CSS_PROP_FONT_SIZE, kw[v]);
}

void list_type(const std::string& raw, bool ordered_context, Out& o) {
    std::string s = trim_ws(raw);
    if (s.empty()) return;
    if (s == "1") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_DECIMAL);
    else if (s == "a") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_LOWER_ALPHA);
    else if (s == "A") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_UPPER_ALPHA);
    else if (s == "i") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_LOWER_ROMAN);
    else if (s == "I") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_UPPER_ROMAN);
    else {
        std::string l = lower(s);
        if (l == "disc") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_DISC);
        else if (l == "circle") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_CIRCLE);
        else if (l == "square") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_SQUARE);
        else if (l == "none") o.keyword(CSS_PROP_LIST_STYLE_TYPE, CSS_LIST_STYLE_TYPE_NONE);
    }
    (void)ordered_context;
}

void hr_hints(const Attrs& a, Out& o) {
    if (const std::string* al = find(a, "align")) {
        std::string s = lower(trim_ws(*al));
        if (s == "left") { o.margin(8, 0); o.margin_auto(2); }
        else if (s == "right") { o.margin_auto(8); o.margin(2, 0); }
        else if (s == "center") o.margin_auto(2 | 8);
    }
    uint32_t c     = 0;
    bool     color = attr_color(a, "color", c);
    bool     noshade = find(a, "noshade") != nullptr;
    if (color) {
        o.text_color(c);
        o.border_color(c);
        o.bg_color(c);
    }
    if (noshade) {
        o.border_style(CSS_BORDER_STYLE_SOLID);
        if (!color) { o.border_color(kGray); o.bg_color(kGray); }
    }
    int size;
    if (const std::string* sz = find(a, "size"); sz && parse_uint(*sz, size)) {
        if (color || noshade) {
            o.border_width(15, size / 2.f);
            o.height(0, false);
        } else if (size == 1) {
            o.border_width(4, 0);
        } else if (size > 1) {
            o.height(static_cast<float>(size - 2), false);
        }
    }
    dimension(a, "width", false, o);
}

void table_hints(const Attrs& a, bool quirks, Out& o) {
    if (quirks) {
        // Quirks mode: tables don't inherit font sizes / weights / alignment (every browser's quirks sheet).
        o.keyword(CSS_PROP_FONT_WEIGHT, CSS_FONT_WEIGHT_NORMAL);
        o.keyword(CSS_PROP_FONT_STYLE, CSS_FONT_STYLE_NORMAL);
        o.keyword(CSS_PROP_FONT_VARIANT, CSS_FONT_VARIANT_NORMAL);
        o.keyword(CSS_PROP_FONT_SIZE, CSS_FONT_SIZE_MEDIUM);
        o.keyword(CSS_PROP_LINE_HEIGHT, CSS_LINE_HEIGHT_NORMAL);
        o.keyword(CSS_PROP_WHITE_SPACE, CSS_WHITE_SPACE_NORMAL);
        o.keyword(CSS_PROP_TEXT_ALIGN, CSS_TEXT_ALIGN_DEFAULT);
    }
    bgcolor_and_background(a, o);
    dimension(a, "width", true, o);
    dimension(a, "height", false, o);
    int n;
    if (const std::string* v = find(a, "cellspacing"); v && parse_uint(*v, n)) {
        css_hint& h              = o.add(CSS_PROP_BORDER_SPACING, CSS_BORDER_SPACING_SET);
        h.data.position.h.value  = INTTOFIX(n);
        h.data.position.h.unit   = CSS_UNIT_PX;
        h.data.position.v.value  = INTTOFIX(n);
        h.data.position.v.unit   = CSS_UNIT_PX;
    }
    int border = table_border(a);
    if (border >= 0) {
        uint32_t bc = kGray;
        attr_color(a, "bordercolor", bc);
        o.border_width(15, static_cast<float>(border));
        o.border_style(CSS_BORDER_STYLE_OUTSET);
        o.border_color(bc);
    } else {
        uint32_t bc;
        if (attr_color(a, "bordercolor", bc)) o.border_color(bc);
    }
    if (const std::string* al = find(a, "align")) {
        std::string s = lower(trim_ws(*al));
        if (s == "left") o.keyword(CSS_PROP_FLOAT, CSS_FLOAT_LEFT);
        else if (s == "right") o.keyword(CSS_PROP_FLOAT, CSS_FLOAT_RIGHT);
        else if (s == "center") o.margin_auto(2 | 8);
    }
    if (const std::string* v = find(a, "hspace"); v && parse_uint(*v, n)) o.margin(2 | 8, static_cast<float>(n));
    if (const std::string* v = find(a, "vspace"); v && parse_uint(*v, n)) o.margin(1 | 4, static_cast<float>(n));
}

void cell_hints(dom_node* n, const Attrs& a, Out& o) {
    bgcolor_and_background(a, o);
    block_align(a, o, true);
    valign(a, o);
    dimension(a, "width", true, o);
    dimension(a, "height", true, o);
    if (find(a, "nowrap")) {
        // nowrap is ignored when the cell also has a fixed pixel width (the width wins in every browser)
        float f;
        bool  pct = false;
        const std::string* w = find(a, "width");
        bool fixed = w && parse_html_dimension(*w, f, pct) && !pct && f > 0;
        if (!fixed) o.keyword(CSS_PROP_WHITE_SPACE, CSS_WHITE_SPACE_NOWRAP);
    }
    if (dom_node* table = ancestor(n, "table")) {
        Attrs ta = all_attrs(table);
        int   p;
        if (const std::string* v = find(ta, "cellpadding"); v && parse_uint(*v, p)) o.padding(static_cast<float>(p));
        if (table_border(ta) > 0) {
            uint32_t bc = kGray;
            attr_color(ta, "bordercolor", bc);
            o.border_width(15, 1);
            o.border_style(CSS_BORDER_STYLE_INSET);
            o.border_color(bc);
        }
    }
}

} // namespace

bool parse_html_dimension(const std::string& value, float& out, bool& percent) {
    size_t i = 0;
    while (i < value.size() && is_ws(value[i])) ++i;
    if (i < value.size() && value[i] == '+') ++i;
    size_t start = i;
    double v = 0;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) v = v * 10 + (value[i++] - '0');
    bool digits = i > start;
    if (i < value.size() && value[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            v += (value[i++] - '0') * scale;
            scale /= 10;
            digits = true;
        }
    }
    if (!digits) return false;
    percent = i < value.size() && value[i] == '%';
    out     = static_cast<float>(std::min(v, 1e6));
    return true;
}

bool parse_legacy_color(const std::string& value, uint32_t& argb) {
    std::string s = trim_ws(value);
    if (s.empty()) return false;
    std::string l = lower(s);
    if (l == "transparent") return false;
    bool alpha_only = std::all_of(l.begin(), l.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    if (alpha_only) {
        bool cur = false;
        uint32_t c;
        if (l != "currentcolor" && parse_css_color(l, c, cur) && !cur) { argb = c | 0xff000000u; return true; }
    }
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() == 4 && s[0] == '#' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0 && hexv(s[3]) >= 0) {
        argb = 0xff000000u | (static_cast<uint32_t>(hexv(s[1]) * 17) << 16) | (static_cast<uint32_t>(hexv(s[2]) * 17) << 8) |
               static_cast<uint32_t>(hexv(s[3]) * 17);
        return true;
    }
    // The forgiving algorithm. Work on code points: anything above U+FFFF becomes "00", other non-hex '0'.
    std::string t;
    for (size_t i = 0; i < s.size() && t.size() < 128;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { t += s[i]; ++i; continue; }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        t += len == 4 ? "00" : "0";
        i += static_cast<size_t>(len);
    }
    if (t.size() > 128) t.resize(128);
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    for (char& c : t)
        if (hexv(c) < 0) c = '0';
    while (t.empty() || t.size() % 3 != 0) t += '0';
    size_t n = t.size() / 3;
    std::string comp[3] = {t.substr(0, n), t.substr(n, n), t.substr(2 * n, n)};
    if (n > 8)
        for (auto& c : comp) c = c.substr(n - 8);
    n = comp[0].size();
    while (n > 2 && comp[0][0] == '0' && comp[1][0] == '0' && comp[2][0] == '0') {
        for (auto& c : comp) c.erase(0, 1);
        --n;
    }
    if (n > 2)
        for (auto& c : comp) c.resize(2);
    uint32_t rgb[3];
    for (int k = 0; k < 3; ++k) {
        uint32_t v = 0;
        for (char c : comp[k]) v = v * 16 + static_cast<uint32_t>(hexv(c));
        rgb[k] = v & 0xff;
    }
    argb = 0xff000000u | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
    return true;
}

void presentational_hints(dom_node* n, bool quirks, const std::function<bool(dom_node*)>& visited, std::vector<css_hint>& hv) {
    std::string tag = tag_of(n);
    if (tag.empty()) return;
    Out o{hv};
    // Elements whose hints come from somewhere else than their own attributes.
    if (tag == "a" || tag == "area") {
        Attrs a = all_attrs(n);
        if (find(a, "href")) {
            if (dom_node* body = ancestor(n, "body")) {
                uint32_t c;
                bool v = visited && visited(n);
                if (attr_color(all_attrs(body), v ? "vlink" : "link", c)) o.text_color(c);
            }
        }
        return;
    }
    if (tag == "center") { o.keyword(CSS_PROP_TEXT_ALIGN, CSS_TEXT_ALIGN_LIBCSS_CENTER); return; }
    if (tag == "table" && quirks) {
        table_hints(all_attrs(n), true, o);
        return;
    }

    // Most elements (span, a div without align, ...) have no attribute that maps to CSS: skip reading their attributes.
    static const char* const kHinted[] = {"body", "font", "table", "td", "th", "tr", "thead", "tbody", "tfoot", "col", "colgroup",
                                          "div", "caption", "p", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "img", "object",
                                          "embed", "iframe", "video", "applet", "input", "br", "ul", "ol", "li", "marquee", "pre"};
    bool hinted = false;
    for (const char* h : kHinted)
        if (tag == h) { hinted = true; break; }
    if (!hinted) return;
    Attrs a = all_attrs(n);
    if (a.empty()) return;
    uint32_t c;
    int      k;

    if (tag == "body") {
        bgcolor_and_background(a, o);
        if (attr_color(a, "text", c)) o.text_color(c);
        auto mg = [&](const char* name, int sides) {
            if (const std::string* v = find(a, name); v && parse_uint(*v, k)) o.margin(sides, static_cast<float>(k));
        };
        mg("marginheight", 1 | 4);
        mg("marginwidth", 2 | 8);
        mg("topmargin", 1);
        mg("bottommargin", 4);
        mg("leftmargin", 8);
        mg("rightmargin", 2);
    } else if (tag == "font") {
        if (attr_color(a, "color", c)) o.text_color(c);
        if (const std::string* v = find(a, "face")) font_face(*v, o);
        if (const std::string* v = find(a, "size")) font_size(*v, o);
    } else if (tag == "table") {
        table_hints(a, false, o);
    } else if (tag == "td" || tag == "th") {
        cell_hints(n, a, o);
    } else if (tag == "tr" || tag == "thead" || tag == "tbody" || tag == "tfoot") {
        bgcolor_and_background(a, o);
        block_align(a, o, true);
        valign(a, o);
        if (tag == "tr") dimension(a, "height", true, o);
    } else if (tag == "col" || tag == "colgroup") {
        dimension(a, "width", true, o);
        valign(a, o);
    } else if (tag == "div" || tag == "caption") {
        block_align(a, o, true);
    } else if (tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6") {
        block_align(a, o, false);
    } else if (tag == "hr") {
        hr_hints(a, o);
    } else if (tag == "img") {
        replaced_common(a, o);
        if (const std::string* v = find(a, "border"); v && parse_uint(*v, k)) {
            o.border_width(15, static_cast<float>(k));
            o.border_style(CSS_BORDER_STYLE_SOLID);
        }
    } else if (tag == "object" || tag == "embed" || tag == "iframe" || tag == "video" || tag == "applet") {
        replaced_common(a, o);
        if (tag == "object" || tag == "iframe")
            if (const std::string* v = find(a, "border"); v && parse_uint(*v, k)) {
                o.border_width(15, static_cast<float>(k));
                o.border_style(CSS_BORDER_STYLE_SOLID);
            }
        if (tag == "iframe")
            if (const std::string* v = find(a, "frameborder")) {
                std::string f = lower(trim_ws(*v));
                if (f == "0" || f == "no") {
                    o.border_width(15, 0);
                    o.border_style(CSS_BORDER_STYLE_NONE);
                }
            }
    } else if (tag == "input") {
        const std::string* t = find(a, "type");
        if (t && lower(trim_ws(*t)) == "image") replaced_common(a, o);
    } else if (tag == "br") {
        if (const std::string* v = find(a, "clear")) {
            std::string s = lower(trim_ws(*v));
            if (s == "left") o.keyword(CSS_PROP_CLEAR, CSS_CLEAR_LEFT);
            else if (s == "right") o.keyword(CSS_PROP_CLEAR, CSS_CLEAR_RIGHT);
            else if (s == "all" || s == "both") o.keyword(CSS_PROP_CLEAR, CSS_CLEAR_BOTH);
        }
    } else if (tag == "ul" || tag == "ol" || tag == "li") {
        if (const std::string* v = find(a, "type")) list_type(*v, tag == "ol", o);
    } else if (tag == "marquee") {
        if (attr_color(a, "bgcolor", c)) o.bg_color(c);
        dimension(a, "width", false, o);
        dimension(a, "height", false, o);
    } else if (tag == "pre") {
        if (find(a, "wrap")) o.keyword(CSS_PROP_WHITE_SPACE, CSS_WHITE_SPACE_PRE_WRAP);
    }
}

} // namespace quill::engine
