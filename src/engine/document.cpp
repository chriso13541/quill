// Engine document: libdom tree + libcss cascade. See document.h.
#include "document.h"

#include "boxbuilder.h"
#include "cssvars.h"
#include "elemcss.h"
#include "hints.h"
#include "csslex.h"
#include <chrono>
#include <cstdio>
#include "layout.h"
#include "ua_css.h"
#include "url.h"

extern "C" {
#include <bindings/hubbub/parser.h>
#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libwapcaplet/libwapcaplet.h>
}

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quill::engine {
namespace {

using namespace domutil;

// ------------------------------------------------------------------ formatting for dumps

const char* display_name(uint8_t d) {
    switch (d) {
    case CSS_DISPLAY_INLINE: return "inline";
    case CSS_DISPLAY_BLOCK: return "block";
    case CSS_DISPLAY_LIST_ITEM: return "list-item";
    case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
    case CSS_DISPLAY_TABLE: return "table";
    case CSS_DISPLAY_INLINE_TABLE: return "inline-table";
    case CSS_DISPLAY_TABLE_ROW_GROUP: return "table-row-group";
    case CSS_DISPLAY_TABLE_HEADER_GROUP: return "table-header-group";
    case CSS_DISPLAY_TABLE_FOOTER_GROUP: return "table-footer-group";
    case CSS_DISPLAY_TABLE_ROW: return "table-row";
    case CSS_DISPLAY_TABLE_COLUMN_GROUP: return "table-column-group";
    case CSS_DISPLAY_TABLE_COLUMN: return "table-column";
    case CSS_DISPLAY_TABLE_CELL: return "table-cell";
    case CSS_DISPLAY_TABLE_CAPTION: return "table-caption";
    case CSS_DISPLAY_NONE: return "none";
    case CSS_DISPLAY_FLEX: return "flex";
    case CSS_DISPLAY_INLINE_FLEX: return "inline-flex";
    case CSS_DISPLAY_GRID: return "grid";
    default: return "?";
    }
}
const char* position_name(uint8_t p) {
    switch (p) {
    case CSS_POSITION_STATIC: return "static";
    case CSS_POSITION_RELATIVE: return "relative";
    case CSS_POSITION_ABSOLUTE: return "absolute";
    case CSS_POSITION_FIXED: return "fixed";
    case CSS_POSITION_STICKY: return "sticky";
    default: return "?";
    }
}
const char* float_name(uint8_t f) {
    switch (f) {
    case CSS_FLOAT_LEFT: return "left";
    case CSS_FLOAT_RIGHT: return "right";
    default: return "none";
    }
}
const char* unit_name(css_unit u) {
    switch (u) {
    case CSS_UNIT_PX: return "px";
    case CSS_UNIT_EX: return "ex";
    case CSS_UNIT_EM: return "em";
    case CSS_UNIT_PT: return "pt";
    case CSS_UNIT_PCT: return "%";
    case CSS_UNIT_REM: return "rem";
    case CSS_UNIT_VH: return "vh";
    case CSS_UNIT_VW: return "vw";
    default: return "u";
    }
}
std::string num(css_fixed v) {
    char b[32];
    float f = FIXTOFLT(v);
    if (f == static_cast<float>(static_cast<long>(f))) std::snprintf(b, sizeof b, "%ld", static_cast<long>(f));
    else std::snprintf(b, sizeof b, "%.2f", f);
    return b;
}
std::string len(css_fixed v, css_unit u) { return num(v) + unit_name(u); }
std::string hexcolor(css_color c) {
    char b[16];
    if ((c >> 24) == 0xff) std::snprintf(b, sizeof b, "#%06x", c & 0xffffff);
    else std::snprintf(b, sizeof b, "#%08x", c);
    return b;
}

// ------------------------------------------------------------------ libcss callbacks

css_fixed measure_cb(void*, const css_computed_style* style, const css_unit unit) {
    css_fixed l = 0;
    css_unit  u = CSS_UNIT_PX;
    css_computed_font_size(style, &l, &u);
    // No font metrics here yet: ex and ch are about half an em, line height about 1.2 em.
    if (unit == CSS_UNIT_LH) return FMUL(l, FLTTOFIX(1.2f));
    return FMUL(l, FLTTOFIX(0.5f));
}

} // namespace

// ------------------------------------------------------------------ implementation

struct Document::Impl {
    dom_hubbub_parser*   parser = nullptr;
    dom_document*        doc    = nullptr;
    css_select_ctx*      ctx    = nullptr;
    css_select_handler   handler{};
    css_computed_style*  default_style = nullptr;
    std::vector<css_stylesheet*> sheets; // the UA sheet; imported children are owned by their parent
    struct Pending { std::string text, base; };
    std::vector<Pending>         pending;        // author sheets as text, compiled in resolve_styles
    std::vector<css_stylesheet*> author_compiled;
    std::unique_ptr<CssVars>     vars;
    std::unique_ptr<ElementCascade> cascade;
    std::unordered_map<dom_node*, int> node_index;
    std::unordered_map<dom_node*, std::vector<lwc_string*>> class_cache;
    // Per-node answers libcss asks for over and over while matching (tag name, :link / :visited), kept for one
    // styling pass: cleared with the class lists and at the start of every resolve_styles().
    std::unordered_set<std::string>            attr_refs;      // attribute names the style sheets' selectors use
    bool                                       attr_refs_all = false; // (a selector we could not read: all of them)
    void note_attr_refs(const std::string& css) {
        for (size_t p = css.find('['); p != std::string::npos; p = css.find('[', p + 1)) {
            size_t i = p + 1;
            while (i < css.size() && lex::is_ws(css[i])) ++i;
            if (i >= css.size()) break;
            unsigned char c = static_cast<unsigned char>(css[i]);
            if (c == '\\' || c >= 0x80) { attr_refs_all = true; return; }
            if (c == '*' || c == '|') { // namespace prefix
                ++i;
                if (i < css.size() && css[i] == '|') ++i;
            }
            size_t j = i;
            while (j < css.size() && (std::isalnum(static_cast<unsigned char>(css[j])) || css[j] == '-' || css[j] == '_')) ++j;
            if (j < css.size() && css[j] == '|' && (j + 1 >= css.size() || css[j + 1] != '=')) { // ns|name
                i = ++j;
                while (j < css.size() && (std::isalnum(static_cast<unsigned char>(css[j])) || css[j] == '-' || css[j] == '_')) ++j;
            }
            if (j > i) attr_refs.insert(lex::lower(css.substr(i, j - i)));
            if (j < css.size() && css[j] == '\\') { attr_refs_all = true; return; }
        }
    }
    std::deque<std::vector<lwc_string*>>       class_arrays; // handed to libcss by h_node_classes (see there)
    size_t                                     class_arrays_used = 0;
    std::unordered_map<dom_node*, lwc_string*> tag_cache;   // lower-case tag name, interned
    std::unordered_map<dom_node*, uint8_t>     link_cache;  // 1 = known, 2 = a link with an href, 4 = visited
    dom_node*                                  cls_last = nullptr;
    const std::vector<lwc_string*>*            cls_last_v = nullptr;
    void clear_class_cache() {
        for (auto& kv : class_cache) for (auto* c : kv.second) lwc_string_unref(c);
        class_cache.clear();
        cls_last = nullptr;
        cls_last_v = nullptr;
        clear_node_caches();
    }
    void clear_node_caches() {
        for (auto& kv : tag_cache) if (kv.second) lwc_string_unref(kv.second);
        tag_cache.clear();
        link_cache.clear();
    }
    lwc_string* tag_lwc(dom_node* n) {
        auto it = tag_cache.find(n);
        if (it != tag_cache.end()) return it->second;
        return tag_cache.emplace(n, intern(tag_of(n))).first->second;
    }
    uint8_t link_state(dom_node* n) {
        auto it = link_cache.find(n);
        if (it != link_cache.end()) return it->second;
        uint8_t     st = 1;
        std::string href;
        lwc_string* t = tag_lwc(n);
        std::string ts = t ? std::string(lwc_string_data(t), lwc_string_length(t)) : std::string();
        if ((ts == "a" || ts == "area" || ts == "link") && get_attr(n, "href", href)) {
            st |= 2;
            if (ts != "link" && link_visited && link_visited(href)) st |= 4;
        }
        link_cache.emplace(n, st);
        return st;
    }
    std::vector<dom_node*>       cascade_nodes;
    std::map<std::string, std::string> import_cache;
    std::vector<std::pair<std::string, std::string>> sheet_texts; // every author sheet (imports included): text, base url
    uint64_t font_scope = 0;
    bool     laid_out = false;        // `boxes` went through layout() (and nothing rebuilt them since)
    bool     paint_only = false;      // the last resolve_styles() only changed how things look: the layout stands
    size_t   paint_only_count = 0;
    bool   authors_dirty = true;
    std::unordered_set<dom_node*> hover_set, active_set, focus_within_set;
    dom_node* hover_leaf = nullptr;
    dom_node* active_leaf = nullptr;
    dom_node* focus_leaf = nullptr;
    bool   has_hover = false, has_fw = false;
    css_stylesheet* variants_sheet = nullptr;
    std::string     variants_text;
    std::vector<dom_node*> restyle_nodes; // elements whose :hover/:focus/... selector match flipped (plain-property rules only)
    bool   apply_state(); // pushes the state sets into the cascade; true if styles must be recomputed
    void   build_boxes(int vw, int vh);
    bool   restyle_partial(int vw, int vh);
    static bool layout_equal(const Style& a, const Style& b);
    static bool same_layout(const Box& a, const Box& b, int depth);
    static void swap_styles(Box& keep, Box& fresh);
    int    cvw = -1, cvh = -1;
    float  zoom = 1.f, styled_zoom = 1.f;
    uint64_t box_gen = 0; // bumped whenever the box tree is (re)built // page zoom asked for / the one the styles and boxes were made with
    VarStats var_stats{};
    std::unordered_map<dom_node*, css_computed_style*> styles;
    std::unordered_map<dom_node*, css_computed_style*> before_styles, after_styles; // ::before / ::after
    std::unique_ptr<Box>                               boxes;
    TextMeasurer*                                      measurer = nullptr;
    ImageSizer                                         image_sizer;
    ImageBroken                                        image_broken;
    std::function<void(const std::string&)>            image_request;
    std::function<bool(const std::string&)>            link_visited;
    bool is_visited(dom_node* n) { return link_visited && (link_state(n) & 4); }
    int                                                last_vw = 1024, last_vh = 768;
    std::unordered_map<dom_node*, void*>               node_data;
    std::vector<dom_node*>                              element_order;
    size_t n_elements = 0;
    int vw = 0, vh = 0;
    dom_node* root_el = nullptr;
    std::string error;
    bool        quirks = false;          // full quirks mode (no / legacy doctype): presentational table quirk
    std::vector<css_hint> hint_buf;      // presentational hints of the element being styled (libcss reads them in place)
    CssLoader   loader;

    Impl();
    ~Impl();

    css_stylesheet* make_sheet(const std::string& css, const std::string& base, bool inline_style, int depth);
    bool            compile_authors(int vw, int vh);
    void            walk(dom_node* n, const std::function<void(dom_node*)>& fn);
    void            style_subtree(dom_node* n, css_computed_style* parent, const css_unit_ctx& uc, const css_media& media);
    void            dump_node(dom_node* n, int depth, bool with_styles, std::string& out) const;
};

namespace {

Document::Impl* self(void* pw) { return static_cast<Document::Impl*>(pw); }

css_error resolve_cb(void*, const char* base, lwc_string* rel, lwc_string** abs) {
    std::string r = resolve_url(base ? base : "", lwcs(rel));
    if (r.empty()) r = lwcs(rel);
    *abs = intern(r);
    return *abs ? CSS_OK : CSS_NOMEM;
}
css_error import_cb(void*, css_stylesheet*, lwc_string*) { return CSS_OK; } // imports are fetched after data_done

// --- selection handler: answers libcss's questions about a DOM node
css_error h_node_name(void* pw, void* node, css_qname* q) {
    q->ns   = nullptr;
    lwc_string* t = static_cast<Document::Impl*>(pw)->tag_lwc(static_cast<dom_node*>(node));
    q->name = t ? lwc_string_ref(t) : nullptr;
    return q->name ? CSS_OK : CSS_NOMEM;
}
// Class lists are parsed from the attribute once per node (libdom's cached list goes stale when the engine
// tags elements) and kept as interned strings until the author sheets are recompiled.
const std::vector<lwc_string*>& classes_of(Document::Impl* d, dom_node* n) {
    if (n == d->cls_last && d->cls_last_v) return *d->cls_last_v; // libcss asks about one node many times in a row
    auto it = d->class_cache.find(n);
    if (it != d->class_cache.end()) {
        d->cls_last   = n;
        d->cls_last_v = &it->second;
        return it->second;
    }
    std::vector<lwc_string*> out;
    std::string v;
    if (get_attr(n, "class", v)) {
        size_t i = 0;
        while (i < v.size()) {
            while (i < v.size() && lex::is_ws(v[i])) ++i;
            size_t j = i;
            while (j < v.size() && !lex::is_ws(v[j])) ++j;
            if (j > i) if (lwc_string* ls = intern(v.substr(i, j - i))) out.push_back(ls);
            i = j;
        }
    }
    const auto& cv = d->class_cache.emplace(n, std::move(out)).first->second; // (references survive rehashing)
    d->cls_last   = n;
    d->cls_last_v = &cv;
    return cv;
}
css_error h_node_classes(void* pw, void* node, lwc_string*** classes, uint32_t* n) {
    // libcss never frees the array, only the strings (each handed out with a ref). Arrays live until the next
    // css_select_style(): one call asks for the node's own list and then for those of the siblings it tries to
    // share a style with, all alive at once (a small shared ring used to be overwritten under its feet).
    auto* d  = self(pw);
    *classes = nullptr;
    *n       = 0;
    const auto& cl = classes_of(d, static_cast<dom_node*>(node));
    if (cl.empty()) return CSS_OK;
    if (d->class_arrays_used == d->class_arrays.size()) d->class_arrays.emplace_back();
    std::vector<lwc_string*>& out = d->class_arrays[d->class_arrays_used++];
    out.clear();
    for (lwc_string* c : cl) out.push_back(lwc_string_ref(c));
    *classes = out.data();
    *n       = static_cast<uint32_t>(out.size());
    return CSS_OK;
}
css_error h_node_id(void*, void* node, lwc_string** id) {
    *id = nullptr;
    std::string v;
    if (get_attr(static_cast<dom_node*>(node), "id", v) && !v.empty()) *id = intern(v);
    return CSS_OK;
}
bool name_matches(void* pw, dom_node* n, const css_qname* q) {
    if (!q->name) return false;
    if (lwc_string_length(q->name) == 1 && lwc_string_data(q->name)[0] == '*') return true;
    lwc_string* t  = static_cast<Document::Impl*>(pw)->tag_lwc(n);
    bool        eq = false;
    return t && lwc_string_caseless_isequal(t, q->name, &eq) == lwc_error_ok && eq;
}
css_error h_named_ancestor(void* pw, void* node, const css_qname* q, void** out) {
    *out = nullptr;
    for (dom_node* p = parent_element(static_cast<dom_node*>(node)); p; p = parent_element(p))
        if (name_matches(pw, p, q)) { *out = p; break; }
    return CSS_OK;
}
css_error h_named_parent(void* pw, void* node, const css_qname* q, void** out) {
    *out = nullptr;
    dom_node* p = parent_element(static_cast<dom_node*>(node));
    if (p && name_matches(pw, p, q)) *out = p;
    return CSS_OK;
}
css_error h_named_sibling(void* pw, void* node, const css_qname* q, void** out) {
    *out = nullptr;
    dom_node* p = prev_element(static_cast<dom_node*>(node));
    if (p && name_matches(pw, p, q)) *out = p;
    return CSS_OK;
}
css_error h_named_generic_sibling(void* pw, void* node, const css_qname* q, void** out) {
    *out = nullptr;
    for (dom_node* p = prev_element(static_cast<dom_node*>(node)); p; p = prev_element(p))
        if (name_matches(pw, p, q)) { *out = p; break; }
    return CSS_OK;
}
css_error h_parent_node(void*, void* node, void** out) {
    *out = parent_element(static_cast<dom_node*>(node));
    return CSS_OK;
}
css_error h_sibling_node(void*, void* node, void** out) {
    *out = prev_element(static_cast<dom_node*>(node));
    return CSS_OK;
}
css_error h_has_name(void* pw, void* node, const css_qname* q, bool* m) {
    *m = name_matches(pw, static_cast<dom_node*>(node), q);
    return CSS_OK;
}
css_error h_has_class(void* pw, void* node, lwc_string* name, bool* m) {
    *m = false;
    for (lwc_string* c : classes_of(self(pw), static_cast<dom_node*>(node))) {
        bool eq = false;
        if (lwc_string_caseless_isequal(c, name, &eq) == lwc_error_ok && eq) { *m = true; break; }
    }
    return CSS_OK;
}
css_error h_has_id(void*, void* node, lwc_string* name, bool* m) {
    std::string v;
    *m = get_attr(static_cast<dom_node*>(node), "id", v) && v == lwcs(name);
    return CSS_OK;
}

enum class AttrOp { Exists, Equal, Dash, Includes, Prefix, Suffix, Substring };
css_error attr_test(void* node, const css_qname* q, lwc_string* value, bool* m, AttrOp op) {
    *m = false;
    std::string have;
    if (!get_attr(static_cast<dom_node*>(node), lower(lwcs(q->name)).c_str(), have)) return CSS_OK;
    if (op == AttrOp::Exists) { *m = true; return CSS_OK; }
    std::string want = lwcs(value);
    switch (op) {
    case AttrOp::Equal: *m = have == want; break;
    case AttrOp::Dash: *m = have == want || (have.size() > want.size() && have.compare(0, want.size(), want) == 0 && have[want.size()] == '-'); break;
    case AttrOp::Includes: *m = !want.empty() && want.find_first_of(" \t\r\n\f") == std::string::npos && has_token(have, lower(want).c_str()) && [&] {
        // case-sensitive word match
        size_t i = 0;
        while (i < have.size()) {
            while (i < have.size() && std::isspace(static_cast<unsigned char>(have[i]))) i++;
            size_t j = i;
            while (j < have.size() && !std::isspace(static_cast<unsigned char>(have[j]))) j++;
            if (have.substr(i, j - i) == want) return true;
            i = j;
        }
        return false;
    }(); break;
    case AttrOp::Prefix: *m = !want.empty() && have.size() >= want.size() && have.compare(0, want.size(), want) == 0; break;
    case AttrOp::Suffix: *m = !want.empty() && have.size() >= want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0; break;
    case AttrOp::Substring: *m = !want.empty() && have.find(want) != std::string::npos; break;
    default: break;
    }
    return CSS_OK;
}
css_error h_has_attr(void*, void* n, const css_qname* q, bool* m) { return attr_test(n, q, nullptr, m, AttrOp::Exists); }
css_error h_attr_eq(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Equal); }
css_error h_attr_dash(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Dash); }
css_error h_attr_incl(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Includes); }
css_error h_attr_prefix(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Prefix); }
css_error h_attr_suffix(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Suffix); }
css_error h_attr_substr(void*, void* n, const css_qname* q, lwc_string* v, bool* m) { return attr_test(n, q, v, m, AttrOp::Substring); }

css_error h_is_root(void*, void* node, bool* m) {
    dom_node* p = parent_of(static_cast<dom_node*>(node));
    *m          = p && type_of(p) == DOM_DOCUMENT_NODE;
    return CSS_OK;
}
css_error h_count_siblings(void*, void* node, bool same_name, bool after, int32_t* count) {
    dom_node*   n    = static_cast<dom_node*>(node);
    std::string name = same_name ? tag_of(n) : std::string();
    int32_t     c    = 0;
    for (dom_node* p = after ? next_element(n) : prev_element(n); p; p = after ? next_element(p) : prev_element(p))
        if (!same_name || tag_of(p) == name) c++;
    *count = c;
    return CSS_OK;
}
css_error h_is_empty(void*, void* node, bool* m) {
    *m = true;
    for (dom_node* c = first_child(static_cast<dom_node*>(node)); c; c = next_sibling(c)) {
        dom_node_type t = type_of(c);
        if (t == DOM_ELEMENT_NODE) { *m = false; break; }
        if (t == DOM_TEXT_NODE && !text_content(c).empty()) { *m = false; break; }
    }
    return CSS_OK;
}
css_error h_is_link(void* pw, void* node, bool* m) {
    // :link and :visited are exclusive: a visited link only matches :visited
    uint8_t st = static_cast<Document::Impl*>(pw)->link_state(static_cast<dom_node*>(node));
    *m = (st & 2) && !(st & 4);
    return CSS_OK;
}
css_error h_is_visited(void* pw, void* node, bool* m) {
    *m = static_cast<Document::Impl*>(pw)->is_visited(static_cast<dom_node*>(node));
    return CSS_OK;
}
css_error h_false(void*, void*, bool* m) { *m = false; return CSS_OK; }
css_error h_is_hover(void* pw, void* node, bool* m) {
    *m = static_cast<Document::Impl*>(pw)->hover_set.count(static_cast<dom_node*>(node)) != 0;
    return CSS_OK;
}
css_error h_is_active(void* pw, void* node, bool* m) {
    *m = static_cast<Document::Impl*>(pw)->active_set.count(static_cast<dom_node*>(node)) != 0;
    return CSS_OK;
}
css_error h_is_focus(void* pw, void* node, bool* m) {
    *m = static_cast<Document::Impl*>(pw)->focus_leaf == static_cast<dom_node*>(node);
    return CSS_OK;
}
css_error h_is_disabled(void*, void* node, bool* m) {
    std::string v;
    *m = get_attr(static_cast<dom_node*>(node), "disabled", v);
    return CSS_OK;
}
css_error h_is_enabled(void*, void* node, bool* m) {
    dom_node*   n = static_cast<dom_node*>(node);
    std::string t = tag_of(n), v;
    bool        form = t == "input" || t == "button" || t == "select" || t == "textarea" || t == "option" || t == "optgroup" || t == "fieldset";
    *m = form && !get_attr(n, "disabled", v);
    return CSS_OK;
}
css_error h_is_checked(void*, void* node, bool* m) {
    dom_node*   n = static_cast<dom_node*>(node);
    std::string v;
    *m = get_attr(n, "checked", v) || (tag_of(n) == "option" && get_attr(n, "selected", v));
    return CSS_OK;
}
css_error h_is_lang(void*, void* node, lwc_string* lang, bool* m) {
    *m = false;
    std::string want = lower(lwcs(lang));
    for (dom_node* n = static_cast<dom_node*>(node); n && is_element(n); n = parent_of(n)) {
        std::string v;
        if (get_attr(n, "lang", v)) {
            v  = lower(v);
            *m = v == want || (v.size() > want.size() && v.compare(0, want.size(), want) == 0 && v[want.size()] == '-');
            break;
        }
    }
    return CSS_OK;
}
css_error h_presentational(void* pw, void* node, uint32_t* n, css_hint** hints) {
    auto* d = static_cast<Document::Impl*>(pw);
    d->hint_buf.clear();
    presentational_hints(static_cast<dom_node*>(node), d->quirks, [d](dom_node* n) { return d->is_visited(n); }, d->hint_buf);
    *n     = static_cast<uint32_t>(d->hint_buf.size());
    *hints = d->hint_buf.empty() ? nullptr : d->hint_buf.data();
    return CSS_OK;
}
css_error h_ua_default(void*, uint32_t property, css_hint* hint) {
    switch (property) {
    case CSS_PROP_COLOR:
        hint->data.color = 0xff000000;
        hint->status     = CSS_COLOR_COLOR;
        return CSS_OK;
    case CSS_PROP_FONT_FAMILY:
        hint->data.strings = nullptr;
        hint->status       = CSS_FONT_FAMILY_SANS_SERIF;
        return CSS_OK;
    case CSS_PROP_QUOTES:
        hint->data.strings = nullptr;
        hint->status       = CSS_QUOTES_NONE;
        return CSS_OK;
    default: return CSS_INVALID;
    }
}
// Two siblings with one tag: libcss may give the second the first one's style if attribute selectors and
// presentational hints cannot tell them apart. Identical attribute lists guarantee that (siblings share every
// ancestor too). Table cells are left out: their hints also look at their row and table.
// Only attributes that some selector names, or that presentational hints read, can tell the two apart (an <a>'s
// href only by being there: where it points shows in :link / :visited, which libcss compares itself).
bool hint_attribute(const std::string& n) {
    static const char* const k[] = {"align", "background", "bgcolor", "border", "bordercolor", "cellpadding", "cellspacing", "clear",
                                    "color", "face", "frameborder", "height", "hspace", "vspace", "link", "vlink", "alink", "text",
                                    "marginheight", "marginwidth", "topmargin", "bottommargin", "leftmargin", "rightmargin", "noshade",
                                    "nowrap", "size", "type", "valign", "width", "wrap", "char", "charoff", "span", "colspan", "rowspan"};
    for (const char* x : k)
        if (n == x) return true;
    return false;
}
css_error h_attrs_alike(void* pw, void* node, void* other, bool* m) {
    auto* d = self(pw);
    *m      = false;
    dom_node* a = static_cast<dom_node*>(node);
    dom_node* b = static_cast<dom_node*>(other);
    std::string t = tag_of(a);
    if (t == "td" || t == "th") return CSS_OK;
    auto relevant = [d](std::vector<std::pair<std::string, std::string>> v) {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto& kv : v) {
            if (kv.first == "class" || kv.first == "id" || kv.first == "style") continue; // libcss checks those itself
            if (kv.first == "href") { out.push_back({kv.first, std::string()}); continue; }
            if (d->attr_refs_all || hint_attribute(kv.first) || d->attr_refs.count(kv.first)) out.push_back(std::move(kv));
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    *m = relevant(all_attrs(a)) == relevant(all_attrs(b));
    return CSS_OK;
}
css_error h_set_node_data(void* pw, void* node, void* data) {
    auto* d  = self(pw);
    auto  it = d->node_data.find(static_cast<dom_node*>(node));
    // Every css_select_style() hands over fresh node data: the previous one (from the last restyle) is ours to free.
    if (it != d->node_data.end() && it->second != data)
        css_libcss_node_data_handler(&d->handler, CSS_NODE_DELETED, pw, node, nullptr, it->second);
    if (data) d->node_data[static_cast<dom_node*>(node)] = data;
    else d->node_data.erase(static_cast<dom_node*>(node));
    return CSS_OK;
}
css_error h_get_node_data(void* pw, void* node, void** data) {
    auto* d = self(pw);
    auto  it = d->node_data.find(static_cast<dom_node*>(node));
    *data    = it == d->node_data.end() ? nullptr : it->second;
    return CSS_OK;
}

css_unit_ctx make_unit_ctx(int vw, int vh, const css_computed_style* root) {
    return css_unit_ctx{INTTOFIX(vw), INTTOFIX(vh), INTTOFIX(16), 0, INTTOFIX(96), root, nullptr, measure_cb};
}

} // namespace

// ------------------------------------------------------------------ compiled stylesheets shared between documents
// Pages of one site keep loading the same stylesheets, and compiling a big one with libcss is a good part of a
// page load on a slow machine. A compiled sheet is never changed by selection, so documents share them: keyed by
// the exact text compiled (after the var() pass) and its base URL. Sheets with @import are left out (their
// children come from that document's loads). Unused ones stay for a while (LRU) for the next page.
namespace {
class SharedSheets {
public:
    static SharedSheets& get() {
        static SharedSheets* s = new SharedSheets; // never freed: may still be needed while documents are torn down at exit
        return *s;
    }
    css_stylesheet* acquire(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = by_key_.find(key);
        if (it == by_key_.end()) return nullptr;
        if (it->second.users++ == 0) unused_bytes_ -= key.size();
        it->second.used = ++tick_;
        hits++;
        return it->second.sheet;
    }
    void put(const std::string& key, css_stylesheet* sh) {
        std::lock_guard<std::mutex> lk(mu_);
        if (by_key_.count(key)) return; // (raced: the caller keeps its own copy, unshared)
        Entry& e = by_key_[key];
        e.sheet  = sh;
        e.users  = 1;
        e.used   = ++tick_;
        key_of_[sh] = key;
    }
    // false: not a shared sheet (the caller owns it)
    bool release(css_stylesheet* sh) {
        std::lock_guard<std::mutex> lk(mu_);
        auto k = key_of_.find(sh);
        if (k == key_of_.end()) return false;
        Entry& e = by_key_[k->second];
        if (--e.users == 0) {
            e.used = ++tick_;
            unused_bytes_ += k->second.size();
            trim();
        }
        return true;
    }
    size_t hits = 0;

private:
    struct Entry {
        css_stylesheet* sheet = nullptr;
        int             users = 0;
        uint64_t        used  = 0;
    };
    void trim() {
        for (;;) {
            size_t unused = 0;
            auto   oldest = by_key_.end();
            for (auto it = by_key_.begin(); it != by_key_.end(); ++it) {
                if (it->second.users) continue;
                ++unused;
                if (oldest == by_key_.end() || it->second.used < oldest->second.used) oldest = it;
            }
            if (oldest == by_key_.end() || (unused <= 24 && unused_bytes_ <= (16u << 20))) return;
            unused_bytes_ -= oldest->first.size();
            key_of_.erase(oldest->second.sheet);
            css_stylesheet_destroy(oldest->second.sheet);
            by_key_.erase(oldest);
        }
    }
    std::mutex                                             mu_;
    std::unordered_map<std::string, Entry>                 by_key_;
    std::unordered_map<const css_stylesheet*, std::string> key_of_;
    uint64_t                                               tick_         = 0;
    size_t                                                 unused_bytes_ = 0;
};

void drop_sheet(css_stylesheet* sh) {
    if (sh && !SharedSheets::get().release(sh)) css_stylesheet_destroy(sh);
}

bool has_import(const std::string& css) {
    for (size_t p = css.find('@'); p != std::string::npos; p = css.find('@', p + 1))
        if (p + 7 <= css.size() && lex::lower(css.substr(p + 1, 6)) == "import") return true;
    return false;
}
} // namespace

Document::Impl::Impl() {
    handler.handler_version             = CSS_SELECT_HANDLER_VERSION_1;
    handler.node_name                   = h_node_name;
    handler.node_classes                = h_node_classes;
    handler.node_id                     = h_node_id;
    handler.named_ancestor_node         = h_named_ancestor;
    handler.named_parent_node           = h_named_parent;
    handler.named_sibling_node          = h_named_sibling;
    handler.named_generic_sibling_node  = h_named_generic_sibling;
    handler.parent_node                 = h_parent_node;
    handler.sibling_node                = h_sibling_node;
    handler.node_has_name               = h_has_name;
    handler.node_has_class              = h_has_class;
    handler.node_has_id                 = h_has_id;
    handler.node_has_attribute          = h_has_attr;
    handler.node_has_attribute_equal    = h_attr_eq;
    handler.node_has_attribute_dashmatch = h_attr_dash;
    handler.node_has_attribute_includes = h_attr_incl;
    handler.node_has_attribute_prefix   = h_attr_prefix;
    handler.node_has_attribute_suffix   = h_attr_suffix;
    handler.node_has_attribute_substring = h_attr_substr;
    handler.node_is_root                = h_is_root;
    handler.node_count_siblings         = h_count_siblings;
    handler.node_is_empty               = h_is_empty;
    handler.node_is_link                = h_is_link;
    handler.node_is_visited             = h_is_visited;
    handler.node_is_hover               = h_is_hover;
    handler.node_is_active              = h_is_active;
    handler.node_is_focus               = h_is_focus;
    handler.node_is_enabled             = h_is_enabled;
    handler.node_is_disabled            = h_is_disabled;
    handler.node_is_checked             = h_is_checked;
    handler.node_is_target              = h_false;
    handler.node_is_lang                = h_is_lang;
    handler.node_presentational_hint    = h_presentational;
    handler.ua_default_for_property     = h_ua_default;
    handler.set_libcss_node_data        = h_set_node_data;
    handler.get_libcss_node_data        = h_get_node_data;
    handler.node_attributes_alike       = h_attrs_alike;

    css_select_ctx_create(&ctx);
    note_attr_refs(kUaCss);
    css_stylesheet* ua = SharedSheets::get().acquire("\x01ua");
    if (!ua && (ua = make_sheet(kUaCss, "about:ua", false, 0))) SharedSheets::get().put("\x01ua", ua);
    if (ua) {
        css_select_ctx_append_sheet(ctx, ua, CSS_ORIGIN_UA, "all");
        sheets.push_back(ua);
    } else {
        error = "could not compile the user-agent stylesheet";
    }
}

Document::Impl::~Impl() {
    clear_class_cache();
    for (auto& kv : node_data) css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, this, kv.first, nullptr, kv.second);
    node_data.clear();
    for (auto& kv : styles)
        if (kv.second) css_computed_style_destroy(kv.second);
    styles.clear();
    for (auto* m : {&before_styles, &after_styles}) {
        for (auto& kv : *m) if (kv.second) css_computed_style_destroy(kv.second);
        m->clear();
    }
    if (default_style) css_computed_style_destroy(default_style);
    if (ctx) css_select_ctx_destroy(ctx);
    for (auto* s : author_compiled) drop_sheet(s);
    for (auto* s : sheets) drop_sheet(s);
    if (doc) dom_node_unref(doc);
    if (parser) dom_hubbub_parser_destroy(parser);
}

// libcss lacks :focus-within and :focus-visible: the first becomes a marker class kept on the focused element's ancestors,
// the second is treated as :focus.
static std::string rewrite_state_pseudos(const std::string& css) {
    if (css.find(':') == std::string::npos) return css;
    std::string out;
    out.reserve(css.size());
    for (size_t i = 0; i < css.size(); ++i) {
        if (css[i] == ':' && i + 6 < css.size()) {
            if (css.compare(i, 13, ":focus-within") == 0) { out += ".qv-fw"; i += 12; continue; }
            if (css.compare(i, 14, ":focus-visible") == 0) { out += ":focus"; i += 13; continue; }
        }
        out += css[i];
    }
    return out;
}

css_stylesheet* Document::Impl::make_sheet(const std::string& css_in, const std::string& base, bool inline_style, int depth) {
    const std::string css = inline_style ? css_in : rewrite_state_pseudos(css_in);
    css_stylesheet_params p{};
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level          = CSS_LEVEL_DEFAULT;
    p.charset        = "UTF-8";
    p.url            = base.c_str();
    p.title          = nullptr;
    p.allow_quirks   = false;
    p.inline_style   = inline_style;
    p.resolve        = resolve_cb;
    p.resolve_pw     = this;
    p.import         = import_cb;
    p.import_pw      = this;
    css_stylesheet* sheet = nullptr;
    if (css_stylesheet_create(&p, &sheet) != CSS_OK || !sheet) return nullptr;
    css_error e = css_stylesheet_append_data(sheet, reinterpret_cast<const uint8_t*>(css.data()), css.size());
    if (e != CSS_OK && e != CSS_NEEDDATA) {
        css_stylesheet_destroy(sheet);
        return nullptr;
    }
    e = css_stylesheet_data_done(sheet);
    if (e == CSS_IMPORTS_PENDING) {
        for (int guard = 0; guard < 64; guard++) {
            lwc_string* u = nullptr;
            if (css_stylesheet_next_pending_import(sheet, &u) != CSS_OK || !u) break;
            std::string href = resolve_url(base, lwcs(u));
            lwc_string_unref(u);
            std::string     text;
            css_stylesheet* child = nullptr;
            if (depth < 8) {
                auto cached = import_cache.find(href);
                bool have   = cached != import_cache.end() ? (text = cached->second, true) : (loader && loader(href, text));
                if (have) child = make_sheet(vars ? vars->apply(text) : text, href, false, depth + 1);
            }
            if (!child) child = make_sheet("", href, false, depth + 1); // a failed import is an empty one
            if (!child) break;
            css_stylesheet_register_import(sheet, child);
        }
    } else if (e != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return nullptr;
    }
    return sheet;
}

void Document::Impl::walk(dom_node* n, const std::function<void(dom_node*)>& fn) {
    for (dom_node* c = first_child(n); c; c = next_sibling(c)) {
        if (!is_element(c)) continue;
        fn(c);
        walk(c, fn);
    }
}

void Document::Impl::style_subtree(dom_node* n, css_computed_style* parent, const css_unit_ctx& uc, const css_media& media) {
    css_stylesheet* inl = nullptr;
    std::string     st;
    if (get_attr(n, "style", st) && !st.empty()) inl = make_sheet(vars ? vars->apply_declarations(st) : st, "", true, 0);

    css_select_results* res = nullptr;
    css_computed_style* composed = nullptr;
    class_arrays_used = 0; // the previous select's class arrays are done with
    css_error           e = css_select_style(ctx, n, &uc, &media, inl, &handler, this, &res);
    if (e == CSS_OK && res && res->styles[CSS_PSEUDO_ELEMENT_NONE]) {
        css_computed_style* base = parent; // NULL for the root element: libcss then resolves against defaults
        css_error ce = css_computed_style_compose(base, res->styles[CSS_PSEUDO_ELEMENT_NONE], &uc, &composed);
        if (ce != CSS_OK) {
            composed = nullptr;
            if (error.empty()) error = std::string("compose failed: ") + css_error_to_string(ce) + " on <" + tag_of(n) + ">";
        }
    } else if (e == CSS_OK && error.empty()) {
        error = "select returned no style for <" + tag_of(n) + ">";
    }
    if (e != CSS_OK && error.empty()) error = std::string("css_select_style failed: ") + css_error_to_string(e) + " on <" + tag_of(n) + ">";
    if (res && composed) {
        for (int p : {CSS_PSEUDO_ELEMENT_BEFORE, CSS_PSEUDO_ELEMENT_AFTER}) {
            if (!res->styles[p]) continue;
            css_computed_style* pc = nullptr;
            if (css_computed_style_compose(composed, res->styles[p], &uc, &pc) == CSS_OK && pc)
                (p == CSS_PSEUDO_ELEMENT_BEFORE ? before_styles : after_styles)[n] = pc;
        }
    }
    if (res) css_select_results_destroy(res);
    if (inl) css_stylesheet_destroy(inl);
    styles[n] = composed;
    n_elements++;

    // The root element defines `rem`: rebuild the unit context once it is known.
    if (!parent && composed) {
        css_unit_ctx uc2 = css_unit_ctx{uc.viewport_width, uc.viewport_height, uc.font_size_default, uc.font_size_minimum,
                                        uc.device_dpi, composed, nullptr, measure_cb};
        for (dom_node* c = first_child(n); c; c = next_sibling(c))
            if (is_element(c)) style_subtree(c, composed, uc2, media);
        return;
    }
    for (dom_node* c = first_child(n); c; c = next_sibling(c))
        if (is_element(c)) style_subtree(c, composed, uc, media);
}

void Document::Impl::dump_node(dom_node* n, int depth, bool with_styles, std::string& out) const {
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    dom_node_type t = type_of(n);
    if (t == DOM_TEXT_NODE) {
        std::string s = text_content(n), t2;
        bool        space = false;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
            if (space && !t2.empty()) t2 += ' ';
            space = false;
            t2 += c;
        }
        if (t2.empty()) return;
        if (t2.size() > 60) t2 = t2.substr(0, 57) + "...";
        out += pad + "\"" + t2 + "\"\n";
        return;
    }
    if (t != DOM_ELEMENT_NODE) return;

    std::string line = pad + tag_of(n);
    std::string v;
    if (get_attr(n, "id", v) && !v.empty()) line += "#" + v;
    if (get_attr(n, "class", v) && !v.empty()) {
        for (char& c : v)
            if (std::isspace(static_cast<unsigned char>(c))) c = '.';
        line += "." + v;
    }
    if (with_styles) {
        auto it = styles.find(n);
        if (it != styles.end() && it->second) {
            const css_computed_style* s = it->second;
            line += "  {display:";
            line += display_name(css_computed_display(s, !parent_element(n)));
            uint8_t pos = css_computed_position(s);
            if (pos != CSS_POSITION_STATIC) { line += "; position:"; line += position_name(pos); }
            uint8_t fl = css_computed_float(s);
            if (fl != CSS_FLOAT_NONE) { line += "; float:"; line += float_name(fl); }
            css_color c = 0;
            css_computed_color(s, &c);
            line += "; color:" + hexcolor(c);
            if (css_computed_background_color(s, &c) == CSS_BACKGROUND_COLOR_COLOR && (c >> 24) != 0) line += "; bg:" + hexcolor(c);
            css_fixed l = 0;
            css_unit  u = CSS_UNIT_PX;
            css_computed_font_size(s, &l, &u);
            line += "; font:" + len(l, u);
            uint8_t fw = css_computed_font_weight(s);
            if (fw == CSS_FONT_WEIGHT_BOLD || fw == CSS_FONT_WEIGHT_700) line += " bold";
            if (css_computed_width(s, &l, &u) == CSS_WIDTH_SET) line += "; w:" + len(l, u);
            if (css_computed_height(s, &l, &u) == CSS_HEIGHT_SET) line += "; h:" + len(l, u);
            auto side = [&](uint8_t (*getter)(const css_computed_style*, css_fixed*, css_unit*), uint8_t set, uint8_t autov) {
                css_fixed ll = 0;
                css_unit  uu = CSS_UNIT_PX;
                uint8_t   r  = getter(s, &ll, &uu);
                if (r == set) return len(ll, uu);
                if (r == autov) return std::string("auto");
                return std::string("0");
            };
            std::string m = side(css_computed_margin_top, CSS_MARGIN_SET, CSS_MARGIN_AUTO) + " " +
                            side(css_computed_margin_right, CSS_MARGIN_SET, CSS_MARGIN_AUTO) + " " +
                            side(css_computed_margin_bottom, CSS_MARGIN_SET, CSS_MARGIN_AUTO) + " " +
                            side(css_computed_margin_left, CSS_MARGIN_SET, CSS_MARGIN_AUTO);
            if (m != "0 0 0 0") line += "; margin:" + m;
            std::string pd = side(css_computed_padding_top, CSS_PADDING_SET, 0xff) + " " + side(css_computed_padding_right, CSS_PADDING_SET, 0xff) +
                             " " + side(css_computed_padding_bottom, CSS_PADDING_SET, 0xff) + " " + side(css_computed_padding_left, CSS_PADDING_SET, 0xff);
            if (pd != "0 0 0 0") line += "; padding:" + pd;
            line += "}";
        } else {
            line += "  {no style}";
        }
    }
    out += line + "\n";
    for (dom_node* c = first_child(n); c; c = next_sibling(c)) dump_node(c, depth + 1, with_styles, out);
}

// ------------------------------------------------------------------ public API

Document::Document() : impl_(new Impl) {}
Document::~Document() = default;

bool Document::parse_html(const std::string& html, const std::string& charset) {
    Impl& d = *impl_;
    std::string enc = charset;
    for (int attempt = 0; attempt < 2; ++attempt) {
        dom_hubbub_parser_params params{};
        params.enc           = enc.empty() ? nullptr : enc.c_str();
        params.fix_enc       = !enc.empty();
        params.enable_script = false;
        if (dom_hubbub_parser_create(&params, &d.parser, &d.doc) != DOM_HUBBUB_OK) {
            d.error = "could not create the HTML parser";
            return false;
        }
        dom_hubbub_error pe = dom_hubbub_parser_parse_chunk(d.parser, reinterpret_cast<const uint8_t*>(html.data()), html.size());
        if (pe == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE && attempt == 0) {
            // A <meta charset> disagreed with the guessed encoding: restart with the declared one.
            dom_hubbub_encoding_source src;
            const char* e = dom_hubbub_parser_get_encoding(d.parser, &src);
            enc = e ? e : "UTF-8";
            dom_node_unref(d.doc);
            dom_hubbub_parser_destroy(d.parser);
            d.doc = nullptr;
            d.parser = nullptr;
            continue;
        }
        if (pe != DOM_HUBBUB_OK) {
            d.error = "HTML parse error (hubbub code " + std::to_string(static_cast<int>(pe)) + ")";
            return false;
        }
        if (dom_hubbub_parser_completed(d.parser) != DOM_HUBBUB_OK) {
            d.error = "HTML parser could not finish";
            return false;
        }
        dom_document_quirks_mode qm = DOM_DOCUMENT_QUIRKS_MODE_NONE;
        dom_document_get_quirks_mode(d.doc, &qm);
        d.quirks = qm == DOM_DOCUMENT_QUIRKS_MODE_FULL;
        return true;
    }
    d.error = "HTML parse error (encoding kept changing)";
    return false;
}

bool Document::add_author_css(const std::string& css, const std::string& base_url) {
    Impl& d = *impl_;
    d.pending.push_back({css, base_url});
    d.authors_dirty = true;
    return true;
}

// Compiles the pending author sheets after the var() pre-pass. Called from resolve_styles (the media
// environment, and so the variable cascade, depends on the viewport).
bool Document::Impl::compile_authors(int vw, int vh) {
    if (!authors_dirty && vw == cvw && vh == cvh) return true;
    std::vector<std::pair<dom_node*, void*>> nd(node_data.begin(), node_data.end());
    node_data.clear();
    clear_class_cache();
    for (auto& kv : nd) css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, this, kv.first, nullptr, kv.second);
    for (auto* sh : author_compiled) {
        css_select_ctx_remove_sheet(ctx, sh);
        drop_sheet(sh);
    }
    author_compiled.clear();
    if (default_style) { css_computed_style_destroy(default_style); default_style = nullptr; }

    dom_node* root = nullptr;
    if (dom_document_get_document_element(doc, &root) != DOM_NO_ERR || !root) return false;
    dom_node_unref(root);
    dom_node* body = nullptr;
    for (dom_node* c = first_child(root); c; c = next_sibling(c))
        if (is_element(c) && tag_of(c) == "body") { body = c; break; }
    auto view = [](dom_node* n, const char* tag) {
        ElementView v;
        v.tag  = n ? tag : "";
        v.attr = [n](const std::string& name, std::string& out) { return n && get_attr(n, name.c_str(), out); };
        return v;
    };
    vars = std::make_unique<CssVars>(MediaEnv{vw, vh}, view(root, "html"), view(body, "body"));
    import_cache.clear();
    std::vector<std::pair<std::string, std::string>> ordered_texts; // (css, base url)
    std::function<void(const std::string&, const std::string&, int)> gather = [&](const std::string& text, const std::string& base, int depth) {
        if (depth < 8)
            for (auto& href : CssVars::imports(text)) {
                std::string abs = resolve_url(base, href);
                if (!import_cache.count(abs)) {
                    std::string t;
                    import_cache[abs] = (loader && loader(abs, t)) ? t : std::string();
                    gather(import_cache[abs], abs, depth + 1);
                }
            }
        vars->collect(text);
        ordered_texts.push_back({text, base});
    };
    auto tpv = std::chrono::steady_clock::now();
    for (auto& p : pending) gather(p.text, p.base, 0);
    vars->finish();
    sheet_texts = ordered_texts;
    for (auto& t : ordered_texts) note_attr_refs(t.first); // (only grows: a sheet that went away just costs sharing)
    auto tpv1 = std::chrono::steady_clock::now();
    has_hover = false;
    has_fw = false;
    for (auto& t : ordered_texts) {
        if (t.first.find(':') == std::string::npos) continue;
        std::string lt = lower(t.first); // once per sheet
        if (lt.find(":focus-within") != std::string::npos) has_fw = true;
        if (lt.find(":hover") != std::string::npos || lt.find(":active") != std::string::npos || lt.find(":focus") != std::string::npos) has_hover = true;
        if (has_fw && has_hover) break;
    }

    // per-element cascade: scoped custom properties, border-radius, box-shadow
    cascade_nodes.clear();
    node_index.clear();
    cascade_nodes.push_back(root);
    walk(root, [&](dom_node* n) { cascade_nodes.push_back(n); });
    std::vector<ENode> enodes(cascade_nodes.size());
    std::vector<std::vector<std::pair<std::string, std::string>>> cascade_nodes_attrs;
    for (size_t i = 0; i < cascade_nodes.size(); ++i) {
        dom_node* n = cascade_nodes[i];
        node_index[n] = static_cast<int>(i);
        enodes[i].tag = tag_of(n);
        enodes[i].hover = hover_set.count(n) != 0;
        enodes[i].active = active_set.count(n) != 0;
        enodes[i].focus = n == focus_leaf;
        enodes[i].focus_within = focus_within_set.count(n) != 0;
        enodes[i].attrs = all_attrs(n);
        for (auto& a : enodes[i].attrs)
            if (a.first == "class" && (a.second.find("qvx") != std::string::npos || a.second.find("qv-fw") != std::string::npos)) { // drop markers from an earlier run
                std::string kept, cur;
                a.second += ' ';
                for (char ch : a.second) {
                    if (std::isspace(static_cast<unsigned char>(ch))) {
                        if (!cur.empty() && cur != "qv-fw" && !(cur.size() > 3 && cur.compare(0, 3, "qvx") == 0 && std::isdigit(static_cast<unsigned char>(cur[3])))) kept += (kept.empty() ? "" : " ") + cur;
                        cur.clear();
                    } else cur += ch;
                }
                a.second = kept;
                set_attr(n, "class", kept);
            }
        dom_node* pe = parent_element(n);
        enodes[i].parent = pe && node_index.count(pe) ? node_index[pe] : -1;
    }
    auto t0 = std::chrono::steady_clock::now();
    for (auto& en : enodes) cascade_nodes_attrs.push_back(en.attrs);
    cascade = std::make_unique<ElementCascade>(*vars, std::move(enodes));
    for (auto& t : ordered_texts) cascade->add_sheet(t.first, t.second);
    auto t1 = std::chrono::steady_clock::now();
    cascade->run();
    auto t2 = std::chrono::steady_clock::now();
    if (std::getenv("QUILL_PROF"))
        std::fprintf(stderr, "elemcss: parse %.1fms run %.1fms rules=%zu variants=%zu marks=%zu tainted=%zu\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                     cascade->rules, cascade->variants, cascade->deferred_applied, vars->tainted_count());
    for (size_t i = 0; i < cascade_nodes.size(); ++i) {
        const auto& mk = cascade->marks(i);
        if (mk.empty()) continue;
        std::string v;
        for (auto& a : cascade_nodes_attrs[i]) if (a.first == "class") v = a.second;
        for (int id : mk) v += (v.empty() ? "" : " ") + std::string("qvx") + std::to_string(id);
        set_attr(cascade_nodes[i], "class", v);
    }
    for (dom_node* n : focus_within_set) { // libcss has no :focus-within: make_sheet rewrites it to .qv-fw
        std::string v;
        get_attr(n, "class", v);
        set_attr(n, "class", v.empty() ? "qv-fw" : v + " qv-fw");
    }
    clear_class_cache();
    auto tps = std::chrono::steady_clock::now();

    for (auto& p : pending) {
        std::string text = vars->apply(p.text);
        bool        share = !has_import(text);
        std::string key;
        css_stylesheet* sh = nullptr;
        if (share) {
            key.reserve(p.base.size() + 1 + text.size());
            key.append(p.base).append(1, '\0').append(text);
            sh = SharedSheets::get().acquire(key);
        }
        if (!sh) {
            sh = make_sheet(text, p.base, false, 0);
            if (!sh) continue;
            if (share) SharedSheets::get().put(key, sh);
        }
        if (css_select_ctx_append_sheet(ctx, sh, CSS_ORIGIN_AUTHOR, "all") != CSS_OK) {
            drop_sheet(sh);
            continue;
        }
        author_compiled.push_back(sh);
    }
    {
        std::string vcss = std::getenv("QUILL_NOVARIANTS") ? std::string() : cascade->variants_css();
        if (std::getenv("QUILL_DUMPVAR")) std::fprintf(stderr, "%s\n", vcss.c_str());
        variants_sheet = nullptr;
        variants_text.clear();
        note_attr_refs(vcss);
        if (!vcss.empty()) {
            css_stylesheet* sh = make_sheet(vcss, "", false, 0);
            if (sh) {
                if (css_select_ctx_append_sheet(ctx, sh, CSS_ORIGIN_AUTHOR, "all") == CSS_OK) {
                    author_compiled.push_back(sh);
                    variants_sheet = sh;
                    variants_text  = vcss;
                } else css_stylesheet_destroy(sh);
            }
        }
    }
    if (std::getenv("QUILL_PROF")) {
        auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::fprintf(stderr, "compile: vars %.1fms  element-cascade setup+run+marks %.1fms  libcss sheets %.1fms  (%zu elements)\n", ms(tpv, tpv1),
                     ms(tpv1, tps), ms(tps, std::chrono::steady_clock::now()), cascade_nodes.size());
    }
    // (imports referenced by sheets compiled later are applied through `vars` in make_sheet)
    var_stats = {vars->defined(), vars->substituted, vars->unresolved, vars->skipped_scoped};
    authors_dirty = false;
    cvw = vw;
    cvh = vh;
    return true;
}

void Document::load_stylesheets(const CssLoader& loader, const std::string& base_url,
                                const std::function<void(const std::vector<std::string>&)>& prefetch) {
    Impl& d = *impl_;
    if (!d.doc) return;
    d.loader = loader;
    dom_node* root = nullptr;
    if (dom_document_get_document_element(d.doc, &root) != DOM_NO_ERR || !root) return;
    dom_node_unref(root);
    // collect first: adding sheets does not touch the tree, but keep the walk simple and ordered
    std::vector<std::pair<std::string, std::string>> items; // (css text, base) ; text empty + base = to fetch
    std::vector<dom_node*> nodes;
    nodes.push_back(root);
    d.walk(root, [&](dom_node* n) { nodes.push_back(n); });
    auto link_href = [&](dom_node* n, std::string& abs) {
        std::string rel, href, media;
        if (!get_attr(n, "rel", rel) || !has_token(rel, "stylesheet") || has_token(rel, "alternate")) return false;
        if (!get_attr(n, "href", href) || href.empty()) return false;
        if (get_attr(n, "media", media) && !media.empty() && lower(media) == "print") return false;
        abs = resolve_url(base_url, href);
        return true;
    };
    if (prefetch) {
        std::vector<std::string> urls;
        for (dom_node* n : nodes) {
            std::string abs;
            if (tag_of(n) == "link" && link_href(n, abs)) urls.push_back(abs);
        }
        if (!urls.empty()) prefetch(urls);
    }
    for (dom_node* n : nodes) {
        std::string t = tag_of(n), v;
        if (t == "style") {
            std::string type;
            if (get_attr(n, "type", type) && !type.empty() && lower(type) != "text/css") continue;
            std::string media;
            if (get_attr(n, "media", media) && !media.empty() && lower(media).find("print") == 0) continue;
            add_author_css(text_content(n), base_url);
        } else if (t == "link") {
            std::string abs, text;
            if (!link_href(n, abs)) continue;
            if (loader && loader(abs, text)) add_author_css(text, abs);
        }
    }
}

bool Document::resolve_styles(int vw, int vh) {
    Impl& d = *impl_;
    if (!d.doc) return false;
    d.paint_only = false;
    d.clear_node_caches(); // links may have been visited, nodes added or removed since the last pass
    // zoomed, the page sees a viewport of that many CSS px (media queries, vw / vh units); lengths are then scaled back
    if (d.zoom != 1.f) {
        vw = std::max(1, static_cast<int>(std::lround(vw / d.zoom)));
        vh = std::max(1, static_cast<int>(std::lround(vh / d.zoom)));
    }
    if (!d.authors_dirty && !d.restyle_nodes.empty() && d.boxes && vw == d.vw && vh == d.vh && vw == d.cvw && vh == d.cvh &&
        d.zoom == d.styled_zoom) {
        if (d.restyle_partial(vw, vh)) return true;
        d.restyle_nodes.clear();
        d.authors_dirty = true; // fall back to the full path
    }
    d.restyle_nodes.clear();
    d.styled_zoom = d.zoom;
    // clear previous results
    for (auto& kv : d.styles)
        if (kv.second) css_computed_style_destroy(kv.second);
    d.styles.clear();
    for (auto* m : {&d.before_styles, &d.after_styles}) {
        for (auto& kv : *m) if (kv.second) css_computed_style_destroy(kv.second);
        m->clear();
    }
    d.boxes.reset();
    d.n_elements = 0;
    if (!d.default_style && css_select_default_style(d.ctx, &d.handler, &d, &d.default_style) != CSS_OK) {
        d.error = "could not create the default style";
        return false;
    }
    auto tp0 = std::chrono::steady_clock::now();
    if (!d.compile_authors(vw, vh)) {
        d.error = "document has no root element";
        return false;
    }
    if (std::getenv("QUILL_PROF"))
        std::fprintf(stderr, "compile_authors %.1fms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count());
    css_media media{};
    media.type        = CSS_MEDIA_SCREEN;
    media.width       = INTTOFIX(vw);
    media.height      = INTTOFIX(vh);
    media.orientation = vw >= vh ? CSS_MEDIA_ORIENTATION_LANDSCAPE : CSS_MEDIA_ORIENTATION_PORTRAIT;
    media.color       = INTTOFIX(8);

    dom_node* root = nullptr;
    if (dom_document_get_document_element(d.doc, &root) != DOM_NO_ERR || !root) {
        d.error = "document has no root element";
        return false;
    }
    dom_node_unref(root);
    css_unit_ctx uc = make_unit_ctx(vw, vh, nullptr);
    d.vw = vw; d.vh = vh; d.root_el = root;
    auto ts0 = std::chrono::steady_clock::now();
    d.style_subtree(root, nullptr, uc, media);
    auto ts1 = std::chrono::steady_clock::now();
    d.build_boxes(vw, vh);
    if (std::getenv("QUILL_PROF"))
        std::fprintf(stderr, "select (libcss) %.1fms  build boxes %.1fms\n", std::chrono::duration<double, std::milli>(ts1 - ts0).count(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts1).count());
    return true;
}

void Document::Impl::build_boxes(int vw, int vh) {
    Impl& d = *this;
    dom_node* root = d.root_el;
    boxes.reset();
    auto rs = d.styles.find(root);
    css_unit_ctx buc = make_unit_ctx(vw, vh, rs != d.styles.end() ? rs->second : nullptr);
    StyleLookup lookup = [&d](dom_node* n, int pseudo) -> const css_computed_style* {
        auto& m  = pseudo == 0 ? d.styles : pseudo == CSS_PSEUDO_ELEMENT_BEFORE ? d.before_styles : d.after_styles;
        auto  it = m.find(n);
        return it == m.end() ? nullptr : it->second;
    };
    ExtraLookup extra_lookup = [&d](dom_node* n, int which) -> const Extras* {
        if (!d.cascade) return nullptr;
        auto it = d.node_index.find(n);
        return it == d.node_index.end() ? nullptr : d.cascade->extras(static_cast<size_t>(it->second), which);
    };
    d.boxes = build_box_tree(root, lookup, buc, extra_lookup, d.font_scope, d.styled_zoom);
    d.box_gen++;
    d.laid_out = false;
    d.last_vw = vw;
    d.last_vh = vh;
}

// Re-styles only the subtrees below elements whose state selectors flipped, then the box tree is rebuilt from the styles.
bool Document::Impl::restyle_partial(int vw, int vh) {
    auto rs = styles.find(root_el);
    if (rs == styles.end() || !rs->second) return false;
    std::unordered_set<dom_node*> set(restyle_nodes.begin(), restyle_nodes.end());
    std::vector<dom_node*>        tops;
    for (dom_node* n : set) {
        bool nested = false;
        for (dom_node* p = parent_element(n); p; p = parent_element(p))
            if (set.count(p)) { nested = true; break; }
        if (!nested) tops.push_back(n);
    }
    for (dom_node* n : tops)
        if (n == root_el || !parent_element(n)) return false;
    css_media media{};
    media.type        = CSS_MEDIA_SCREEN;
    media.width       = INTTOFIX(vw);
    media.height      = INTTOFIX(vh);
    media.orientation = vw >= vh ? CSS_MEDIA_ORIENTATION_LANDSCAPE : CSS_MEDIA_ORIENTATION_PORTRAIT;
    media.color       = INTTOFIX(8);
    css_unit_ctx uc = make_unit_ctx(vw, vh, rs->second);
    auto tq0 = std::chrono::steady_clock::now();
    size_t restyled = 0;
    std::vector<dom_node*> restyled_nodes;
    for (dom_node* top : tops) {
        std::vector<dom_node*> sub{top};
        walk(top, [&](dom_node* n) { sub.push_back(n); });
        for (dom_node* n : sub) {
            auto it = styles.find(n);
            if (it != styles.end()) { if (it->second) css_computed_style_destroy(it->second); styles.erase(it); }
            for (auto* m : {&before_styles, &after_styles}) {
                auto jt = m->find(n);
                if (jt != m->end()) { if (jt->second) css_computed_style_destroy(jt->second); m->erase(jt); }
            }
        }
        auto ps = styles.find(parent_element(top));
        style_subtree(top, ps != styles.end() ? ps->second : nullptr, uc, media);
        restyled += sub.size();
        restyled_nodes.insert(restyled_nodes.end(), sub.begin(), sub.end());
    }
    auto tq1 = std::chrono::steady_clock::now();
    restyle_nodes.clear();
    // :hover and friends mostly change colours, backgrounds, underlines, shadows. First try giving just the restyled
    // elements' boxes their new styles (no rebuild, no layout); if any of them changes something layout reads,
    // rebuild the boxes and still keep the old layout when the new tree turns out to lay out the same.
    if (laid_out && boxes) {
        std::unordered_set<const void*> changed(restyled_nodes.begin(), restyled_nodes.end());
        css_unit_ctx buc = make_unit_ctx(vw, vh, styles.count(root_el) ? styles[root_el] : nullptr);
        Impl& d = *this;
        StyleLookup lookup = [&d](dom_node* n, int pseudo) -> const css_computed_style* {
            auto& m  = pseudo == 0 ? d.styles : pseudo == CSS_PSEUDO_ELEMENT_BEFORE ? d.before_styles : d.after_styles;
            auto  it = m.find(n);
            return it == m.end() ? nullptr : it->second;
        };
        ExtraLookup extra_lookup = [&d](dom_node* n, int which) -> const Extras* {
            if (!d.cascade) return nullptr;
            auto it = d.node_index.find(n);
            return it == d.node_index.end() ? nullptr : d.cascade->extras(static_cast<size_t>(it->second), which);
        };
        if (restyle_in_place(*boxes, root_el, changed, lookup, buc, extra_lookup, font_scope, layout_equal, styled_zoom)) {
            paint_only = true;
            paint_only_count++;
            update_ink(*boxes);
            if (std::getenv("QUILL_PROF"))
                std::fprintf(stderr, "restyle: %zu tops, %zu elements %.1fms, in place %.1fms (paint only)\n", tops.size(), restyled,
                             std::chrono::duration<double, std::milli>(tq1 - tq0).count(),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tq1).count());
            return true;
        }
    }
    std::unique_ptr<Box> old = laid_out ? std::move(boxes) : nullptr;
    build_boxes(vw, vh);
    if (old && boxes && same_layout(*old, *boxes, 0)) {
        swap_styles(*old, *boxes);
        boxes      = std::move(old);
        laid_out   = true;
        paint_only = true;
        paint_only_count++;
        update_ink(*boxes);
    }
    if (std::getenv("QUILL_PROF"))
        std::fprintf(stderr, "restyle: %zu tops, %zu elements %.1fms, boxes + compare %.1fms (%s)\n", tops.size(), restyled,
                     std::chrono::duration<double, std::milli>(tq1 - tq0).count(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tq1).count(), paint_only ? "paint only" : "layout");
    return true;
}

namespace {
bool len_eq(const Length& a, const Length& b) { return a.kind == b.kind && a.v == b.v; }
} // namespace

// Everything in Style that layout reads. Paint-only properties (colours, backgrounds, gradients, borders' colour and
// style, shadows, radii, opacity, z-index, visibility, cursor, text-decoration, masks, the transform's linear part)
// are left out on purpose. A new layout-relevant Style field must be added here.
bool Document::Impl::layout_equal(const Style& a, const Style& b) {
    if (a.display != b.display || a.position != b.position || a.float_ != b.float_ || a.clear != b.clear || a.overflow_x != b.overflow_x ||
        a.overflow_y != b.overflow_y || a.box_sizing != b.box_sizing)
        return false;
    if (!len_eq(a.width, b.width) || !len_eq(a.height, b.height) || !len_eq(a.min_width, b.min_width) || !len_eq(a.min_height, b.min_height) ||
        !len_eq(a.max_width, b.max_width) || !len_eq(a.max_height, b.max_height) || a.min_height_auto != b.min_height_auto)
        return false;
    for (int i = 0; i < 4; i++)
        if (!len_eq(a.margin[i], b.margin[i]) || !len_eq(a.padding[i], b.padding[i]) || !len_eq(a.inset[i], b.inset[i]) ||
            a.border_width[i] != b.border_width[i] || a.border_style[i] != b.border_style[i])
            return false;
    if (a.font_size != b.font_size || a.font_weight != b.font_weight || a.italic != b.italic || a.font_family != b.font_family ||
        a.font_scope != b.font_scope || a.line_height != b.line_height || a.letter_spacing != b.letter_spacing || a.word_spacing != b.word_spacing ||
        !len_eq(a.text_indent, b.text_indent) || a.text_align != b.text_align || a.legacy_align != b.legacy_align || a.parent_align != b.parent_align ||
        a.white_space != b.white_space || a.text_transform != b.text_transform || a.vertical_align != b.vertical_align ||
        a.vertical_align_len != b.vertical_align_len || a.rtl != b.rtl || a.list_type != b.list_type || a.list_inside != b.list_inside)
        return false;
    if (a.tr_x_px != b.tr_x_px || a.tr_x_pct != b.tr_x_pct || a.tr_y_px != b.tr_y_px || a.tr_y_pct != b.tr_y_pct || a.appearance_none != b.appearance_none)
        return false;
    if (a.flex_direction != b.flex_direction || a.flex_wrap != b.flex_wrap || a.flex_grow != b.flex_grow || a.flex_shrink != b.flex_shrink ||
        !len_eq(a.flex_basis, b.flex_basis) || a.justify_content != b.justify_content || a.align_items != b.align_items ||
        a.align_self != b.align_self || a.align_content != b.align_content || a.order != b.order || !len_eq(a.column_gap, b.column_gap) ||
        !len_eq(a.row_gap, b.row_gap))
        return false;
    if ((a.grid || b.grid) && a.grid != b.grid) return false;    // (pointer: a restyle makes new ones, so a grid relayouts)
    if ((a.gitem || b.gitem) && a.gitem != b.gitem) return false;
    if (a.border_collapse != b.border_collapse || a.border_spacing_h != b.border_spacing_h || a.border_spacing_v != b.border_spacing_v ||
        a.table_fixed != b.table_fixed || a.caption_bottom != b.caption_bottom)
        return false;
    if ((a.position == Position::Sticky) != (b.position == Position::Sticky)) return false;
    return true;
}

bool Document::Impl::same_layout(const Box& a, const Box& b, int depth) {
    if (depth > 400) return false;
    if (a.kind != b.kind || a.pseudo != b.pseudo || a.node != b.node || a.replaced != b.replaced || a.text != b.text || a.tag != b.tag ||
        a.children.size() != b.children.size() || a.continuation != b.continuation || a.list_ordinal != b.list_ordinal ||
        a.attrs != b.attrs || !a.style != !b.style)
        return false;
    if (a.style && a.style != b.style && !layout_equal(*a.style, *b.style)) return false;
    for (size_t i = 0; i < a.children.size(); i++)
        if (!same_layout(*a.children[i], *b.children[i], depth + 1)) return false;
    return true;
}

void Document::Impl::swap_styles(Box& keep, Box& fresh) {
    keep.style = fresh.style;
    for (size_t i = 0; i < keep.children.size(); i++) swap_styles(*keep.children[i], *fresh.children[i]);
}

std::string Document::dump_tree(bool with_styles) const {
    const Impl& d = *impl_;
    std::string out;
    if (!d.doc) return out;
    dom_node* root = nullptr;
    if (dom_document_get_document_element(d.doc, &root) != DOM_NO_ERR || !root) return out;
    dom_node_unref(root);
    d.dump_node(root, 0, with_styles, out);
    return out;
}

namespace {
// Simple selector: [tag][#id][.class]... (no combinators). Enough for test expectations.
bool simple_match(dom_node* n, const std::string& sel) {
    std::string tag, id;
    std::vector<std::string> classes;
    size_t i = 0;
    auto ident = [&]() { size_t j = i; while (j < sel.size() && sel[j] != '#' && sel[j] != '.') ++j; std::string r = sel.substr(i, j - i); i = j; return r; };
    tag = lower(ident());
    while (i < sel.size()) {
        char c = sel[i++];
        std::string v = ident();
        if (c == '#') id = v; else if (c == '.') classes.push_back(v);
    }
    if (!tag.empty() && tag != "*" && tag != tag_of(n)) return false;
    std::string v;
    if (!id.empty() && !(get_attr(n, "id", v) && v == id)) return false;
    for (auto& c : classes) if (!(get_attr(n, "class", v) && has_token(v, c.c_str()))) return false;
    return true;
}
std::string px(const css_computed_style* s, const css_unit_ctx& uc, css_fixed l, css_unit u) {
    if (u != CSS_UNIT_PX && u != CSS_UNIT_PCT) l = css_unit_len2css_px(s, &uc, l, u), u = CSS_UNIT_PX;
    return num(l) + (u == CSS_UNIT_PCT ? "%" : "px");
}
} // namespace

bool Document::computed_value(const std::string& selector, const std::string& prop, std::string& out) const {
    const Impl& d = *impl_;
    if (!d.doc || !d.root_el) return false;
    dom_node* found = nullptr;
    std::vector<dom_node*> all{d.root_el};
    const_cast<Impl&>(d).walk(d.root_el, [&](dom_node* n) { all.push_back(n); });
    for (dom_node* n : all) if (is_element(n) && simple_match(n, selector)) { found = n; break; }
    if (!found) { out = "<no such element>"; return false; }
    if (prop == "background-size") {
        const Extras* ex = nullptr;
        auto ni = d.node_index.find(found);
        if (d.cascade && ni != d.node_index.end()) ex = d.cascade->extras(static_cast<size_t>(ni->second));
        const SizeSpec sp = ex && ex->has_bgsize ? ex->bg_size : SizeSpec{};
        auto fl = [](const Length& l) { return l.is_auto() ? std::string("auto") : num(FLTTOFIX(l.v)) + (l.is_pct() ? "%" : "px"); };
        out = sp.kind == 1 ? "contain" : sp.kind == 2 ? "cover" : sp.kind == 3 ? fl(sp.w) + " " + fl(sp.h) : "auto";
        return true;
    }
    if (prop == "mask-image") {
        const Extras* ex = nullptr;
        auto ni = d.node_index.find(found);
        if (d.cascade && ni != d.node_index.end()) ex = d.cascade->extras(static_cast<size_t>(ni->second));
        out = ex && ex->has_mask && !ex->mask.url.empty() ? ex->mask.url : "none";
        return true;
    }
    if (prop == "border-radius" || prop == "box-shadow") {
        const Extras* ex = nullptr;
        auto ni = d.node_index.find(found);
        if (d.cascade && ni != d.node_index.end()) ex = d.cascade->extras(static_cast<size_t>(ni->second));
        auto fl = [](const Length& l) { return num(FLTTOFIX(l.v)) + (l.is_pct() ? "%" : "px"); };
        out.clear();
        if (prop == "border-radius") {
            for (int k = 0; k < 4; ++k) {
                Radius r = ex && ex->has_radius ? ex->radius[k] : Radius{};
                out += (k ? " " : "") + fl(r.x) + (r.y.v == r.x.v && r.y.kind == r.x.kind ? "" : "/" + fl(r.y));
            }
        } else if (!ex || ex->shadows.empty()) {
            out = "none";
        } else {
            char buf[96];
            for (auto& sh : ex->shadows) {
                std::snprintf(buf, sizeof buf, "%s%g %g %g %g %08x", sh.inset ? "inset " : "", sh.ox, sh.oy, sh.blur, sh.spread, sh.current ? 0u : sh.color);
                out += (out.empty() ? "" : ", ") + std::string(buf);
            }
        }
        return true;
    }
    auto it = d.styles.find(found);
    if (it == d.styles.end() || !it->second) { out = "<no style>"; return false; }
    const css_computed_style* s = it->second;
    css_unit_ctx uc = make_unit_ctx(d.vw, d.vh, d.styles.count(d.root_el) ? d.styles.at(d.root_el) : nullptr);
    css_fixed l = 0; css_unit u = CSS_UNIT_PX; css_color c = 0;
    auto lenprop = [&](uint8_t r, uint8_t set, uint8_t autov) {
        if (r == set) out = px(s, uc, l, u); else if (r == autov) out = "auto"; else out = "0px";
        return true;
    };
    const std::string& p = prop;
    if (p == "display") out = display_name(css_computed_display(s, found == d.root_el));
    else if (p == "position") out = position_name(css_computed_position(s));
    else if (p == "float") out = float_name(css_computed_float(s));
    else if (p == "color") { css_computed_color(s, &c); out = hexcolor(c); }
    else if (p == "background-color") { css_computed_background_color(s, &c); out = (c >> 24) ? hexcolor(c) : "transparent"; }
    else if (p == "border-top-color" || p == "border-right-color" || p == "border-bottom-color" || p == "border-left-color") {
        uint8_t t = p == "border-top-color" ? css_computed_border_top_color(s, &c) : p == "border-right-color" ? css_computed_border_right_color(s, &c)
                  : p == "border-bottom-color" ? css_computed_border_bottom_color(s, &c) : css_computed_border_left_color(s, &c);
        if (t == CSS_BORDER_COLOR_CURRENT_COLOR) css_computed_color(s, &c);
        out = hexcolor(c);
    }
    else if (p == "font-size") { css_computed_font_size(s, &l, &u); out = px(s, uc, l, u); }
    else if (p == "font-weight") { uint8_t w = css_computed_font_weight(s); out = (w == CSS_FONT_WEIGHT_BOLD || w == CSS_FONT_WEIGHT_700) ? "bold" : (w == CSS_FONT_WEIGHT_NORMAL || w == CSS_FONT_WEIGHT_400) ? "normal" : "other"; }
    else if (p == "font-style") { uint8_t w = css_computed_font_style(s); out = w == CSS_FONT_STYLE_ITALIC ? "italic" : w == CSS_FONT_STYLE_OBLIQUE ? "oblique" : "normal"; }
    else if (p == "visibility") { uint8_t w = css_computed_visibility(s); out = w == CSS_VISIBILITY_HIDDEN ? "hidden" : w == CSS_VISIBILITY_COLLAPSE ? "collapse" : "visible"; }
    else if (p == "overflow-x" || p == "overflow-y") { uint8_t w = p == "overflow-x" ? css_computed_overflow_x(s) : css_computed_overflow_y(s); out = w == CSS_OVERFLOW_HIDDEN ? "hidden" : w == CSS_OVERFLOW_SCROLL ? "scroll" : w == CSS_OVERFLOW_AUTO ? "auto" : "visible"; }
    else if (p == "opacity") { css_computed_opacity(s, &l); out = num(l); }
    else if (p == "width") return lenprop(css_computed_width(s, &l, &u), CSS_WIDTH_SET, CSS_WIDTH_AUTO);
    else if (p == "height") return lenprop(css_computed_height(s, &l, &u), CSS_HEIGHT_SET, CSS_HEIGHT_AUTO);
    else if (p == "margin-top") return lenprop(css_computed_margin_top(s, &l, &u), CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    else if (p == "margin-right") return lenprop(css_computed_margin_right(s, &l, &u), CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    else if (p == "margin-bottom") return lenprop(css_computed_margin_bottom(s, &l, &u), CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    else if (p == "margin-left") return lenprop(css_computed_margin_left(s, &l, &u), CSS_MARGIN_SET, CSS_MARGIN_AUTO);
    else if (p == "padding-top") return lenprop(css_computed_padding_top(s, &l, &u), CSS_PADDING_SET, 0xff);
    else if (p == "padding-right") return lenprop(css_computed_padding_right(s, &l, &u), CSS_PADDING_SET, 0xff);
    else if (p == "padding-bottom") return lenprop(css_computed_padding_bottom(s, &l, &u), CSS_PADDING_SET, 0xff);
    else if (p == "padding-left") return lenprop(css_computed_padding_left(s, &l, &u), CSS_PADDING_SET, 0xff);
    else { out = "<unsupported property>"; return false; }
    return true;
}

namespace {
std::string squash_ws(const std::string& in) {
    std::string out;
    bool        sp = false;
    for (char c : in) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') sp = true;
        else {
            if (sp && !out.empty()) out += ' ';
            sp = false;
            out += c;
        }
    }
    return out;
}
int attr_int(dom_node* n, const char* name, int def) {
    std::string v;
    if (!get_attr(n, name, v) || v.empty()) return def;
    char* e = nullptr;
    long  r = std::strtol(v.c_str(), &e, 10);
    return e == v.c_str() ? def : static_cast<int>(r);
}
void collect_options(dom_node* sel, std::vector<ControlInfo::Opt>& out) {
    for (dom_node* c = first_child(sel); c; c = next_sibling(c)) {
        if (!is_element(c)) continue;
        std::string t = tag_of(c);
        if (t == "option") {
            ControlInfo::Opt o;
            o.label = squash_ws(text_content(c));
            std::string v;
            o.value = get_attr(c, "value", v) ? v : o.label;
            o.selected = get_attr(c, "selected", v);
            out.push_back(std::move(o));
        } else if (t == "optgroup") collect_options(c, out);
    }
}
} // namespace

std::string Document::title() const {
    const Impl& d = *impl_;
    if (!d.doc || !d.root_el) return {};
    std::string out;
    const_cast<Impl&>(d).walk(d.root_el, [&](dom_node* n) {
        if (out.empty() && tag_of(n) == "title") out = squash_ws(text_content(n));
    });
    return out;
}

namespace {
// The <html> element, also before resolve_styles() has run (root_el is only set there).
dom_node* document_root(const Document::Impl& d, dom_document* doc, dom_node* root_el) {
    if (root_el) return root_el;
    dom_element* html = nullptr;
    if (!doc || dom_document_get_document_element(doc, &html) != DOM_NO_ERR || !html) return nullptr;
    dom_node* r = reinterpret_cast<dom_node*>(html);
    dom_node_unref(r); // borrowed: the document owns it
    (void)d;
    return r;
}
} // namespace

std::string Document::base_href() const {
    const Impl& d = *impl_;
    dom_node*   root = document_root(d, d.doc, d.root_el);
    if (!root) return {};
    std::string out;
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        std::string v;
        if (out.empty() && tag_of(n) == "base" && get_attr(n, "href", v)) out = v;
    });
    return out;
}

bool Document::meta_refresh(double& delay, std::string& url) const {
    const Impl& d = *impl_;
    dom_node*   root = document_root(d, d.doc, d.root_el);
    if (!root) return false;
    bool found = false;
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        std::string he, content;
        if (found || tag_of(n) != "meta" || !get_attr(n, "http-equiv", he) || lower(he) != "refresh" || !get_attr(n, "content", content)) return;
        // HTML "shared declarative refresh steps": <delay>[.fraction] [;,] [url=]['"]<url>['"]
        size_t i = 0;
        auto   ws = [&]() { while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) ++i; };
        ws();
        size_t start = i;
        while (i < content.size() && std::isdigit(static_cast<unsigned char>(content[i]))) ++i;
        if (i == start && !(i < content.size() && content[i] == '.')) return;
        double t = i > start ? std::atof(content.substr(start, i - start).c_str()) : 0;
        while (i < content.size() && (std::isdigit(static_cast<unsigned char>(content[i])) || content[i] == '.')) ++i;
        ws();
        std::string u;
        if (i < content.size() && (content[i] == ';' || content[i] == ',')) {
            ++i;
            ws();
            if (content.size() - i >= 3 && lower(content.substr(i, 3)) == "url") {
                size_t j = i + 3;
                while (j < content.size() && std::isspace(static_cast<unsigned char>(content[j]))) ++j;
                if (j < content.size() && content[j] == '=') {
                    i = j + 1;
                    ws();
                }
            }
            char q = i < content.size() && (content[i] == '"' || content[i] == '\'') ? content[i] : 0;
            if (q) ++i;
            u = content.substr(i);
            if (q) {
                size_t e = u.find(q);
                if (e != std::string::npos) u.erase(e);
            }
            while (!u.empty() && std::isspace(static_cast<unsigned char>(u.back()))) u.pop_back();
        } else if (i < content.size()) {
            return; // junk after the delay
        }
        delay = t;
        url   = u;
        found = true;
    });
    return found;
}

void Document::set_font_scope(uint64_t scope) { impl_->font_scope = scope; }
bool Document::restyle_was_paint_only() const { return impl_->paint_only; }
size_t Document::paint_only_restyles() const { return impl_->paint_only_count; }

namespace {
// "U+0025-00FF, u+4??, U+0131" -> ranges
std::vector<std::pair<uint32_t, uint32_t>> unicode_ranges(const std::string& v) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (std::string t : lex::split_commas(v)) {
        t = lex::lower(lex::trim(t));
        if (t.compare(0, 2, "u+") != 0) continue;
        t = t.substr(2);
        std::string a = t, b;
        size_t      dash = t.find('-');
        if (dash != std::string::npos) { a = t.substr(0, dash); b = t.substr(dash + 1); }
        auto hexv = [](const std::string& h, char wild, uint32_t& out_v) {
            if (h.empty() || h.size() > 6) return false;
            uint32_t r = 0;
            for (char c : h) {
                int d = c == '?' ? (wild == '0' ? 0 : 15) : (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (d < 0) return false;
                r = r * 16 + static_cast<uint32_t>(d);
            }
            out_v = r;
            return true;
        };
        uint32_t lo, hi;
        if (b.empty()) {
            if (!hexv(a, '0', lo) || !hexv(a, 'f', hi)) continue;
        } else if (!hexv(a, '0', lo) || !hexv(b, 'f', hi)) {
            continue;
        }
        if (lo <= hi && lo <= 0x10FFFF) out.push_back({lo, std::min<uint32_t>(hi, 0x10FFFF)});
        if (out.size() > 200) break;
    }
    return out;
}

int font_weight_value(const std::string& t) {
    std::string l = lex::lower(lex::trim(t));
    if (l == "normal" || l.empty()) return 400;
    if (l == "bold") return 700;
    if (l == "bolder") return 700;
    if (l == "lighter") return 300;
    int v = std::atoi(l.c_str());
    return v >= 1 && v <= 1000 ? v : 400;
}

std::string unquote(std::string v) {
    v = lex::trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
}

void collect_font_faces(const std::string& s, size_t b, size_t e, const std::string& base, std::vector<FontFaceRule>& out, int depth) {
    if (depth > 6 || out.size() > 400) return;
    for (const lex::Rule& r : lex::parse_rules(s, b, e)) {
        if (!r.at || !r.has_block) continue;
        if (r.name == "media" || r.name == "supports" || r.name == "layer" || r.name == "document" || r.name == "-moz-document") {
            collect_font_faces(s, r.bb, r.be, base, out, depth + 1);
            continue;
        }
        if (r.name != "font-face") continue;
        FontFaceRule f;
        for (const lex::Decl& d : lex::parse_decls(s, r.bb, r.be)) {
            std::string n = lex::lower(d.name);
            if (n == "font-family") {
                f.family = lex::lower(unquote(d.value));
            } else if (n == "font-weight") {
                std::string v = lex::trim(d.value);
                size_t      sp = v.find(' ');
                f.weight_lo    = font_weight_value(sp == std::string::npos ? v : v.substr(0, sp));
                f.weight_hi    = sp == std::string::npos ? f.weight_lo : font_weight_value(v.substr(sp + 1));
                if (f.weight_hi < f.weight_lo) std::swap(f.weight_lo, f.weight_hi);
            } else if (n == "font-style") {
                std::string v = lex::lower(lex::trim(d.value));
                f.italic      = v.compare(0, 6, "italic") == 0 || v.compare(0, 7, "oblique") == 0;
            } else if (n == "unicode-range") {
                f.ranges = unicode_ranges(d.value);
            } else if (n == "font-display") {
                std::string v = lex::lower(lex::trim(d.value));
                f.display     = v == "swap" ? 1 : v == "fallback" ? 2 : v == "optional" ? 3 : v == "block" ? 4 : 0;
            } else if (n == "src") {
                for (const std::string& item : lex::split_commas(d.value)) {
                    std::string l = lex::lower(item);
                    size_t      u = l.find("url(");
                    if (u == std::string::npos) continue; // local(...): Quill doesn't look fonts up by name
                    size_t      close = item.find(')', u + 4);
                    if (close == std::string::npos) continue;
                    std::string url = unquote(item.substr(u + 4, close - u - 4));
                    std::string fmt;
                    size_t      fo = l.find("format(", close);
                    if (fo != std::string::npos) {
                        size_t fc = l.find(')', fo);
                        if (fc != std::string::npos) fmt = lex::lower(unquote(l.substr(fo + 7, fc - fo - 7)));
                        size_t comma = fmt.find(',');
                        if (comma != std::string::npos) fmt = unquote(fmt.substr(0, comma));
                    }
                    if (url.empty() || url.size() > 4096) continue;
                    if (lex::lower(url).compare(0, 5, "data:") != 0) url = resolve_url(base, url);
                    f.src.push_back({url, fmt});
                    if (f.src.size() >= 8) break;
                }
            }
        }
        if (!f.family.empty() && !f.src.empty()) out.push_back(std::move(f));
    }
}

bool generic_family(const std::string& n) {
    return n == "serif" || n == "sans-serif" || n == "monospace" || n == "cursive" || n == "fantasy" || n == "system-ui" ||
           n == "ui-serif" || n == "ui-sans-serif" || n == "ui-monospace" || n == "emoji" || n == "math";
}
} // namespace

std::vector<FontFaceRule> Document::font_faces_in_use() const {
    const Impl& d = *impl_;
    std::vector<FontFaceRule> all;
    for (auto& t : d.sheet_texts) {
        if (lex::lower(t.first).find("@font-face") == std::string::npos) continue;
        std::string css = lex::strip_comments(t.first);
        collect_font_faces(css, 0, css.size(), t.second, all, 0);
    }
    if (all.empty() || !d.boxes) return {};
    std::set<std::string> families;
    for (auto& f : all) families.insert(f.family);
    // which of those families text asks for, and with which characters
    std::map<std::string, std::set<uint32_t>> used;
    std::function<void(const Box&)> walk = [&](const Box& b) {
        if (b.kind == Box::Kind::Text && b.style && !b.text.empty()) {
            for (const std::string& raw : b.style->font_family) {
                std::string n = lex::lower(unquote(raw));
                if (generic_family(n)) break;
                if (!families.count(n)) continue;
                auto& cps = used[n];
                for (size_t i = 0; i < b.text.size() && cps.size() < 4000;) {
                    unsigned char c = static_cast<unsigned char>(b.text[i]);
                    uint32_t      cp;
                    int           len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    if (i + len > b.text.size()) break;
                    if (len == 1) cp = c;
                    else if (len == 2) cp = ((c & 0x1Fu) << 6) | (b.text[i + 1] & 0x3F);
                    else if (len == 3) cp = ((c & 0x0Fu) << 12) | ((b.text[i + 1] & 0x3Fu) << 6) | (b.text[i + 2] & 0x3F);
                    else cp = ((c & 0x07u) << 18) | ((b.text[i + 1] & 0x3Fu) << 12) | ((b.text[i + 2] & 0x3Fu) << 6) | (b.text[i + 3] & 0x3F);
                    i += static_cast<size_t>(len);
                    if (cp > 0x20) cps.insert(cp);
                }
                break; // the first web family in the list is the one that draws
            }
        }
        for (auto& c : b.children) walk(*c);
        for (auto& l : b.lines)
            for (auto& fr : l.frags)
                if (fr.box && fr.box->parent != &b) walk(*fr.box);
    };
    walk(*d.boxes);
    std::vector<FontFaceRule> out;
    for (auto& f : all) {
        auto it = used.find(f.family);
        if (it == used.end()) continue;
        bool hit = f.ranges.empty();
        for (uint32_t cp : it->second) {
            if (hit) break;
            for (auto& r : f.ranges)
                if (cp >= r.first && cp <= r.second) { hit = true; break; }
        }
        if (hit) out.push_back(f);
        if (out.size() >= 64) break;
    }
    return out;
}

bool MapArea::contains(float x, float y) const {
    const auto& c = coords;
    switch (shape) {
    case Shape::Default: return true;
    case Shape::Rect:
        if (c.size() < 4) return false;
        return x >= std::min(c[0], c[2]) && x < std::max(c[0], c[2]) && y >= std::min(c[1], c[3]) && y < std::max(c[1], c[3]);
    case Shape::Circle:
        if (c.size() < 3) return false;
        return (x - c[0]) * (x - c[0]) + (y - c[1]) * (y - c[1]) <= c[2] * c[2];
    case Shape::Poly: {
        size_t n = c.size() / 2;
        if (n < 3) return false;
        bool in = false; // even-odd rule
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            float xi = c[2 * i], yi = c[2 * i + 1], xj = c[2 * j], yj = c[2 * j + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    }
    }
    return false;
}

std::vector<std::pair<std::string, std::vector<MapArea>>> Document::image_maps() const {
    const Impl& d = *impl_;
    std::vector<std::pair<std::string, std::vector<MapArea>>> out;
    dom_node* root = document_root(d, d.doc, d.root_el);
    if (!root) return out;
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        if (tag_of(n) != "map" || out.size() >= 200) return;
        std::vector<MapArea> areas;
        const_cast<Impl&>(d).walk(n, [&](dom_node* a) {
            if (tag_of(a) != "area" || areas.size() >= 500) return;
            MapArea    m;
            std::string v;
            get_attr(a, "shape", v);
            v = lower(v);
            if (v == "circle" || v == "circ") m.shape = MapArea::Shape::Circle;
            else if (v == "poly" || v == "polygon") m.shape = MapArea::Shape::Poly;
            else if (v == "default") m.shape = MapArea::Shape::Default;
            std::string cs;
            get_attr(a, "coords", cs);
            for (char& ch : cs) if (ch == ';' || ch == ' ' || ch == '\t') ch = ',';
            size_t pos = 0;
            while (pos < cs.size() && m.coords.size() < 2000) {
                size_t comma = cs.find(',', pos);
                if (comma == std::string::npos) comma = cs.size();
                std::string t = cs.substr(pos, comma - pos);
                pos           = comma + 1;
                if (t.empty()) continue;
                m.coords.push_back(std::strtof(t.c_str(), nullptr));
            }
            m.nohref = get_attr(a, "nohref", v) || !get_attr(a, "href", m.href);
            get_attr(a, "target", m.target);
            get_attr(a, "alt", m.alt);
            areas.push_back(std::move(m));
        });
        std::string name, id;
        bool has_name = get_attr(n, "name", name), has_id = get_attr(n, "id", id);
        if (has_name && !name.empty()) out.push_back({name, areas});
        if (has_id && !id.empty() && id != name) out.push_back({id, areas});
    });
    return out;
}

std::string Document::icon_href() const {
    const Impl& d = *impl_;
    dom_node*   root = document_root(d, d.doc, d.root_el);
    if (!root) return {};
    std::string best;
    int         best_score = -1;
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        if (tag_of(n) != "link") return;
        std::string rel, href, sizes, type;
        if (!get_attr(n, "rel", rel) || !get_attr(n, "href", href) || lex::trim(href).empty()) return;
        rel = " " + lower(rel) + " ";
        for (char& c : rel) if (c == '\t' || c == '\n') c = ' ';
        int score;
        if (rel.find(" icon ") != std::string::npos) score = 100;
        else if (rel.find(" apple-touch-icon ") != std::string::npos || rel.find(" apple-touch-icon-precomposed ") != std::string::npos) score = 10;
        else return;
        get_attr(n, "sizes", sizes);
        get_attr(n, "type", type);
        sizes = lower(sizes);
        type  = lower(type);
        if (sizes.find("32x32") != std::string::npos) score += 20;
        else if (sizes.find("16x16") != std::string::npos) score += 15;
        else if (sizes.find("any") != std::string::npos) score += 12;
        else if (sizes.find("48x48") != std::string::npos) score += 10;
        else if (sizes.find("64x64") != std::string::npos || sizes.find("96x96") != std::string::npos) score += 8;
        else if (sizes.empty()) score += 9;
        else score += 2;
        if (type == "image/svg+xml") score += 3;
        if (score > best_score) { best_score = score; best = lex::trim(href); }
    });
    return best;
}

std::string Document::base_target() const {
    const Impl& d = *impl_;
    dom_node*   root = document_root(d, d.doc, d.root_el);
    if (!root) return {};
    std::string out;
    bool        found = false;
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        std::string v;
        if (!found && tag_of(n) == "base" && get_attr(n, "target", v)) { out = v; found = true; }
    });
    return out;
}

uint64_t Document::box_generation() const { return impl_->box_gen; }
void  Document::set_zoom(float z) { impl_->zoom = std::max(0.25f, std::min(5.f, z)); }
float Document::zoom() const { return impl_->zoom; }

std::string Document::meta_named(const std::string& name) const {
    const Impl& d = *impl_;
    dom_node*   root = document_root(d, d.doc, d.root_el);
    if (!root) return {};
    std::string out, want = lower(name);
    const_cast<Impl&>(d).walk(root, [&](dom_node* n) {
        std::string nm, c;
        if (tag_of(n) == "meta" && get_attr(n, "name", nm) && lex::lower(lex::trim(nm)) == want && get_attr(n, "content", c)) out = c;
    });
    return out;
}

namespace {
int frame_px_attr(dom_node* n, const char* name) {
    std::string v;
    if (!get_attr(n, name, v)) return -1;
    char* end = nullptr;
    long  x   = std::strtol(v.c_str(), &end, 10);
    return end == v.c_str() || x < 0 ? -1 : static_cast<int>(std::min(x, 100L));
}
bool frame_off(dom_node* n) {
    std::string v;
    return get_attr(n, "frameborder", v) && (lower(v) == "0" || lower(v) == "no");
}
void read_frameset(dom_node* set, FrameNode& out, int depth) {
    out.is_set = true;
    get_attr(set, "rows", out.rows);
    get_attr(set, "cols", out.cols);
    out.border = frame_px_attr(set, "border");
    if (out.border < 0) out.border = frame_px_attr(set, "framespacing");
    out.frameborder = !frame_off(set);
    for (dom_node* c = first_child(set); c && out.children.size() < 64; c = next_sibling(c)) {
        if (!is_element(c)) continue;
        std::string t = tag_of(c);
        if (t == "frameset" && depth < 8) {
            FrameNode sub;
            read_frameset(c, sub, depth + 1);
            out.children.push_back(std::move(sub));
        } else if (t == "frame") {
            FrameNode f;
            get_attr(c, "src", f.src);
            get_attr(c, "name", f.name);
            f.frameborder = !frame_off(c);
            std::string sc;
            if (get_attr(c, "scrolling", sc)) {
                sc = lower(sc);
                f.scrolling = sc == "no" ? 2 : sc == "yes" ? 1 : 0;
            }
            f.marginwidth  = frame_px_attr(c, "marginwidth");
            f.marginheight = frame_px_attr(c, "marginheight");
            out.children.push_back(std::move(f));
        }
    }
}
} // namespace

bool Document::frameset(FrameNode& out) const {
    const Impl& d = *impl_;
    if (!d.doc) return false;
    dom_element* html = nullptr;
    if (dom_document_get_document_element(d.doc, &html) != DOM_NO_ERR || !html) return false;
    dom_node* root = reinterpret_cast<dom_node*>(html);
    dom_node_unref(root); // borrowed: the document owns it
    for (dom_node* c = first_child(root); c; c = next_sibling(c)) {
        if (!is_element(c)) continue;
        std::string t = tag_of(c);
        if (t == "body") return false;
        if (t == "frameset") {
            out = FrameNode();
            read_frameset(c, out, 0);
            return true;
        }
    }
    return false;
}

std::vector<FormInfo> Document::forms() const {
    const Impl&           d = *impl_;
    std::vector<FormInfo> out;
    if (!d.doc || !d.root_el) return out;
    const_cast<Impl&>(d).walk(d.root_el, [&](dom_node* n) {
        if (tag_of(n) != "form") return;
        FormInfo f;
        f.node = n;
        get_attr(n, "action", f.action);
        get_attr(n, "method", f.method);
        get_attr(n, "enctype", f.enctype);
        get_attr(n, "target", f.target);
        out.push_back(std::move(f));
    });
    return out;
}

std::vector<ControlInfo> Document::controls() const {
    const Impl&              d = *impl_;
    std::vector<ControlInfo> out;
    if (!d.doc || !d.root_el) return out;
    const_cast<Impl&>(d).walk(d.root_el, [&](dom_node* n) {
        std::string tag = tag_of(n);
        if (tag != "input" && tag != "textarea" && tag != "select" && tag != "button") return;
        ControlInfo c;
        c.node = n;
        c.tag  = tag;
        for (dom_node* p = parent_element(n); p; p = parent_element(p))
            if (tag_of(p) == "form") { c.form = p; break; }
        std::string v;
        if (get_attr(n, "type", v)) c.type = lower(v);
        get_attr(n, "name", c.name);
        get_attr(n, "id", c.id);
        c.has_value = get_attr(n, "value", c.value);
        get_attr(n, "placeholder", c.placeholder);
        c.checked  = get_attr(n, "checked", v);
        c.disabled = get_attr(n, "disabled", v);
        c.readonly = get_attr(n, "readonly", v);
        c.size      = std::max(1, attr_int(n, "size", 20));
        c.rows      = std::max(1, attr_int(n, "rows", 2));
        c.cols      = std::max(1, attr_int(n, "cols", 20));
        c.maxlength = std::max(0, attr_int(n, "maxlength", 0));
        c.multiple  = get_attr(n, "multiple", v);
        c.autofocus = get_attr(n, "autofocus", v);
        c.list_size = std::max(0, attr_int(n, "size", 0));
        get_attr(n, "accept", c.accept);
        if (tag == "textarea") {
            c.text = text_content(n);
            if (!c.text.empty() && c.text[0] == '\n') c.text.erase(0, 1);
        } else if (tag == "select") collect_options(n, c.options);
        out.push_back(std::move(c));
    });
    return out;
}

bool Document::has_state_rules() const { return impl_->has_hover; }

bool Document::Impl::apply_state() {
    if (!cascade) return false; // not styled yet: the next resolve picks the state up
    std::vector<unsigned char> flags(cascade_nodes.size(), 0);
    for (size_t i = 0; i < cascade_nodes.size(); ++i) {
        dom_node* n = cascade_nodes[i];
        flags[i] = static_cast<unsigned char>((hover_set.count(n) ? 1 : 0) | (active_set.count(n) ? 2 : 0) | (n == focus_leaf ? 4 : 0) |
                                              (focus_within_set.count(n) ? 8 : 0));
    }
    std::vector<int> affected;
    int r = cascade->set_state_flags(flags, affected);
    if (r & 2) {
        // var()/extra rules changed: recompute the per-element results of those elements and everything below them,
        // refresh their marker classes and swap the generated variants sheet if it grew
        std::vector<int> all = cascade->recompute(affected);
        for (int i : all) {
            dom_node* n = cascade_nodes[static_cast<size_t>(i)];
            std::string v, kept, cur;
            get_attr(n, "class", v);
            v += ' ';
            for (char ch : v) {
                if (std::isspace(static_cast<unsigned char>(ch))) {
                    if (!cur.empty() && !(cur.size() > 3 && cur.compare(0, 3, "qvx") == 0 && std::isdigit(static_cast<unsigned char>(cur[3])))) kept += (kept.empty() ? "" : " ") + cur;
                    cur.clear();
                } else cur += ch;
            }
            for (int id : cascade->marks(static_cast<size_t>(i))) kept += (kept.empty() ? "" : " ") + std::string("qvx") + std::to_string(id);
            set_attr(n, "class", kept);
            auto nd = node_data.find(n); // libcss caches ancestor class data per node
            if (nd != node_data.end()) {
                void* data = nd->second;
                css_libcss_node_data_handler(&handler, CSS_NODE_DELETED, this, n, nullptr, data);
                node_data.erase(n);
            }
        }
        clear_class_cache();
        std::string vcss = cascade->variants_css();
        if (vcss != variants_text) {
            if (variants_sheet) {
                css_select_ctx_remove_sheet(ctx, variants_sheet);
                author_compiled.erase(std::remove(author_compiled.begin(), author_compiled.end(), variants_sheet), author_compiled.end());
                drop_sheet(variants_sheet);
                variants_sheet = nullptr;
            }
            variants_text = vcss;
            note_attr_refs(vcss);
            if (!vcss.empty()) {
                css_stylesheet* sh = make_sheet(vcss, "", false, 0);
                if (sh) {
                    if (css_select_ctx_append_sheet(ctx, sh, CSS_ORIGIN_AUTHOR, "all") == CSS_OK) { author_compiled.push_back(sh); variants_sheet = sh; }
                    else css_stylesheet_destroy(sh);
                }
            }
        }
    }
    if (r & 1)
        for (int i : affected) restyle_nodes.push_back(cascade_nodes[static_cast<size_t>(i)]);
    return r != 0;
}

bool Document::set_hover(const void* node) {
    Impl& d = *impl_;
    dom_node* leaf = static_cast<dom_node*>(const_cast<void*>(node));
    if (leaf == d.hover_leaf) return false;
    d.hover_leaf = leaf;
    d.hover_set.clear();
    for (dom_node* p = leaf; p; p = parent_element(p)) d.hover_set.insert(p);
    return d.apply_state();
}

bool Document::set_active(const void* node) {
    Impl& d = *impl_;
    dom_node* leaf = static_cast<dom_node*>(const_cast<void*>(node));
    if (leaf == d.active_leaf) return false;
    d.active_leaf = leaf;
    d.active_set.clear();
    for (dom_node* p = leaf; p; p = parent_element(p)) d.active_set.insert(p);
    return d.apply_state();
}

bool Document::set_focus(const void* node) {
    Impl& d = *impl_;
    dom_node* leaf = static_cast<dom_node*>(const_cast<void*>(node));
    if (leaf == d.focus_leaf) return false;
    d.focus_leaf = leaf;
    d.focus_within_set.clear();
    for (dom_node* p = leaf; p; p = parent_element(p)) d.focus_within_set.insert(p);
    bool differs = d.apply_state();
    if (d.has_fw) d.authors_dirty = true; // the .qv-fw marker classes follow the focus chain
    return differs || d.has_fw;
}

const Box* Document::box_root() const { return impl_->boxes.get(); }

void Document::set_text_measurer(TextMeasurer* m) { impl_->measurer = m; }
void Document::set_image_sizer(ImageSizer f) { impl_->image_sizer = std::move(f); }
void Document::set_image_broken(ImageBroken f) { impl_->image_broken = std::move(f); }
void Document::set_image_request(std::function<void(const std::string&)> f) { impl_->image_request = std::move(f); }
void Document::restyle_all() { impl_->restyle_nodes.clear(); }

bool Document::toggle_details(const void* node) {
    for (dom_node* p = static_cast<dom_node*>(const_cast<void*>(node)); p; p = parent_element(p)) {
        std::string t = tag_of(p);
        if (t == "a" || t == "input" || t == "button" || t == "select" || t == "textarea" || t == "label") return false; // they act
        if (t != "summary") continue;
        dom_node* det = parent_element(p);
        if (!det || tag_of(det) != "details") return false;
        dom_node* first = nullptr;
        for (dom_node* c = first_child(det); c; c = next_sibling(c))
            if (is_element(c) && tag_of(c) == "summary") { first = c; break; }
        if (first != p) return false; // only the first summary toggles
        std::string v;
        if (get_attr(det, "open", v)) remove_attr(det, "open");
        else set_attr(det, "open", "");
        restyle_all();
        return true;
    }
    return false;
}
void Document::set_link_visited(std::function<bool(const std::string&)> f) {
    impl_->link_visited = std::move(f);
    impl_->link_cache.clear();
}

namespace {
void reset_layout(Box& b) {
    b.min_content = b.max_content = -1;
    b.skipped = false;
    b.x = b.y = b.w = b.h = 0;
    for (auto& c : b.children) reset_layout(*c);
}
} // namespace

namespace {
// CSS Overflow: the root element's overflow (or, if that is visible, the body's) belongs to the viewport, and the
// element itself doesn't clip. So a `body { overflow-x: hidden }` page still scrolls as a whole and paints below the
// fold. (Quill lets the viewport scroll whatever the propagated value says: with no scripts, a page that hides its
// scroll bar until some script runs would otherwise be stuck.)
void propagate_viewport_overflow(Box& root) {
    auto visible = [](const Box& b) { return !b.style || (b.style->overflow_x == Overflow::Visible && b.style->overflow_y == Overflow::Visible); };
    if (root.style && root.style->overflow_to_viewport) return; // done (layout runs again on the same boxes)
    Box* target = &root;
    if (visible(root)) {
        target = nullptr;
        for (auto& c : root.children)
            if (c->kind == Box::Kind::Element && c->tag == "body") {
                if (c->style && c->style->overflow_to_viewport) return;
                target = c.get();
                break;
            }
    }
    if (!target || visible(*target)) return;
    auto st        = std::make_shared<Style>(*target->style);
    st->overflow_x = st->overflow_y = Overflow::Visible;
    st->overflow_to_viewport = true;
    target->style  = st;
}
} // namespace

bool Document::layout(int viewport_w, int viewport_h) {
    Impl& d = *impl_;
    if (!d.boxes) return false;
    propagate_viewport_overflow(*d.boxes);
    reset_layout(*d.boxes);
    d.laid_out = true;
    LayoutEnv env;
    env.text       = d.measurer;
    env.image_size = d.image_sizer;
    env.image_broken = d.image_broken;
    env.image_request = d.image_request;
    env.viewport_w = static_cast<float>(viewport_w);
    env.viewport_h = static_cast<float>(viewport_h);
    env.zoom       = d.styled_zoom;
    layout_document(*d.boxes, env);
    return true;
}

namespace {
struct SimpleSel {
    std::string tag, id;
    std::vector<std::string> classes;
    static SimpleSel parse(const std::string& sel) {
        SimpleSel r;
        size_t i = 0;
        auto ident = [&]() { size_t j = i; while (j < sel.size() && sel[j] != '#' && sel[j] != '.') ++j; std::string v = sel.substr(i, j - i); i = j; return v; };
        r.tag = lower(ident());
        while (i < sel.size()) {
            char c = sel[i++];
            std::string v = ident();
            if (c == '#') r.id = v; else if (c == '.') r.classes.push_back(v);
        }
        return r;
    }
    bool match(const Box& b) const {
        if (b.kind != Box::Kind::Element) return false;
        if (!tag.empty() && tag != "*" && tag != b.tag) return false;
        if (!id.empty()) { auto* v = b.attr("id"); if (!v || *v != id) return false; }
        for (auto& c : classes) { auto* v = b.attr("class"); if (!v || !has_token(*v, c.c_str())) return false; }
        return true;
    }
};
const Box* find_box(const Box& b, const SimpleSel& s) {
    if (s.match(b)) return &b;
    for (auto& c : b.children)
        if (const Box* r = find_box(*c, s)) return r;
    return nullptr;
}
} // namespace

bool Document::box_rect(const std::string& selector, Rect& out) const {
    const Box* root = impl_->boxes.get();
    if (!root) return false;
    const Box* b = find_box(*root, SimpleSel::parse(selector));
    if (!b) return false;
    out = {b->x, b->y, b->w, b->h};
    return true;
}

bool Document::line_rect(const std::string& selector, size_t index, Rect& out) const {
    const Box* root = impl_->boxes.get();
    if (!root) return false;
    const Box* b = find_box(*root, SimpleSel::parse(selector));
    if (!b || index >= b->lines.size()) return false;
    const Line& l = b->lines[index];
    out = {l.x, l.y, l.w, l.h};
    return true;
}

std::string Document::dump_boxes(const std::string& selector) const {
    const Box* root = impl_->boxes.get();
    if (!root) return {};
    const Box* b = selector.empty() ? root : find_box(*root, SimpleSel::parse(selector));
    return b ? dump_box(*b) : std::string("<no such box>\n");
}

size_t Document::element_count() const { return impl_->n_elements; }
size_t Document::stylesheet_count() const { return impl_->sheets.size() + impl_->pending.size(); }
VarStats Document::var_stats() const { return impl_->var_stats; }
const std::string& Document::last_error() const { return impl_->error; }

} // namespace quill::engine
