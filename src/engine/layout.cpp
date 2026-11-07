// Block and inline layout. See layout.h.
#include "layout.h"

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>

namespace quill::engine {
namespace {

size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

class FakeMeasurer : public TextMeasurer {
public:
    float width(const Style& st, const std::string& t) override { return static_cast<float>(utf8_len(t)) * st.font_size * 0.5f; }
    FontMetrics metrics(const Style& st) override { return {st.font_size * 0.8f, st.font_size * 0.2f, 0}; }
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Margin collapsing: positive and negative margins collapse separately.
struct Margin {
    float pos = 0, neg = 0;
    void add(float m) {
        if (m >= 0) pos = std::max(pos, m);
        else neg = std::min(neg, m);
    }
    float value() const { return pos + neg; }
};
Margin joined(Margin a, float m) { a.add(m); return a; }

struct BlockOut {
    float mt = 0, mb = 0; // margins that escape to the parent (collapsed with children where allowed)
    bool  empty_through = false;
};

bool establishes_bfc(const Box& b, bool is_root) {
    if (is_root || !b.style) return true;
    const Style& s = *b.style;
    if (s.overflow_x != Overflow::Visible || s.overflow_y != Overflow::Visible) return true;
    if (s.float_ != Float::None || s.position == Position::Absolute || s.position == Position::Fixed) return true;
    switch (s.display) {
    case Display::InlineBlock: case Display::InlineTable: case Display::InlineFlex: case Display::TableCell: case Display::Flex: case Display::Grid: case Display::InlineGrid:
    case Display::Table: case Display::TableCaption: return true;
    default: return false;
    }
}

struct Atom {
    enum class K : uint8_t { Word, Space, Open, Close, Atomic, Break, Float, Abs, Nop } k = K::Word;
    Box*        box = nullptr; // Word/Space: the text box; Open/Close: the inline element; Atomic: the box
    std::string text;
    float       w = 0;
    float       shift = 0;     // vertical-align shift (positive = down), accumulated over enclosing inlines
    bool        collapsible = true;
    bool        first_seg = false; // Open: carries the left edge
    float       up = 0, down = 0;  // Atomic: extents above/below the baseline (margin box)
};

// ---- floats: one context per block formatting context; rectangles are margin boxes in BFC coordinates.
struct FloatRec {
    Box*  box;
    float x0, y0, x1, y1;
    bool  left;
};
struct Ctx {
    std::vector<FloatRec> f;
    float                 last_top = -std::numeric_limits<float>::infinity();

    // Narrows [lo, hi] by the floats overlapping the vertical band [y0, y1). Returns true if narrowed.
    bool range(float y0, float y1, float& lo, float& hi) const {
        bool narrowed = false;
        for (auto& r : f) {
            if (r.y1 <= y0 || r.y0 >= std::max(y1, y0 + 0.01f)) continue;
            if (r.left) { if (r.x1 > lo) { lo = r.x1; narrowed = true; } }
            else if (r.x0 < hi) { hi = r.x0; narrowed = true; }
        }
        return narrowed;
    }
    float next_edge(float y) const {
        float best = -1;
        for (auto& r : f)
            if (r.y1 > y + 0.001f && (best < 0 || r.y1 < best)) best = r.y1;
        return best;
    }
    float clear_y(Clear c) const {
        float y = -std::numeric_limits<float>::infinity();
        for (auto& r : f) {
            if ((c == Clear::Left && !r.left) || (c == Clear::Right && r.left)) continue;
            y = std::max(y, r.y1);
        }
        return y;
    }
    float bottom() const {
        float y = 0;
        for (auto& r : f) y = std::max(y, r.y1);
        return y;
    }
};
// Where a block sits in its formatting context: BFC coordinates of the containing block's content-left
// edge and of this box's (provisional) border-box top.
struct LCtx {
    Ctx*  ctx = nullptr;
    float xabs = 0, yabs = 0;
};
struct Opts {
    bool  shrink = false, root = false;
    float forced_cw = -1; // content width fixed by the caller (table cells, absolute boxes)
    float forced_ch = -1; // content height fixed by the caller
    float xshift = 0;     // horizontal displacement inside the containing block (BFC boxes beside floats)
    bool  bfc_root = false; // flex items etc.: independent formatting context even if the style says otherwise
};
struct AbsRec {
    Box*  box;
    Box*  parent;   // box in whose flow it sits (static position reference)
    float sx, sy;   // static position relative to the parent's content origin
};

class Layouter {
public:
    explicit Layouter(const LayoutEnv& e) : env_(e), tm_(e.text ? *e.text : fake_text_measurer()) {}

    void run(Box& root) {
        BlockOut out;
        Ctx      dummy;
        root.skipped = false;
        Opts o;
        o.root = true;
        layout_block(root, env_.viewport_w, env_.viewport_h, o, LCtx{&dummy, 0, 0}, out);
        root.y = out.mt;
        finalize(root, 0, 0);
        for (size_t qi = 0; qi < abs_.size(); ++qi) {
            AbsRec r = abs_[qi]; // copy: placing may queue more
            place_abs(r);
        }
        clear_ink(root);
        compute_ink(root);
        compute_scroll(root, 0);
    }

    // How far each scroll container's content reaches past its padding box.
    static void overflow_extent(const Box& b, float& x1, float& y1, int depth) {
        if (depth > 200 || b.skipped || !b.style) return;
        for (auto& L : b.lines) {
            y1 = std::max(y1, L.y + L.h);
            for (auto& f : L.frags) {
                x1 = std::max(x1, f.x + f.w);
                if (f.kind == Frag::Kind::Atomic && f.box && f.box->parent != &b) {
                    x1 = std::max(x1, f.box->x + f.box->w + f.box->margin.right);
                    y1 = std::max(y1, f.box->y + f.box->h + f.box->margin.bottom);
                    if (!clips_content(*f.box)) overflow_extent(*f.box, x1, y1, depth + 1);
                }
            }
        }
        for (auto& c : b.children) {
            if (c->skipped || !c->style || c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak || c->kind == Box::Kind::Marker) continue;
            if (c->style->position == Position::Fixed) continue;
            if (!b.lines.empty() && c->style->display == Display::Inline && c->style->position != Position::Absolute) continue; // reached through lines
            x1 = std::max(x1, c->x + c->w + std::max(0.f, c->margin.right));
            y1 = std::max(y1, c->y + c->h + std::max(0.f, c->margin.bottom));
            if (!clips_content(*c)) overflow_extent(*c, x1, y1, depth + 1);
        }
        for (const Box* f : b.inline_floats) {
            x1 = std::max(x1, f->x + f->w);
            y1 = std::max(y1, f->y + f->h);
        }
    }
    static bool clips_content(const Box& b) { return b.style && (b.style->overflow_x != Overflow::Visible || b.style->overflow_y != Overflow::Visible); }
    static void compute_scroll(Box& b, int depth) {
        if (depth > 200) return;
        b.scroll_max_x = b.scroll_max_y = 0;
        if (b.style && b.kind != Box::Kind::Text && b.replaced == ReplacedKind::None && depth > 0) {
            bool sx = b.style->overflow_x == Overflow::Auto || b.style->overflow_x == Overflow::Scroll;
            bool sy = b.style->overflow_y == Overflow::Auto || b.style->overflow_y == Overflow::Scroll;
            if (sx || sy) {
                float x1 = -1e30f, y1 = -1e30f;
                overflow_extent(b, x1, y1, 0);
                float right  = b.x + b.w - b.border.right, bottom = b.y + b.h - b.border.bottom;
                if (sx && x1 > -1e29f) b.scroll_max_x = std::max(0.f, std::floor(x1 + b.padding.right - right));
                if (sy && y1 > -1e29f) b.scroll_max_y = std::max(0.f, std::floor(y1 + b.padding.bottom - bottom));
                if (b.scroll_max_x < 1) b.scroll_max_x = 0;
                if (b.scroll_max_y < 1) b.scroll_max_y = 0;
            }
        }
        for (auto& c : b.children) compute_scroll(*c, depth + 1);
    }

    static void clear_ink(Box& b) {
        b.ink_valid = false;
        for (auto& c : b.children) clear_ink(*c);
    }

    // Ink extents for paint culling (see Box::ink_y0). Conservative: anything uncertain widens the range.
    static void compute_ink(Box& b) {
        float y0 = b.y, y1 = b.y + b.h;
        if (b.style)
            for (const BoxShadow& sh : b.style->shadows)
                if (!sh.inset) {
                    float e = std::fabs(sh.oy) + 2 * sh.blur + std::max(0.f, sh.spread);
                    y0 = std::min(y0, b.y - e);
                    y1 = std::max(y1, b.y + b.h + e);
                }
        if (b.has_marker) { y0 = std::min(y0, b.marker_y - 64); y1 = std::max(y1, b.marker_y + 64); }
        if (b.style && b.style->has_xform) { // rotated / scaled: may reach well outside its box
            float e = std::max(b.w, b.h) * std::max({1.f, std::fabs(b.style->xf[0]) + std::fabs(b.style->xf[2]), std::fabs(b.style->xf[1]) + std::fabs(b.style->xf[3])});
            y0 = std::min(y0, b.y - e);
            y1 = std::max(y1, b.y + b.h + e);
        }
        auto merge = [&](const Box& c) {
            if (!c.ink_valid || c.skipped) return;
            if (c.style && c.style->position == Position::Fixed) return; // painted in viewport coordinates
            y0 = std::min(y0, c.ink_y0);
            y1 = std::max(y1, c.ink_y1);
        };
        for (auto& L : b.lines) {
            float l0 = L.y, l1 = L.y + L.h;
            for (auto& f : L.frags)
                if (f.kind == Frag::Kind::Atomic && f.box) {
                    compute_ink(*f.box);
                    if (f.box->ink_valid) { l0 = std::min(l0, f.box->ink_y0); l1 = std::max(l1, f.box->ink_y1); }
                }
            L.ink_y0 = l0;
            L.ink_y1 = l1;
            y0 = std::min(y0, l0);
            y1 = std::max(y1, l1);
        }
        for (Box* f : b.inline_floats) { compute_ink(*f); merge(*f); }
        bool inline_container = !b.lines.empty() || !b.inline_floats.empty();
        for (auto& c : b.children) {
            if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak || c->kind == Box::Kind::Marker) continue;
            if (!c->ink_valid) compute_ink(*c); // atomic inlines were done with their line
            // Inside an inline container, plain inline elements have no geometry of their own (their lines carry it);
            // only out-of-flow children count there.
            if (!inline_container || (c->style && (c->style->position == Position::Absolute || c->style->float_ != Float::None))) merge(*c);
        }
        b.ink_y0    = y0;
        b.ink_y1    = y1;
        b.ink_valid = true;
    }

private:
    const LayoutEnv& env_;
    TextMeasurer&    tm_;

    // ------------------------------------------------------------ edges / sizes
    static float pxv(const Length& l, float base) { return l.kind == Length::Px ? l.v : l.kind == Length::Percent ? l.v * base / 100.f : 0.f; }

    void resolve_edges(Box& b, float cb_w) {
        const Style& s = *b.style;
        b.margin  = {pxv(s.margin[0], cb_w), pxv(s.margin[1], cb_w), pxv(s.margin[2], cb_w), pxv(s.margin[3], cb_w)};
        b.padding = {std::max(0.f, pxv(s.padding[0], cb_w)), std::max(0.f, pxv(s.padding[1], cb_w)), std::max(0.f, pxv(s.padding[2], cb_w)),
                     std::max(0.f, pxv(s.padding[3], cb_w))};
        if (!b.border_fixed) b.border = {s.border_width[0], s.border_width[1], s.border_width[2], s.border_width[3]};
    }
    static float hpad(const Box& b) { return b.padding.left + b.padding.right + b.border.left + b.border.right; }
    static float vpad(const Box& b) { return b.padding.top + b.padding.bottom + b.border.top + b.border.bottom; }

    float line_height_of(const Style& st) {
        FontMetrics m = tm_.metrics(st);
        return st.line_height >= 0 ? st.line_height : m.ascent + m.descent + m.line_gap;
    }

    // A block narrower than its container whose parent has HTML align=center / <center> (or align=right) is moved
    // like the old browsers did, even without auto margins: <center><table width=600> centres the table.
    static void legacy_place(const Style& s, float slack, float& ml, float& mr) {
        if (s.float_ != Float::None || s.position == Position::Absolute || s.position == Position::Fixed) return;
        if (s.parent_align == 2) { ml += slack / 2; mr += slack / 2; }
        else if (s.parent_align == 3) ml += slack;
    }

    // ------------------------------------------------------------ replaced elements
    static bool attr_px(const Box& b, const char* name, float& out) {
        const std::string* v = b.attr(name);
        if (!v || v->empty() || v->find('%') != std::string::npos) return false;
        char* end = nullptr;
        float f   = std::strtof(v->c_str(), &end);
        if (end == v->c_str() || f < 0) return false;
        out = f;
        return true;
    }

    // Used content-box size of a replaced element.
    void replaced_size(Box& b, float cb_w, float cb_h, float& w, float& h) {
        const Style& s = *b.style;
        float iw = 0, ih = 0;
        bool  have_intrinsic = false, ratio = false;
        switch (b.replaced) {
        case ReplacedKind::Image: {
            const std::string* src = b.attr("src");
            int                ww = 0, hh = 0;
            // width and height both fixed (CSS or the HTML attributes): the picture itself cannot change the size, so
            // don't ask (and don't make its arrival relayout the page, as for every Wikipedia thumbnail)
            if (s.width.is_px() && s.height.is_px()) {
                if (src && env_.image_request) env_.image_request(*src); // still start the download now
                iw = s.width.v; ih = s.height.v; have_intrinsic = true;
                break;
            }
            if (src && env_.image_size && env_.image_size(*src, ww, hh) && ww > 0 && hh > 0) {
                iw = static_cast<float>(ww); ih = static_cast<float>(hh); have_intrinsic = ratio = true;
                // a srcset pick: a 2x picture is shown at half its pixel size, a "480w" one at its slot size
                if (const std::string* d = b.attr("density")) {
                    float den = std::strtof(d->c_str(), nullptr);
                    if (den > 0.01f && den < 100.f) { iw /= den; ih /= den; }
                }
                iw *= env_.zoom; ih *= env_.zoom;
            }
            else if (env_.image_broken && (!src || src->empty() || env_.image_broken(*src))) {
                // A broken image is shown as its alt text, so it takes the text's size (unless width/height say otherwise).
                const std::string* alt = b.attr("alt");
                if (alt && !alt->empty()) {
                    iw = tm_.width(s, *alt) + 4;
                    ih = line_height_of(s);
                } else if (!alt) {
                    iw = ih = 16 * env_.zoom; // broken-image marker
                }
                have_intrinsic = true;
            }
            break;
        }
        case ReplacedKind::Input: control_size(b, iw, ih); have_intrinsic = true; break;
        case ReplacedKind::Textarea: control_size(b, iw, ih); have_intrinsic = true; break;
        case ReplacedKind::Select: control_size(b, iw, ih); have_intrinsic = true; break;
        case ReplacedKind::Meter: case ReplacedKind::Progress: iw = 100 * env_.zoom; ih = 16 * env_.zoom; have_intrinsic = true; break;
        case ReplacedKind::Svg: {
            iw = 300 * env_.zoom; ih = 150 * env_.zoom; have_intrinsic = ratio = true;
            if (const std::string* vb = b.attr("viewbox")) {
                float a[4];
                const char* p = vb->c_str();
                int n = 0;
                while (n < 4) {
                    char* e = nullptr;
                    a[n] = std::strtof(p, &e);
                    if (e == p) break;
                    ++n;
                    p = e;
                    while (*p == ',' || *p == ' ') ++p;
                }
                if (n == 4 && a[2] > 0 && a[3] > 0) {
                    // no width/height attributes: like browsers, an inline svg with a viewBox fills its container's width
                    float base = (!b.attr("width") && !b.attr("height") && cb_w > 0 && cb_w < 1e5f) ? cb_w : 300.f * env_.zoom;
                    iw = base;
                    ih = base * a[3] / a[2];
                }
            }
            break;
        }
        default: iw = 300 * env_.zoom; ih = 150 * env_.zoom; have_intrinsic = true; ratio = true; break; // canvas, iframe, video, object
        }
        float aw = -1, ah = -1;
        float t;
        if (s.width.is_px() || s.width.is_pct()) aw = s.width.resolve(cb_w);
        else if (attr_px(b, "width", t)) aw = t * env_.zoom;
        if (s.height.is_px()) ah = s.height.v;
        else if (s.height.is_pct() && cb_h >= 0) ah = s.height.v * cb_h / 100.f;
        else if (attr_px(b, "height", t) && !s.height.is_pct()) ah = (s.height.is_auto() ? t * env_.zoom : -1);
        if (s.box_sizing == BoxSizing::BorderBox) {
            if (aw >= 0 && (s.width.is_px() || s.width.is_pct())) aw = std::max(0.f, aw - hpad(b));
            if (ah >= 0 && s.height.is_px()) ah = std::max(0.f, ah - vpad(b));
        }
        if (aw < 0 && ah < 0) { w = iw; h = ih; }
        else if (aw < 0) { h = ah; w = (ratio && ih > 0) ? ah * iw / ih : iw; }
        else if (ah < 0) { w = aw; h = (ratio && iw > 0) ? aw * ih / iw : ih; }
        else { w = aw; h = ah; }
        (void)have_intrinsic;
        // min/max constraints (keeping the ratio when the other dimension is auto)
        float maxw = s.max_width.is_none() ? -1 : s.max_width.resolve(cb_w);
        float minw = s.min_width.resolve(cb_w, 0);
        if (s.box_sizing == BoxSizing::BorderBox) {
            if (maxw >= 0) maxw = std::max(0.f, maxw - hpad(b));
            minw = std::max(0.f, minw - hpad(b));
        }
        bool h_auto = !(s.height.is_px() || (s.height.is_pct() && cb_h >= 0));
        if (maxw >= 0 && w > maxw) { if (h_auto && w > 0) h = h * maxw / w; w = maxw; }
        if (w < minw) { if (h_auto && w > 0) h = h * minw / w; w = minw; }
        float maxh = s.max_height.is_none() ? -1 : s.max_height.resolve(cb_h >= 0 ? cb_h : 0, -1);
        if (s.max_height.is_pct() && cb_h < 0) maxh = -1;
        if (maxh >= 0 && h > maxh) h = maxh;
        h = std::max(h, s.min_height.is_px() ? s.min_height.v : 0.f);
        w = std::max(0.f, w);
        h = std::max(0.f, h);
    }

    void control_size(Box& b, float& w, float& h) {
        const Style& s  = *b.style;
        float        cw = tm_.width(s, "0");
        float        lh = line_height_of(s);
        const std::string* type = b.attr("type");
        std::string t = type ? *type : "text";
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (b.replaced == ReplacedKind::Textarea) {
            float cols = 20, rows = 2, v;
            if (attr_px(b, "cols", v) && v > 0) cols = v;
            if (attr_px(b, "rows", v) && v > 0) rows = v;
            w = cols * cw;
            h = rows * lh;
        } else if (b.replaced == ReplacedKind::Select) {
            { const std::string* ml = b.attr("maxlabel"); w = tm_.width(s, ml && !ml->empty() ? *ml : b.text) + 24 * env_.zoom; }
            h = lh;
            float sz = 0;
            bool  multi = b.attr("multiple") != nullptr;
            if (!attr_px(b, "size", sz) || sz < 1) sz = 0;
            if (multi || sz > 1) h = lh * (sz >= 1 ? std::min(sz, 64.0f) : 4.0f); // a list box shows that many rows
        } else if (t == "checkbox" || t == "radio") {
            w = h = 13 * env_.zoom;
        } else if (t == "submit" || t == "button" || t == "reset") {
            const std::string* v = b.attr("value");
            std::string label = v ? *v : (t == "submit" ? "Submit" : t == "reset" ? "Reset" : "");
            w = tm_.width(s, label);
            h = lh;
        } else if (t == "image") {
            w = 0; h = 0;
            const std::string* src = b.attr("src");
            int ww = 0, hh = 0;
            if (src && env_.image_size && env_.image_size(*src, ww, hh)) { w = ww * env_.zoom; h = hh * env_.zoom; }
        } else if (t == "range") {
            w = 129 * env_.zoom; h = 16 * env_.zoom;
        } else if (t == "file") { // [Browse…] No file selected.
            w = tm_.width(s, "Browse\xe2\x80\xa6") + 22 * env_.zoom + tm_.width(s, b.attr("multiple") ? "No files selected." : "No file selected.");
            h = lh + 4 * env_.zoom;
        } else {
            float size = 20, v;
            if (attr_px(b, "size", v) && v > 0) size = v;
            w = size * cw;
            h = lh;
        }
    }

    // ------------------------------------------------------------ intrinsic (min/max-content) widths
    struct Intrinsic { float min = 0, max = 0; };

    float outer_h_edges(const Box&, const Style& s) {
        float m = (s.margin[1].is_px() ? s.margin[1].v : 0) + (s.margin[3].is_px() ? s.margin[3].v : 0);
        float p = (s.padding[1].is_px() ? s.padding[1].v : 0) + (s.padding[3].is_px() ? s.padding[3].v : 0);
        return m + p + s.border_width[1] + s.border_width[3];
    }

    Intrinsic intrinsic(Box& b) {
        if (b.min_content >= 0) return {b.min_content, b.max_content};
        Intrinsic r;
        const Style& s = *b.style;
        if (b.replaced != ReplacedKind::None) {
            resolve_edges(b, 0);
            float w, h;
            // a percentage max-width must not clamp the intrinsic size against a zero containing block
            replaced_size(b, s.max_width.is_pct() && !s.width.is_pct() ? 1e6f : 0.f, -1, w, h);
            r.min = r.max = w;
            if (s.width.is_pct() || s.max_width.is_pct()) r.min = 0;
        } else if (s.width.is_px() && s.display != Display::Inline) {
            float w = s.width.v;
            if (s.box_sizing == BoxSizing::BorderBox) w = std::max(0.f, w - (s.padding[1].is_px() ? s.padding[1].v : 0) - (s.padding[3].is_px() ? s.padding[3].v : 0) - s.border_width[1] - s.border_width[3]);
            r.min = r.max = w;
        } else if (s.display == Display::Table || s.display == Display::InlineTable) {
            r = table_intrinsic(b);
        } else if (s.display == Display::Flex || s.display == Display::InlineFlex) {
            r = flex_intrinsic(b);
        } else if (s.display == Display::Grid || s.display == Display::InlineGrid) {
            r = grid_intrinsic(b);
        } else {
            r = intrinsic_contents(b);
        }
        if (s.min_width.is_px()) {
            float mw = s.min_width.v;
            if (s.box_sizing == BoxSizing::BorderBox) { resolve_edges(b, 0); mw = std::max(0.f, mw - hpad(b)); }
            r.min = std::max(r.min, mw);
            r.max = std::max(r.max, mw); // min-width also lifts the preferred width
        }
        if (s.max_width.is_px()) { r.max = std::min(r.max, s.max_width.v); r.min = std::min(r.min, s.max_width.v); }
        b.min_content = r.min;
        b.max_content = r.max;
        return r;
    }

    // Min-content width of b's contents, ignoring b's own `width` (the flex "content size suggestion").
    float content_min_width(Box& b) {
        const Style& s = *b.style;
        if (b.replaced != ReplacedKind::None || !s.width.is_px() || s.display == Display::Inline) return intrinsic(b).min;
        Intrinsic r;
        if (s.display == Display::Table || s.display == Display::InlineTable) r = table_intrinsic(b);
        else if (s.display == Display::Flex || s.display == Display::InlineFlex) r = flex_intrinsic(b);
        else if (s.display == Display::Grid || s.display == Display::InlineGrid) r = grid_intrinsic(b);
        else r = intrinsic_contents(b);
        return r.min;
    }

    Intrinsic intrinsic_contents(Box& b) {
        Intrinsic r;
        bool block_children = false;
        for (auto& c : b.children)
            if (c->kind != Box::Kind::Text && c->kind != Box::Kind::LineBreak && c->kind != Box::Kind::Marker && c->style && c->style->block_level() && !is_out_of_flow(*c)) block_children = true;
        if (block_children) {
            for (auto& c : b.children) {
                if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak || c->kind == Box::Kind::Marker || is_out_of_flow(*c)) continue;
                Intrinsic ci = intrinsic(*c);
                float     e  = outer_h_edges(*c, *c->style);
                r.min = std::max(r.min, ci.min + e);
                r.max = std::max(r.max, ci.max + e);
            }
        } else {
            r = inline_intrinsic(b);
        }
        return r;
    }

    Intrinsic inline_intrinsic(Box& b) {
        Intrinsic r;
        float     cur = 0, chunk = 0;
        bool      prev_space = true;
        std::function<void(Box&)> walk = [&](Box& c) {
            for (auto& ch : c.children) {
                Box& x = *ch;
                if (x.kind == Box::Kind::Text) {
                    const Style& st = *x.style;
                    bool         pre = !st.collapses_whitespace();
                    size_t       i = 0;
                    const std::string& t = x.text;
                    while (i < t.size()) {
                        if (is_space(t[i])) {
                            if (pre && t[i] == '\n') { r.min = std::max(r.min, chunk); chunk = 0; r.max = std::max(r.max, cur); cur = 0; prev_space = true; ++i; continue; }
                            float sw = tm_.width(st, " ");
                            bool can_wrap = st.white_space != WhiteSpace::NoWrap && st.white_space != WhiteSpace::Pre;
                            if (!(prev_space && !pre)) { cur += sw; if (!can_wrap) chunk += sw; }
                            if (can_wrap) { r.min = std::max(r.min, chunk); chunk = 0; }
                            prev_space = true;
                            while (i < t.size() && is_space(t[i]) && !(pre && t[i] == '\n')) ++i;
                        } else {
                            size_t j = i;
                            while (j < t.size() && !is_space(t[j])) ++j;
                            float w = tm_.width(st, t.substr(i, j - i)) + st.letter_spacing * static_cast<float>(utf8_len(t.substr(i, j - i)));
                            chunk += w;
                            cur += w;
                            prev_space = false;
                            i = j;
                        }
                    }
                } else if (x.kind == Box::Kind::LineBreak) {
                    r.min = std::max(r.min, chunk); chunk = 0;
                    r.max = std::max(r.max, cur); cur = 0;
                    prev_space = true;
                } else if (x.kind == Box::Kind::Marker) {
                    continue;
                } else if (is_out_of_flow(x) && !(x.style->float_ != Float::None)) {
                    continue;
                } else if (x.style->display == Display::Inline && x.replaced == ReplacedKind::None) {
                    resolve_edges(x, 0);
                    float el = x.margin.left + x.padding.left + x.border.left, er = x.margin.right + x.padding.right + x.border.right;
                    cur += el + er; chunk += el + er;
                    walk(x);
                } else {
                    Intrinsic ci = intrinsic(x);
                    float     e  = outer_h_edges(x, *x.style);
                    r.min = std::max(r.min, std::max(chunk, ci.min + e));
                    chunk = 0;
                    cur += ci.max + e;
                    prev_space = false;
                }
            }
        };
        walk(b);
        r.min = std::max(r.min, chunk);
        r.max = std::max(r.max, cur);
        return r;
    }

    static bool is_out_of_flow(const Box& b) {
        if (b.kind == Box::Kind::Text || !b.style) return false;
        const Style& s = *b.style;
        return s.float_ != Float::None || s.position == Position::Absolute || s.position == Position::Fixed;
    }
    static bool is_abs(const Box& b) {
        return b.kind != Box::Kind::Text && b.style && (b.style->position == Position::Absolute || b.style->position == Position::Fixed);
    }

    // ------------------------------------------------------------ block layout
    std::vector<AbsRec> abs_;

    static bool is_float(const Box& b) {
        return b.kind != Box::Kind::Text && b.kind != Box::Kind::LineBreak && b.kind != Box::Kind::Marker && b.style && b.style->float_ != Float::None && !is_abs(b);
    }

    // Top margin that escapes from c: its own plus the chain of first in-flow block children it collapses with.
    Margin pre_mt(Box& c, float cbw) {
        Margin       m;
        const Style& s = *c.style;
        m.add(pxv(s.margin[0], cbw));
        if (c.replaced != ReplacedKind::None || establishes_bfc(c, false) || s.border_width[0] != 0 || pxv(s.padding[0], cbw) != 0) return m;
        for (auto& ch : c.children) {
            Box& d = *ch;
            if (d.kind == Box::Kind::Marker) continue;
            if (d.kind == Box::Kind::Text || d.kind == Box::Kind::LineBreak) return m;
            if (is_abs(d) || is_float(d)) continue;
            if (!d.style->block_level()) return m;
            Margin dm = pre_mt(d, cbw);
            m.add(dm.pos);
            m.add(dm.neg);
            return m;
        }
        return m;
    }

    // Lays out float `c` and places it in `ctx`. (cxa, cya): BFC coordinates of the container's content origin;
    // flow_y: where the float would start in the container's flow (relative to its content top).
    void place_float(Box& c, float cbw, Ctx& ctx, float cxa, float cya, float flow_y) {
        BlockOut o;
        Ctx      dummy;
        Opts     op;
        op.shrink = true;
        layout_block(c, cbw, -1, op, LCtx{&dummy, 0, 0}, o);
        float wm = c.margin.left + c.w + c.margin.right;
        float hm = std::max(0.f, c.margin.top + c.h + c.margin.bottom);
        float y0 = std::max(cya + flow_y, ctx.last_top);
        if (c.style->clear != Clear::None) y0 = std::max(y0, ctx.clear_y(c.style->clear));
        bool  left = c.style->float_ == Float::Left;
        float lo = cxa, hi = cxa + cbw;
        for (int tries = 0; tries < 256; ++tries) {
            lo = cxa;
            hi = cxa + cbw;
            ctx.range(y0, y0 + hm, lo, hi);
            if (wm <= hi - lo + 0.01f) break;
            float ne = ctx.next_edge(y0);
            if (ne < 0) break;
            y0 = ne;
        }
        float x0 = left ? lo : std::max(lo, hi - wm);
        ctx.f.push_back({&c, x0, y0, x0 + wm, y0 + hm, left});
        ctx.last_top = y0;
        c.x = x0 + c.margin.left - cxa;
        c.y = y0 + c.margin.top - cya;
    }

    void layout_block(Box& b, float cb_w, float cb_h, const Opts& o, const LCtx& lc, BlockOut& out) {
        const Style& s = *b.style;
        resolve_edges(b, cb_w);
        b.lines.clear();
        b.inline_floats.clear();
        b.has_marker    = false;
        b.last_baseline = -1;
        b.content_dy    = 0;
        if (s.display == Display::Table || s.display == Display::InlineTable) { layout_table(b, cb_w, cb_h, o, out); return; }
        bool shrink = o.shrink, is_root = o.root;

        // ---- content width
        bool  auto_ml = s.margin[3].is_auto(), auto_mr = s.margin[1].is_auto();
        float content_w;
        float h_extra = hpad(b);
        bool  replaced = b.replaced != ReplacedKind::None;
        float rw = 0, rh = 0;
        if (replaced) {
            replaced_size(b, cb_w, cb_h, rw, rh);
            if (o.forced_cw >= 0) rw = o.forced_cw;
            if (o.forced_ch >= 0) rh = o.forced_ch;
            content_w = rw;
        } else if (o.forced_cw >= 0) {
            content_w = o.forced_cw;
        } else {
            bool explicit_w = s.width.is_px() || s.width.is_pct();
            if (explicit_w) {
                content_w = s.width.resolve(cb_w);
                if (s.box_sizing == BoxSizing::BorderBox) content_w = std::max(0.f, content_w - h_extra);
            } else if (shrink) {
                Intrinsic in = intrinsic(b);
                float avail = std::max(0.f, cb_w - b.margin.left - b.margin.right - h_extra);
                content_w = std::min(std::max(in.min, avail), in.max);
            } else {
                content_w = std::max(0.f, cb_w - (auto_ml ? 0 : b.margin.left) - (auto_mr ? 0 : b.margin.right) - h_extra);
            }
            // min / max width
            float minw = s.min_width.resolve(cb_w, 0), maxw = s.max_width.is_none() ? -1.f : s.max_width.resolve(cb_w);
            if (s.box_sizing == BoxSizing::BorderBox) { minw = std::max(0.f, minw - h_extra); if (maxw >= 0) maxw = std::max(0.f, maxw - h_extra); }
            if (maxw >= 0 && content_w > maxw) content_w = maxw;
            if (content_w < minw) content_w = minw;
        }
        // horizontal margins (auto): only for in-flow block-level boxes
        if (!shrink && o.forced_cw < 0) {
            float slack = cb_w - (content_w + h_extra + b.margin.left + b.margin.right);
            if (slack > 0) {
                if (auto_ml && auto_mr) b.margin.left = b.margin.right = slack / 2;
                else if (auto_ml) b.margin.left = slack;
                else if (auto_mr) b.margin.right = slack;
                else legacy_place(s, slack, b.margin.left, b.margin.right);
            }
        }
        b.x = b.margin.left + o.xshift;
        float inner_x_w = content_w;

        // ---- content
        float content_h = 0;
        float definite_h = -1;
        if (o.forced_ch >= 0) definite_h = o.forced_ch;
        else if (s.height.is_px()) definite_h = s.height.v;
        else if (s.height.is_pct() && cb_h >= 0) definite_h = s.height.v * cb_h / 100.f;
        if (o.forced_ch < 0 && definite_h >= 0 && s.box_sizing == BoxSizing::BorderBox) definite_h = std::max(0.f, definite_h - vpad(b));

        bool bfc = establishes_bfc(b, is_root) || o.bfc_root;
        bool top_collapses = b.border.top == 0 && b.padding.top == 0 && !bfc;
        bool bottom_collapses = b.border.bottom == 0 && b.padding.bottom == 0 && !bfc && definite_h < 0 && s.min_height.kind != Length::Px;
        if (s.min_height.is_px() && s.min_height.v > 0) bottom_collapses = false;
        Margin out_top;
        out_top.add(b.margin.top);
        Margin out_bottom_pending;
        bool   placed_any = false, has_content = false;

        // float context: a BFC root owns one, everything else shares its parent's
        Ctx   own;
        Ctx*  ctx;
        float cxa, cya;
        if (bfc || !lc.ctx) { ctx = &own; cxa = cya = 0; }
        else {
            ctx = lc.ctx;
            cxa = lc.xabs + o.xshift + b.margin.left + b.border.left + b.padding.left;
            cya = lc.yabs + b.border.top + b.padding.top;
        }

        const bool is_flex = !replaced && (s.display == Display::Flex || s.display == Display::InlineFlex);
        const bool is_grid = !replaced && (s.display == Display::Grid || s.display == Display::InlineGrid);
        if (replaced) {
            content_h = rh;
            has_content = true;
        } else if (is_grid) {
            float gminh = s.min_height.is_px() ? s.min_height.v : 0.f, gmaxh = s.max_height.is_px() ? s.max_height.v : -1.f;
            if (s.box_sizing == BoxSizing::BorderBox) { gminh = std::max(0.f, gminh - vpad(b)); if (gmaxh >= 0) gmaxh = std::max(0.f, gmaxh - vpad(b)); }
            content_h = grid_content(b, inner_x_w, definite_h, gminh, gmaxh);
            has_content = true;
            placed_any = true;
        } else if (is_flex) {
            float fminh = s.min_height.is_px() ? s.min_height.v : 0.f, fmaxh = s.max_height.is_px() ? s.max_height.v : -1.f;
            if (s.box_sizing == BoxSizing::BorderBox) { fminh = std::max(0.f, fminh - vpad(b)); if (fmaxh >= 0) fmaxh = std::max(0.f, fmaxh - vpad(b)); }
            content_h = flex_content(b, inner_x_w, definite_h, fminh, fmaxh);
            has_content = true;
            placed_any = true;
        } else {
            bool has_inline = false, has_block = false, any_abs = false;
            for (auto& c : b.children) {
                if (c->kind == Box::Kind::Marker) continue;
                if (is_abs(*c)) { any_abs = true; continue; }
                if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak) has_inline = true;
                else if (c->style->inline_level() && !is_out_of_flow(*c)) has_inline = true;
                else has_block = true; // blocks (and floats, which sit among blocks)
            }
            if (!has_inline && !has_block && any_abs) has_block = true; // only out-of-flow children: still queue them
            if (has_inline) has_block = false; // mixed content: floats among inline content are laid out inline
            if (has_block && !has_inline) {
                float cursor = 0;
                Margin prev_mb;
                float child_cb_h = definite_h;
                for (auto& cp : b.children) {
                    Box& c = *cp;
                    if (c.kind == Box::Kind::Marker) continue;
                    if (is_abs(c)) {
                        c.skipped = true; c.w = c.h = 0;
                        abs_.push_back({&c, &b, 0, (placed_any || !top_collapses) ? cursor + prev_mb.value() : cursor});
                        continue;
                    }
                    if (is_float(c)) {
                        place_float(c, inner_x_w, *ctx, cxa, cya, (placed_any || !top_collapses) ? cursor + prev_mb.value() : cursor);
                        continue;
                    }
                    const bool   cbfc = establishes_bfc(c, false);
                    const size_t saved_f = ctx->f.size(), saved_abs = abs_.size();
                    const bool   first_collapse = !placed_any && top_collapses;
                    Margin       pm = pre_mt(c, inner_x_w);
                    Margin       gp = prev_mb;
                    gp.add(pm.pos);
                    gp.add(pm.neg);
                    float ytop = first_collapse ? cursor : cursor + gp.value();
                    float clear_need = -std::numeric_limits<float>::infinity();
                    if (c.style->clear != Clear::None) {
                        float cl = ctx->clear_y(c.style->clear);
                        if (cl > -1e30f) { clear_need = cl - cya; ytop = std::max(ytop, clear_need); }
                    }
                    BlockOut co;
                    float    cbw = inner_x_w, xs = 0;
                    for (int tries = 0;; ++tries) {
                        cbw = inner_x_w;
                        xs  = 0;
                        if (cbfc && !ctx->f.empty()) {
                            float lo = cxa, hi = cxa + inner_x_w, ya = cya + ytop;
                            if (ctx->range(ya, ya + 1, lo, hi)) { cbw = hi - lo; xs = lo - cxa; }
                        }
                        abs_.resize(saved_abs);
                        Opts co_opts;
                        co_opts.xshift = xs;
                        layout_block(c, cbw, child_cb_h, co_opts, LCtx{ctx, cxa, cya + ytop}, co);
                        if (xs == 0 && cbw == inner_x_w) break;
                        float need = c.w + c.margin.left + c.margin.right;
                        if (c.style->width.is_auto()) need = std::min(need, intrinsic(c).min + outer_h_edges(c, *c.style));
                        if (need <= cbw + 0.01f || tries > 16) break;
                        float ne = ctx->next_edge(cya + ytop);
                        if (ne < 0) break;
                        ytop = ne - cya;
                    }
                    float ch_total = c.h;
                    if (co.empty_through) {
                        if (!placed_any && top_collapses) { out_top.add(co.mt); out_top.add(co.mb); }
                        else { prev_mb.add(co.mt); prev_mb.add(co.mb); }
                        c.y = cursor;
                        continue;
                    }
                    float yfin;
                    if (!placed_any) {
                        if (top_collapses) {
                            out_top.add(co.mt);
                            yfin = cursor;
                        } else {
                            Margin g = prev_mb;
                            g.add(co.mt);
                            yfin = cursor + g.value();
                        }
                    } else {
                        Margin g = prev_mb;
                        g.add(co.mt);
                        yfin = cursor + g.value();
                    }
                    if (clear_need > -1e30f) yfin = std::max(yfin, clear_need);
                    if (!cbfc && !ctx->f.empty() && std::fabs(yfin - ytop) > 0.5f) {
                        // the provisional position was off (margins collapsed differently): lay out again at the right place
                        ctx->f.resize(saved_f);
                        abs_.resize(saved_abs);
                        BlockOut co2;
                        Opts     co_opts;
                        co_opts.xshift = xs;
                        layout_block(c, cbw, child_cb_h, co_opts, LCtx{ctx, cxa, cya + yfin}, co2);
                        ch_total = c.h;
                    }
                    c.y = yfin;
                    cursor = c.y + ch_total;
                    prev_mb = Margin{};
                    prev_mb.add(co.mb);
                    placed_any = true;
                    has_content = true;
                }
                if (placed_any) {
                    if (bottom_collapses) { out_bottom_pending = prev_mb; content_h = cursor; }
                    else { content_h = cursor + prev_mb.value(); }
                } else {
                    content_h = cursor;
                }
            } else if (has_inline) {
                content_h = layout_inline(b, inner_x_w, *ctx, cxa, cya);
                has_content = !b.lines.empty();
                if (has_content) placed_any = true;
            }
        }
        if (bfc && !ctx->f.empty()) content_h = std::max(content_h, ctx->bottom()); // a BFC contains its floats

        // ---- height
        float final_content_h = content_h;
        bool  height_auto = definite_h < 0;
        if (!height_auto) final_content_h = definite_h;
        float minh = s.min_height.is_px() ? s.min_height.v : 0.f;
        if (s.box_sizing == BoxSizing::BorderBox) minh = std::max(0.f, minh - vpad(b));
        float maxh = s.max_height.is_px() ? s.max_height.v : -1.f;
        if (s.max_height.is_pct() && cb_h >= 0) maxh = s.max_height.v * cb_h / 100.f;
        if (s.box_sizing == BoxSizing::BorderBox && maxh >= 0) maxh = std::max(0.f, maxh - vpad(b));
        if (o.forced_ch < 0) {
            if (maxh >= 0 && final_content_h > maxh) final_content_h = maxh;
            if (final_content_h < minh) final_content_h = minh;
        }

        b.w = content_w + h_extra;
        b.h = final_content_h + vpad(b);

        // ---- margins escaping to the parent
        bool empty = !has_content && height_auto && final_content_h == 0 && b.border.top == 0 && b.border.bottom == 0 && b.padding.top == 0 &&
                     b.padding.bottom == 0 && !bfc && !replaced && minh == 0;
        out.empty_through = empty;
        if (top_collapses) out.mt = out_top.value(); else out.mt = b.margin.top;
        if (bottom_collapses && placed_any && !b.lines.size()) { Margin m = out_bottom_pending; m.add(b.margin.bottom); out.mb = m.value(); }
        else out.mb = b.margin.bottom;
        if (empty) { out.mt = joined(out_top, 0).value(); out.mb = b.margin.bottom; }

        // ---- list marker (outside)
        if (s.display == Display::ListItem) place_marker(b);
        if (b.last_baseline < 0 && !b.lines.empty()) b.last_baseline = b.border.top + b.padding.top + b.lines.back().y + b.lines.back().baseline;
        else if (b.last_baseline < 0 && !replaced) {
            // last in-flow block child's baseline
            for (size_t i = b.children.size(); i-- > 0;) {
                Box& c = *b.children[i];
                if (c.kind == Box::Kind::Marker || c.skipped || c.kind == Box::Kind::Text || is_float(c)) continue;
                if (c.last_baseline >= 0) { b.last_baseline = b.border.top + b.padding.top + c.y + c.last_baseline; }
                break;
            }
        }
    }

    // Baseline of the first line, relative to b's content origin; -1 if none.
    float first_baseline(const Box& b) {
        if (!b.lines.empty()) return b.lines[0].y + b.lines[0].baseline;
        for (auto& c : b.children) {
            if (c->kind == Box::Kind::Marker || c->kind == Box::Kind::Text || c->skipped) continue;
            float fb = first_baseline(*c);
            if (fb >= 0) return c->y + c->border.top + c->padding.top + fb;
        }
        return -1;
    }

    void place_marker(Box& b) {
        for (auto& c : b.children) {
            if (c->kind != Box::Kind::Marker) continue;
            const Style& st = *c->style;
            float w  = tm_.width(st, c->text);
            float fb = first_baseline(b);
            if (fb < 0) {
                FontMetrics m = tm_.metrics(st);
                fb = m.ascent;
            }
            b.has_marker = true;
            b.marker_w   = w;
            b.marker_x   = -(w + st.font_size * 0.4f);
            b.marker_y   = fb;
            return;
        }
    }

    // ------------------------------------------------------------ inline layout
    void gen_atoms(Box& container, std::vector<Atom>& atoms) {
        bool  prev_space = true; // collapsible space at line start is dropped
        std::vector<float> shifts{0.f};
        std::function<void(Box&)> walk = [&](Box& c) {
            for (auto& ch : c.children) {
                Box& x = *ch;
                if (x.kind == Box::Kind::Marker) continue;
                if (x.kind == Box::Kind::Text) { text_atoms(x, atoms, prev_space, shifts.back()); continue; }
                if (x.kind == Box::Kind::LineBreak) {
                    Atom a; a.k = Atom::K::Break; a.box = &x; atoms.push_back(a);
                    prev_space = true;
                    continue;
                }
                if (is_abs(x)) { x.skipped = true; x.w = x.h = 0; Atom a; a.k = Atom::K::Abs; a.box = &x; atoms.push_back(a); continue; }
                if (is_float(x)) { Atom a; a.k = Atom::K::Float; a.box = &x; atoms.push_back(a); continue; }
                bool atomic = x.replaced != ReplacedKind::None || x.style->display != Display::Inline;
                if (atomic) {
                    Atom a; a.k = Atom::K::Atomic; a.box = &x; a.shift = shifts.back();
                    atoms.push_back(a);
                    prev_space = false;
                    continue;
                }
                // inline element
                resolve_edges(x, container_w_);
                Atom o; o.k = Atom::K::Open; o.box = &x; o.first_seg = !x.continuation;
                o.w = x.margin.left + x.border.left + x.padding.left;
                float sh = shifts.back();
                const Style& st = *x.style;
                switch (st.vertical_align) {
                case VAlign::Sub: sh += st.font_size * 0.2f; break;
                case VAlign::Super: sh -= st.font_size * 0.33f; break;
                case VAlign::Length: sh -= st.vertical_align_len; break;
                default: break;
                }
                shifts.push_back(sh);
                o.shift = sh;
                atoms.push_back(o);
                walk(x);
                Atom e; e.k = Atom::K::Close; e.box = &x; e.shift = sh;
                e.w = x.margin.right + x.border.right + x.padding.right;
                atoms.push_back(e);
                shifts.pop_back();
            }
        };
        walk(container);
    }

    float container_w_ = 0;

    void text_atoms(Box& tb, std::vector<Atom>& atoms, bool& prev_space, float shift) {
        const Style& st = *tb.style;
        const std::string& t = tb.text;
        bool collapse = st.collapses_whitespace();
        bool keep_nl  = st.white_space == WhiteSpace::Pre || st.white_space == WhiteSpace::PreWrap || st.white_space == WhiteSpace::PreLine;
        float spw = tm_.width(st, " ") + st.word_spacing + st.letter_spacing;
        size_t i = 0;
        while (i < t.size()) {
            char c = t[i];
            if (c == '\n' && keep_nl) {
                Atom a; a.k = Atom::K::Break; a.box = &tb; atoms.push_back(a);
                prev_space = true;
                ++i;
                continue;
            }
            if (is_space(c)) {
                if (collapse || st.white_space == WhiteSpace::PreLine) {
                    size_t j = i;
                    while (j < t.size() && is_space(t[j]) && !(keep_nl && t[j] == '\n')) ++j;
                    if (!prev_space) {
                        Atom a; a.k = Atom::K::Space; a.box = &tb; a.w = spw; a.text = " "; a.shift = shift; a.collapsible = true;
                        atoms.push_back(a);
                        prev_space = true;
                    }
                    i = j;
                } else { // pre / pre-wrap: literal spaces, tabs to the next multiple of 8
                    Atom a; a.k = Atom::K::Space; a.box = &tb; a.shift = shift; a.collapsible = false;
                    if (c == '\t') { a.text = "        "; a.w = spw * 8; } else { a.text = " "; a.w = spw; }
                    atoms.push_back(a);
                    prev_space = false;
                    ++i;
                }
                continue;
            }
            size_t j = i;
            while (j < t.size() && !is_space(t[j])) ++j;
            Atom a; a.k = Atom::K::Word; a.box = &tb; a.text = t.substr(i, j - i); a.shift = shift;
            a.w = tm_.width(st, a.text) + st.letter_spacing * static_cast<float>(utf8_len(a.text)) + st.word_spacing * 0;
            atoms.push_back(a);
            prev_space = false;
            i = j;
        }
    }

    struct Vert { float asc = 0, desc = 0; };

    static float baseline_offset(const Box& x, float mh) { // from the margin-box top
        if (x.replaced != ReplacedKind::None || x.last_baseline < 0) return mh;
        if (x.style->overflow_x != Overflow::Visible || x.style->overflow_y != Overflow::Visible) return mh;
        return x.margin.top + x.last_baseline;
    }

    // Vertical extents of an atomic box around the baseline, applying its vertical-align.
    void prepare_atomic(Atom& a, const FontMetrics& sm) {
        Box&  x  = *a.box;
        float mh = x.margin.top + x.h + x.margin.bottom;
        float B  = baseline_offset(x, mh);
        a.up = B;
        a.down = mh - B;
        const Style& st = *x.style;
        switch (st.vertical_align) {
        case VAlign::Middle: { float mid = 0.25f * st.font_size; a.up = mh / 2 + mid; a.down = mh / 2 - mid; break; }
        case VAlign::Sub: a.shift += st.font_size * 0.2f; break;
        case VAlign::Super: a.shift -= st.font_size * 0.33f; break;
        case VAlign::Length: a.shift -= st.vertical_align_len; break;
        case VAlign::TextTop: a.shift = a.up - sm.ascent; break;
        case VAlign::TextBottom: a.shift = sm.descent - a.down; break;
        default: break;
        }
    }

    Vert text_vert(const Style& st) {
        FontMetrics m = tm_.metrics(st);
        float lh = line_height_of(st);
        float lead = (lh - m.ascent - m.descent) / 2;
        return Vert{m.ascent + lead, m.descent + lead};
    }

    struct OpenSpan { Box* box; float x0; bool first; float shift; };

    static bool is_skippable(const Atom& a) { return (a.k == Atom::K::Space && a.collapsible) || a.k == Atom::K::Nop; }

    // Returns the total height of the inline content; fills b.lines (relative to b's content origin).
    // (cxa, cya) is b's content origin in the BFC of `ctx`, used to flow lines around floats.
    float layout_inline(Box& b, float avail, Ctx& ctx, float cxa, float cya) {
        const Style& cs = *b.style;
        container_w_ = avail;
        std::vector<Atom> atoms;
        gen_atoms(b, atoms);
        bool any = false;
        for (auto& a : atoms) {
            if ((a.k == Atom::K::Space && a.collapsible) || a.k == Atom::K::Float || a.k == Atom::K::Abs || a.k == Atom::K::Nop) continue;
            // CSS 2.1 9.4.2: an inline with no margin / border / padding alone does not make a line exist
            // (the blank pieces of a <font> or <a> split around blocks must not leave empty lines behind).
            if ((a.k == Atom::K::Open || a.k == Atom::K::Close) && a.w == 0 && a.box && a.box->border.top == 0 && a.box->border.bottom == 0 &&
                a.box->padding.top == 0 && a.box->padding.bottom == 0)
                continue;
            any = true;
            break;
        }

        auto side = [&](Atom& a, float flow_y, float sx) {
            if (a.k == Atom::K::Float) {
                place_float(*a.box, avail, ctx, cxa, cya, flow_y);
                b.inline_floats.push_back(a.box);
            } else if (a.k == Atom::K::Abs) {
                abs_.push_back({a.box, &b, sx, flow_y});
            }
            a.k = Atom::K::Nop;
            a.w = 0;
        };
        if (!any) {
            for (auto& a : atoms) if (a.k == Atom::K::Float || a.k == Atom::K::Abs) side(a, 0, 0);
            return 0;
        }

        FontMetrics sm = tm_.metrics(cs);
        for (auto& a : atoms)
            if (a.k == Atom::K::Atomic) {
                Box& x = *a.box;
                BlockOut o;
                Ctx      dummy;
                Opts     op;
                op.shrink = true;
                layout_block(x, avail, -1, op, LCtx{&dummy, 0, 0}, o);
                a.w = x.w + x.margin.left + x.margin.right;
                prepare_atomic(a, sm);
            }
        container_w_ = avail;

        Vert  strut = text_vert(cs);
        float strut_lh = strut.asc + strut.desc;
        bool  wrap = cs.white_space != WhiteSpace::Pre && cs.white_space != WhiteSpace::NoWrap;
        float indent = pxv(cs.text_indent, avail);

        float cursor_y = 0;
        size_t i = 0;
        bool first_line = true;
        std::vector<OpenSpan> carried;
        std::vector<Box*>     deferred; // floats met mid-line: placed at the top of the next line

        auto flush_deferred = [&]() {
            for (Box* fb : deferred) {
                place_float(*fb, avail, ctx, cxa, cya, cursor_y);
                b.inline_floats.push_back(fb);
            }
            deferred.clear();
        };

        while (i < atoms.size()) {
            flush_deferred();
            size_t start = i;
            for (;;) {
                while (start < atoms.size() && is_skippable(atoms[start])) ++start;
                if (start < atoms.size() && (atoms[start].k == Atom::K::Float || atoms[start].k == Atom::K::Abs)) {
                    side(atoms[start], cursor_y, first_line ? indent : 0);
                    ++start;
                    continue;
                }
                break;
            }
            i = start;
            if (start >= atoms.size()) break;

            float ly = cya + cursor_y;
            float lo = cxa, hi = cxa + avail;
            ctx.range(ly, ly + strut_lh, lo, hi);
            float shiftx = lo - cxa, line_avail = hi - lo;
            float ind = first_line ? indent : 0;
            float width_avail = line_avail - ind;

            float  x = 0, trailing = 0;
            size_t end = start, last_opp = start, j = start;
            bool   forced = false;
            for (; j < atoms.size(); ++j) {
                Atom& a = atoms[j];
                if (a.k == Atom::K::Break) { end = j; forced = true; break; }
                if (a.k == Atom::K::Atomic && wrap && j > start) { last_opp = j; }
                if (wrap && a.k != Atom::K::Space && x + a.w > width_avail + 0.01f && last_opp > start) { end = last_opp; goto line_done; }
                x += a.w;
                if (a.k == Atom::K::Space) {
                    if (a.collapsible) { trailing = a.w; if (wrap) last_opp = j + 1; }
                    else { trailing = 0; if (wrap) last_opp = j + 1; }
                } else {
                    trailing = 0;
                    if (a.k == Atom::K::Atomic && wrap) last_opp = j + 1;
                }
            }
            if (!forced) end = j;
        line_done:
            {
                (void)trailing;
                size_t trim_end = end;
                while (trim_end > start && is_skippable(atoms[trim_end - 1])) --trim_end;
                float cw = 0;
                for (size_t k = start; k < trim_end; ++k) cw += atoms[k].w;
                if ((shiftx > 0.01f || line_avail < avail - 0.01f) && cw > width_avail + 0.01f) {
                    // doesn't fit beside the floats: move below the next float edge and try again
                    float ne = ctx.next_edge(ly);
                    if (ne > 0) { cursor_y += ne - ly; continue; }
                }
                Line L;
                bool  last_line = !forced && end >= atoms.size();
                float free_w = std::max(0.f, line_avail - ind - cw);
                float off = 0, extra_per_space = 0;
                switch (cs.text_align) {
                case TextAlign::Center: off = free_w / 2; break;
                case TextAlign::Right: off = free_w; break;
                case TextAlign::Justify:
                    if (!last_line && !forced) {
                        int spaces = 0;
                        for (size_t k = start; k < trim_end; ++k) if (atoms[k].k == Atom::K::Space) ++spaces;
                        if (spaces) extra_per_space = free_w / static_cast<float>(spaces);
                    }
                    break;
                default: if (cs.rtl) off = free_w; break;
                }
                L.x = shiftx + ind + off;
                L.w = cw;

                // vertical extents
                Vert v = strut;
                float top_h = 0, bottom_h = 0;
                for (auto& os : carried) { Vert t = text_vert(*os.box->style); v.asc = std::max(v.asc, t.asc - os.shift); v.desc = std::max(v.desc, t.desc + os.shift); }
                for (size_t k = start; k < trim_end; ++k) {
                    Atom& a = atoms[k];
                    if (a.k == Atom::K::Word || a.k == Atom::K::Space || a.k == Atom::K::Open) {
                        Vert t = text_vert(*a.box->style);
                        v.asc  = std::max(v.asc, t.asc - a.shift);
                        v.desc = std::max(v.desc, t.desc + a.shift);
                    } else if (a.k == Atom::K::Atomic) {
                        VAlign va = a.box->style->vertical_align;
                        if (va == VAlign::Top) top_h = std::max(top_h, a.up + a.down);
                        else if (va == VAlign::Bottom) bottom_h = std::max(bottom_h, a.up + a.down);
                        else { v.asc = std::max(v.asc, a.up - a.shift); v.desc = std::max(v.desc, a.down + a.shift); }
                    }
                }
                float need = std::max(top_h, bottom_h);
                if (need > v.asc + v.desc) {
                    if (bottom_h > top_h) v.asc += need - (v.asc + v.desc); else v.desc += need - (v.asc + v.desc);
                }
                L.y = cursor_y;
                L.baseline = v.asc;
                L.h = v.asc + v.desc;

                // positions
                float px = L.x;
                std::vector<OpenSpan> stack;
                for (auto& os : carried) stack.push_back({os.box, px, false, os.shift});
                Frag* cur = nullptr;
                auto emit_span = [&](const OpenSpan& os, float x1, bool lastseg) {
                    FontMetrics m = tm_.metrics(*os.box->style);
                    Span sp;
                    sp.box = os.box; sp.x = os.x0; sp.w = std::max(0.f, x1 - os.x0);
                    sp.baseline = L.baseline + os.shift; sp.ascent = m.ascent; sp.descent = m.descent;
                    sp.first = os.first; sp.last = lastseg;
                    L.spans.push_back(sp);
                };
                for (size_t k = start; k < end; ++k) {
                    Atom& a = atoms[k];
                    switch (a.k) {
                    case Atom::K::Open:
                        cur = nullptr;
                        stack.push_back({a.box, px, a.first_seg, a.shift});
                        px += a.w;
                        break;
                    case Atom::K::Close:
                        cur = nullptr;
                        px += a.w;
                        for (size_t q = stack.size(); q-- > 0;)
                            if (stack[q].box == a.box) { emit_span(stack[q], px, true); stack.erase(stack.begin() + static_cast<long>(q)); break; }
                        break;
                    case Atom::K::Word: case Atom::K::Space: {
                        if (k >= trim_end) break;
                        float w = a.w + (a.k == Atom::K::Space ? extra_per_space : 0);
                        float fy = L.y + L.baseline + a.shift;
                        if (cur && cur->box == a.box && std::abs(cur->y - fy) < 0.001f) { cur->text += a.text; cur->w += w; }
                        else {
                            Frag f; f.kind = Frag::Kind::Text; f.box = a.box; f.text = a.text; f.x = px; f.y = fy; f.w = w;
                            L.frags.push_back(f);
                            cur = &L.frags.back();
                        }
                        px += w;
                        break;
                    }
                    case Atom::K::Atomic: {
                        cur = nullptr;
                        Box& x = *a.box;
                        float mh = x.margin.top + x.h + x.margin.bottom;
                        VAlign va = x.style->vertical_align;
                        float top_edge = va == VAlign::Top ? L.y : va == VAlign::Bottom ? L.y + L.h - mh : L.y + L.baseline + a.shift - a.up;
                        x.x = px + x.margin.left;
                        x.y = top_edge + x.margin.top;
                        Frag f; f.kind = Frag::Kind::Atomic; f.box = &x; f.x = x.x; f.y = x.y; f.w = x.w;
                        L.frags.push_back(f);
                        px += a.w;
                        break;
                    }
                    case Atom::K::Float: {
                        // a right float met mid-line goes on this line when it fits beside the content; others start the next line
                        bool placed = false;
                        if (a.box->style->float_ == Float::Right) {
                            Box& fl = *a.box;
                            Ctx  dummy2;
                            Opts op;
                            op.shrink = true;
                            BlockOut fo;
                            layout_block(fl, avail, -1, op, LCtx{&dummy2, 0, 0}, fo);
                            float wm = fl.margin.left + fl.w + fl.margin.right;
                            if (shiftx + ind + cw + wm <= line_avail + 0.01f && a.box->style->clear == Clear::None) {
                                place_float(fl, avail, ctx, cxa, cya, L.y);
                                b.inline_floats.push_back(a.box);
                                placed = true;
                            }
                        }
                        if (!placed) deferred.push_back(a.box);
                        a.k = Atom::K::Nop;
                        break;
                    }
                    case Atom::K::Abs: abs_.push_back({a.box, &b, px, L.y}); a.k = Atom::K::Nop; break;
                    default: break;
                    }
                }
                carried.clear();
                for (auto& os : stack) { emit_span(os, px, false); carried.push_back(os); }
                b.lines.push_back(std::move(L));
                cursor_y += b.lines.back().h;
            }
            first_line = false;
            i = forced ? end + 1 : end;
            // <br clear=all> / br { clear: both }: the next line starts below the floats
            if (forced && atoms[end].box && atoms[end].box->style && atoms[end].box->style->clear != Clear::None) {
                flush_deferred();
                float cl = ctx.clear_y(atoms[end].box->style->clear);
                if (cl > -1e30f) cursor_y = std::max(cursor_y, cl - cya);
            }
        }
        flush_deferred();
        return cursor_y;
    }

    // ------------------------------------------------------------ absolute / fixed positioning
    void place_abs(const AbsRec& r) {
        Box&         b = *r.box;
        const Style& s = *b.style;
        float cbx = 0, cby = 0, cbw = env_.viewport_w, cbh = env_.viewport_h;
        if (s.position != Position::Fixed) {
            for (Box* a = b.parent; a; a = a->parent) {
                if (!a->style || a->kind == Box::Kind::Text) continue;
                if (a->style->position != Position::Static && !(a->style->display == Display::Inline && a->replaced == ReplacedKind::None)) {
                    cbx = a->x + a->border.left;
                    cby = a->y + a->border.top;
                    cbw = a->w - a->border.left - a->border.right;
                    cbh = a->h - a->border.top - a->border.bottom;
                    break;
                }
            }
        }
        const Box* p = r.parent;
        float static_x = p->x + p->border.left + p->padding.left + r.sx;
        float static_y = p->y + p->border.top + p->padding.top + p->content_dy + r.sy;

        resolve_edges(b, cbw);
        const float h_extra = hpad(b), v_extra = vpad(b);
        const bool  hasL = !s.inset[3].is_auto(), hasR = !s.inset[1].is_auto(), hasT = !s.inset[0].is_auto(), hasB = !s.inset[2].is_auto();
        const float left = s.inset[3].resolve(cbw), right = s.inset[1].resolve(cbw), top = s.inset[0].resolve(cbh), bottom = s.inset[2].resolve(cbh);
        const bool  auto_ml = s.margin[3].is_auto(), auto_mr = s.margin[1].is_auto(), auto_mt = s.margin[0].is_auto(), auto_mb = s.margin[2].is_auto();
        const bool  replaced = b.replaced != ReplacedKind::None;
        float       ml = auto_ml ? 0 : b.margin.left, mr = auto_mr ? 0 : b.margin.right;
        float       mt = auto_mt ? 0 : b.margin.top, mb = auto_mb ? 0 : b.margin.bottom;

        Opts o;
        if (!replaced && s.width.is_auto()) {
            float cw;
            if (hasL && hasR) cw = cbw - left - right - ml - mr - h_extra;
            else {
                float avail = cbw - (hasL ? left : (hasR ? 0 : static_x - cbx)) - (hasR ? right : 0) - ml - mr - h_extra;
                Intrinsic in = intrinsic(b);
                cw = std::min(std::max(in.min, avail), in.max);
            }
            float minw = s.min_width.resolve(cbw, 0), maxw = s.max_width.is_none() ? -1.f : s.max_width.resolve(cbw);
            if (s.box_sizing == BoxSizing::BorderBox) { minw = std::max(0.f, minw - h_extra); if (maxw >= 0) maxw = std::max(0.f, maxw - h_extra); }
            if (maxw >= 0 && cw > maxw) cw = maxw;
            cw = std::max(cw, minw);
            o.forced_cw = std::max(0.f, cw);
        }
        if (!replaced && s.height.is_auto() && hasT && hasB) o.forced_ch = std::max(0.f, cbh - top - bottom - mt - mb - v_extra);

        b.skipped = false;
        b.x = b.y = 0;
        BlockOut out;
        Ctx      dummy;
        layout_block(b, cbw, cbh, o, LCtx{&dummy, 0, 0}, out);

        float px, py;
        if (hasL && hasR) {
            float slack = cbw - left - right - b.w - ml - mr;
            if (auto_ml && auto_mr) { if (slack >= 0) ml = mr = slack / 2; else { ml = 0; mr = slack; } }
            else if (auto_ml) ml = slack;
            else if (auto_mr) mr = slack;
            px = cbx + left + ml;
        } else if (hasL) px = cbx + left + ml;
        else if (hasR) px = cbx + cbw - right - mr - b.w;
        else px = static_x + ml;
        if (hasT && hasB) {
            float slack = cbh - top - bottom - b.h - mt - mb;
            if (auto_mt && auto_mb) { if (slack >= 0) mt = mb = slack / 2; else { mt = 0; mb = slack; } }
            else if (auto_mt) mt = slack;
            else if (auto_mb) mb = slack;
            py = cby + top + mt;
        } else if (hasT) py = cby + top + mt;
        else if (hasB) py = cby + cbh - bottom - mb - b.h;
        else py = static_y + mt;
        finalize(b, px - b.x, py);
    }

    // ------------------------------------------------------------ flexbox
    struct FItem {
        Box*  b = nullptr;
        float hp = 0, vp = 0;               // padding+border, horizontal / vertical
        float ml = 0, mr = 0, mt = 0, mb = 0; // used margins (auto = 0)
        bool  am_l = false, am_r = false, am_t = false, am_b = false;
        float basis = 0, minm = 0, maxm = 1e30f, hyp = 0, main = 0; // inner main sizes
        float grow = 0, shrink = 1;
        bool  frozen = false;
        Align align = Align::Stretch;
        float cross_out = 0;                // outer cross size
        float xs = 0, ys = 0;               // outer-box position
        float asc = -1;                     // baseline offset from the margin-box top
        int   order = 0;
    };

    static float gap_of(const Length& l, float base) { return l.is_px() ? l.v : l.is_pct() ? l.v * base / 100.f : 0.f; }

    // Flexes the main sizes of one line to `avail` (content box minus gaps).
    static void resolve_flex(std::vector<FItem*>& line, float avail, bool row_outer_extras_main_h) {
        float sum_hyp = 0;
        auto extras = [&](FItem* it) { return row_outer_extras_main_h ? it->hp + it->ml + it->mr : it->vp + it->mt + it->mb; };
        for (auto* it : line) sum_hyp += it->hyp + extras(it);
        bool growing = sum_hyp < avail;
        for (auto* it : line) {
            it->main = it->hyp;
            it->frozen = (growing ? it->grow <= 0 : it->shrink <= 0) || (growing ? it->basis > it->hyp : it->basis < it->hyp);
            if (it->frozen) it->main = it->hyp;
        }
        for (size_t iter = 0; iter <= line.size() + 1; ++iter) {
            float used = 0, tot_grow = 0, tot_shrink = 0;
            for (auto* it : line) {
                used += extras(it) + (it->frozen ? it->main : it->basis);
                if (!it->frozen) { tot_grow += it->grow; tot_shrink += it->shrink * it->basis; }
            }
            float remaining = avail - used;
            bool any = false;
            for (auto* it : line) if (!it->frozen) any = true;
            if (!any) break;
            if (growing) { if (tot_grow < 1) remaining = std::min(remaining, remaining * tot_grow); }
            float viol = 0;
            for (auto* it : line) {
                if (it->frozen) continue;
                float target = it->basis;
                if (growing) target += tot_grow > 0 ? remaining * it->grow / std::max(tot_grow, 1.f) : 0;
                else target += tot_shrink > 0 ? remaining * (it->shrink * it->basis) / tot_shrink : 0;
                float clamped = std::min(std::max(target, it->minm), it->maxm);
                viol += clamped - target;
                it->main = clamped;
            }
            bool froze = false;
            for (auto* it : line) {
                if (it->frozen) continue;
                float target_clamped = it->main;
                (void)target_clamped;
            }
            for (auto* it : line) {
                if (it->frozen) continue;
                bool min_v = it->main <= it->minm + 0.001f && it->minm > 0, max_v = it->main >= it->maxm - 0.001f;
                if (viol == 0 || (viol > 0 && min_v) || (viol < 0 && max_v)) { it->frozen = true; froze = true; }
            }
            if (!froze) break;
        }
    }

    Intrinsic flex_intrinsic(Box& b) {
        const Style& s = *b.style;
        bool row = s.flex_direction == FlexDirection::Row || s.flex_direction == FlexDirection::RowReverse;
        Intrinsic r;
        int n = 0;
        float gap = gap_of(s.column_gap, 0);
        for (auto& c : b.children) {
            if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak || c->kind == Box::Kind::Marker || is_abs(*c)) continue;
            Intrinsic ci = intrinsic(*c);
            float e = outer_h_edges(*c, *c->style);
            ++n;
            if (row) {
                r.max += ci.max + e;
                if (s.flex_wrap == FlexWrap::NoWrap) r.min += ci.min + e; else r.min = std::max(r.min, ci.min + e);
            } else {
                r.min = std::max(r.min, ci.min + e);
                r.max = std::max(r.max, ci.max + e);
            }
        }
        if (row && n > 1) { r.max += gap * static_cast<float>(n - 1); if (s.flex_wrap == FlexWrap::NoWrap) r.min += gap * static_cast<float>(n - 1); }
        return r;
    }

    // ============================================================ CSS grid
    struct GItem {
        Box*  b = nullptr;
        int   rs = -1, re = -1, cs = -1, ce = -1; // resolved lines (0-based, end exclusive) once placed
        int   rspan = 1, cspan = 1;
        bool  rdef = false, cdef = false;
        int   order = 0;
        float hp = 0, vp = 0, ml = 0, mr = 0, mt = 0, mb = 0;
        bool  am_l = false, am_r = false, am_t = false, am_b = false;
        float amin = 0, mc = 0, Mc = 0; // column contributions (outer): automatic minimum, min-content, max-content
        float w = 0;                     // used content width
        float hout = 0;                  // row contribution (outer height)
        uint8_t js = 0, as = 0;          // effective justify/align (0 normal, 1 start, 2 end, 3 center, 4 stretch)
    };
    struct GTr {
        GTrack def;
        float  base = 0, limit = 0, pos = 0;
        bool   flex = false, collapsed = false;
        float  fr = 0, lim_acc = -1;
    };
    struct GModel {
        std::vector<GItem> items;
        std::vector<GTr>   cols, rows;
        int ecols = 0, erows = 0; // explicit track counts
    };

    static const GridContainer& default_grid() {
        static const GridContainer g;
        return g;
    }

    static bool grid_name_line(const GTrackList& l, const std::string& name, int occurrence, int& out) {
        int seen = 0;
        for (size_t i = 0; i < l.names.size(); ++i)
            for (auto& n : l.names[i])
                if (n == name && ++seen == occurrence) { out = static_cast<int>(i); return true; }
        return false;
    }
    static bool grid_area_bounds(const GridContainer& g, const std::string& name, int& r0, int& r1, int& c0, int& c1) {
        r0 = c0 = 1 << 30; r1 = c1 = -1;
        for (size_t r = 0; r < g.areas.size(); ++r)
            for (size_t c = 0; c < g.areas[r].size(); ++c)
                if (g.areas[r][c] == name) {
                    r0 = std::min(r0, static_cast<int>(r)); r1 = std::max(r1, static_cast<int>(r) + 1);
                    c0 = std::min(c0, static_cast<int>(c)); c1 = std::max(c1, static_cast<int>(c) + 1);
                }
        return r1 >= 0;
    }

    // One line reference -> 0-based line index. Returns false when it is auto / a span / unresolvable.
    static bool grid_line_index(const GLine& l, const GTrackList& tl, const GridContainer& g, int explicit_n, bool end, bool col, int& out) {
        if (l.k == 1) {
            if (l.n > 0) { out = l.n - 1; return true; }
            if (l.n < 0) { out = std::max(0, explicit_n + 1 + l.n); return true; }
            return false;
        }
        if (l.k == 3) {
            if (grid_name_line(tl, l.name, l.n, out)) return true;
            int r0, r1, c0, c1;
            std::string base = l.name;
            bool as_start = false, as_end = false;
            if (base.size() > 6 && base.compare(base.size() - 6, 6, "-start") == 0) { base.resize(base.size() - 6); as_start = true; }
            else if (base.size() > 4 && base.compare(base.size() - 4, 4, "-end") == 0) { base.resize(base.size() - 4); as_end = true; }
            if (grid_area_bounds(g, base, r0, r1, c0, c1)) {
                bool use_end = as_end || (!as_start && end);
                out = col ? (use_end ? c1 : c0) : (use_end ? r1 : r0);
                return true;
            }
        }
        return false;
    }

    // Resolves start/end of one axis. `def` = position is definite; span = number of tracks.
    static void grid_axis(const GLine& s, const GLine& e, const GTrackList& tl, const GridContainer& g, int explicit_n, bool col, bool& def, int& a0, int& span) {
        int i0 = 0, i1 = 0;
        bool hs = grid_line_index(s, tl, g, explicit_n, false, col, i0), he = grid_line_index(e, tl, g, explicit_n, true, col, i1);
        auto spn = [](const GLine& l) { return l.k == 2 ? std::max(1, l.n) : 1; };
        def = false; a0 = 0; span = 1;
        if (hs && he) {
            if (i1 < i0) std::swap(i0, i1);
            if (i1 == i0) i1 = i0 + 1;
            def = true; a0 = i0; span = i1 - i0;
        } else if (hs) {
            def = true; a0 = i0; span = e.k == 2 ? spn(e) : 1;
        } else if (he) {
            def = true; span = s.k == 2 ? spn(s) : 1; a0 = std::max(0, i1 - span);
        } else {
            span = s.k == 2 ? spn(s) : (e.k == 2 ? spn(e) : 1);
        }
        if (span > 1000) span = 1000;
    }

    // Expands a template track list (auto-fill / auto-fit repeat counted against `avail`).
    static void grid_expand(const GTrackList& tl, float avail, float gap, std::vector<GTr>& out) {
        size_t before = tl.auto_repeat ? tl.auto_at : tl.tracks.size();
        for (size_t i = 0; i < before; ++i) { GTr t; t.def = tl.tracks[i]; out.push_back(t); }
        if (!tl.auto_repeat) return;
        auto sz = [&](const GTrack& t) {
            if (t.max.k == 1) return t.max.v;
            if (t.max.k == 2 && avail >= 0) return t.max.v * avail / 100.f;
            if (t.min.k == 1) return t.min.v;
            if (t.min.k == 2 && avail >= 0) return t.min.v * avail / 100.f;
            return 0.f;
        };
        float fixed = 0, rep = 0;
        for (auto& t : tl.tracks) fixed += sz(t);
        for (auto& t : tl.rep) rep += sz(t);
        int n = 1;
        float per = rep + gap * static_cast<float>(tl.rep.size());
        if (avail >= 0 && per > 0.01f) {
            float room = avail - fixed - gap * static_cast<float>(tl.tracks.size());
            n = static_cast<int>(std::floor((room + gap) / per + 1e-4f));
            n = std::max(1, std::min(n, 500));
        }
        for (int k = 0; k < n; ++k)
            for (auto& t : tl.rep) { GTr x; x.def = t; out.push_back(x); }
        for (size_t i = tl.auto_at; i < tl.tracks.size(); ++i) { GTr x; x.def = tl.tracks[i]; out.push_back(x); }
    }

    GModel grid_model(Box& b, float cw) {
        const Style& s = *b.style;
        const GridContainer& g = s.grid ? *s.grid : default_grid();
        GModel m;
        const float cgap = gap_of(s.column_gap, cw);
        grid_expand(g.cols, cw, cgap, m.cols);
        m.ecols = static_cast<int>(m.cols.size());
        grid_expand(g.rows, -1.f, 0.f, m.rows);
        m.erows = static_cast<int>(m.rows.size());
        // named areas also define the explicit extent
        if (!g.areas.empty()) {
            m.erows = std::max(m.erows, static_cast<int>(g.areas.size()));
            m.ecols = std::max(m.ecols, static_cast<int>(g.areas[0].size()));
        }
        auto fill_implicit = [&](std::vector<GTr>& tr, int upto, int explicit_n, const std::vector<GTrack>& autos) {
            while (static_cast<int>(tr.size()) < upto) {
                GTr t;
                size_t k = tr.size() >= static_cast<size_t>(explicit_n) ? tr.size() - static_cast<size_t>(explicit_n) : 0;
                if (!autos.empty()) t.def = autos[k % autos.size()];
                tr.push_back(t);
            }
        };
        fill_implicit(m.rows, m.erows, m.erows, g.auto_rows);
        fill_implicit(m.cols, m.ecols, m.ecols, g.auto_cols);

        // collect items
        int seq = 0;
        for (auto& cp : b.children) {
            Box& c = *cp;
            if (c.kind == Box::Kind::Text || c.kind == Box::Kind::LineBreak || c.kind == Box::Kind::Marker) continue;
            if (is_abs(c)) { c.skipped = true; c.w = c.h = 0; abs_.push_back({&c, &b, 0, 0}); continue; }
            GItem it;
            it.b = &c;
            it.order = c.style->order * 100000 + seq++;
            GridItem def_item;
            const GridItem& gi = c.style->gitem ? *c.style->gitem : def_item;
            GLine rs = gi.rs, re = gi.re, cs = gi.cs, ce = gi.ce;
            if (!gi.area.empty()) {
                int r0, r1, c0, c1;
                if (grid_area_bounds(g, gi.area, r0, r1, c0, c1)) {
                    rs = GLine{1, r0 + 1, ""}; re = GLine{1, r1 + 1, ""}; cs = GLine{1, c0 + 1, ""}; ce = GLine{1, c1 + 1, ""};
                }
            }
            grid_axis(rs, re, g.rows, g, m.erows, false, it.rdef, it.rs, it.rspan);
            grid_axis(cs, ce, g.cols, g, m.ecols, true, it.cdef, it.cs, it.cspan);
            it.js = gi.justify_self ? gi.justify_self : g.justify_items;
            it.as = gi.align_self ? gi.align_self : g.align_items;
            m.items.push_back(std::move(it));
        }
        std::stable_sort(m.items.begin(), m.items.end(), [](const GItem& a, const GItem& b2) { return a.order < b2.order; });

        // ---- auto placement (generic over the flow direction: "major" = the axis the cursor wraps onto)
        const bool colflow = g.flow_col;
        auto major_def = [&](const GItem& it) { return colflow ? it.cdef : it.rdef; };
        auto minor_def = [&](const GItem& it) { return colflow ? it.rdef : it.cdef; };
        auto major_pos = [&](GItem& it) -> int& { return colflow ? it.cs : it.rs; };
        auto minor_pos = [&](GItem& it) -> int& { return colflow ? it.rs : it.cs; };
        auto major_span = [&](const GItem& it) { return colflow ? it.cspan : it.rspan; };
        auto minor_span = [&](const GItem& it) { return colflow ? it.rspan : it.cspan; };
        auto set_major_def = [&](GItem& it) { (colflow ? it.cdef : it.rdef) = true; };
        auto set_minor_def = [&](GItem& it) { (colflow ? it.rdef : it.cdef) = true; };
        const int e_minor = std::max(1, colflow ? m.erows : m.ecols);
        std::vector<std::vector<char>> occ; // [major][minor]
        auto occupied = [&](int mj, int mi, int sj, int si) {
            for (int a = mj; a < mj + sj; ++a) {
                if (a >= static_cast<int>(occ.size())) break;
                const auto& row = occ[static_cast<size_t>(a)];
                for (int c2 = mi; c2 < mi + si; ++c2)
                    if (c2 < static_cast<int>(row.size()) && row[static_cast<size_t>(c2)]) return true;
            }
            return false;
        };
        auto mark = [&](int mj, int mi, int sj, int si) {
            if (mj + sj > 2000 || mi + si > 2000) return;
            if (static_cast<int>(occ.size()) < mj + sj) occ.resize(static_cast<size_t>(mj + sj));
            for (int a = mj; a < mj + sj; ++a) {
                auto& row = occ[static_cast<size_t>(a)];
                if (static_cast<int>(row.size()) < mi + si) row.resize(static_cast<size_t>(mi + si), 0);
                for (int c2 = mi; c2 < mi + si; ++c2) row[static_cast<size_t>(c2)] = 1;
            }
        };
        std::vector<char> placed(m.items.size(), 0);
        for (size_t i = 0; i < m.items.size(); ++i) { // both definite
            GItem& it = m.items[i];
            if (major_def(it) && minor_def(it)) {
                mark(major_pos(it), minor_pos(it), major_span(it), minor_span(it));
                placed[i] = 1;
            }
        }
        std::unordered_map<int, int> last_minor;
        for (size_t i = 0; i < m.items.size(); ++i) { // definite major position
            GItem& it = m.items[i];
            if (placed[i] || !major_def(it)) continue;
            int mj = major_pos(it), sj = major_span(it), si = minor_span(it);
            int mi = 0;
            if (!g.dense)
                for (int a = mj; a < mj + sj; ++a) { auto f = last_minor.find(a); if (f != last_minor.end()) mi = std::max(mi, f->second); }
            while (occupied(mj, mi, sj, si) && mi < 2000) ++mi;
            minor_pos(it) = mi;
            set_minor_def(it);
            mark(mj, mi, sj, si);
            for (int a = mj; a < mj + sj; ++a) last_minor[a] = mi + si;
            placed[i] = 1;
        }
        int cur_mj = 0, cur_mi = 0;
        for (size_t i = 0; i < m.items.size(); ++i) { // the rest, with the auto-placement cursor
            GItem& it = m.items[i];
            if (placed[i]) continue;
            int sj = major_span(it), si = minor_span(it);
            if (g.dense) { cur_mj = 0; cur_mi = 0; }
            if (minor_def(it)) {
                int mi = minor_pos(it);
                if (!g.dense && mi < cur_mi) ++cur_mj;
                cur_mi = mi;
                while (occupied(cur_mj, cur_mi, sj, si) && cur_mj < 2000) ++cur_mj;
                major_pos(it) = cur_mj;
                set_major_def(it);
                mark(cur_mj, cur_mi, sj, si);
            } else {
                const int bound = std::max(e_minor, si);
                for (int guard = 0; guard < 100000; ++guard) {
                    if (cur_mi + si > bound) { ++cur_mj; cur_mi = 0; continue; }
                    if (!occupied(cur_mj, cur_mi, sj, si)) break;
                    ++cur_mi;
                }
                major_pos(it) = cur_mj;
                minor_pos(it) = cur_mi;
                set_major_def(it);
                set_minor_def(it);
                mark(cur_mj, cur_mi, sj, si);
                cur_mi += si;
            }
            placed[i] = 1;
        }
        int ncols = m.ecols, nrows = m.erows;
        for (auto& it : m.items) {
            it.re = it.rs + it.rspan;
            it.ce = it.cs + it.cspan;
            ncols = std::max(ncols, it.ce);
            nrows = std::max(nrows, it.re);
        }
        ncols = std::min(ncols, 2000);
        nrows = std::min(nrows, 2000);
        for (auto& it : m.items) {
            it.ce = std::min(it.ce, ncols); it.re = std::min(it.re, nrows);
            it.cs = std::min(it.cs, ncols - 1); it.rs = std::min(it.rs, nrows - 1);
        }
        fill_implicit(m.cols, ncols, m.ecols, g.auto_cols);
        fill_implicit(m.rows, nrows, m.erows, g.auto_rows);
        // auto-fit: repeated tracks that received no items collapse
        auto collapse = [&](const GTrackList& tl, std::vector<GTr>& tr, bool col) {
            if (!tl.auto_repeat || !tl.auto_fit) return;
            size_t n_rep = tr.size() >= tl.tracks.size() ? tr.size() - tl.tracks.size() : 0;
            size_t first = tl.auto_at, last = tl.auto_at + n_rep;
            for (size_t t = first; t < last && t < tr.size(); ++t) {
                bool used = false;
                for (auto& it : m.items) {
                    int a = col ? it.cs : it.rs, e = col ? it.ce : it.re;
                    if (static_cast<int>(t) >= a && static_cast<int>(t) < e) { used = true; break; }
                }
                if (!used) tr[t].collapsed = true;
            }
        };
        collapse(g.cols, m.cols, true);
        collapse(g.rows, m.rows, false);
        return m;
    }

    struct ASpan { int s, e; float amin, mc, Mc; };

    // Sizes one axis of a grid. mode: 0 = definite `avail`, 1 = min-content, 2 = max-content.
    void grid_size_tracks(std::vector<GTr>& tr, const std::vector<ASpan>& items, float gap, float avail, int mode, bool stretch_auto) {
        const size_t n = tr.size();
        if (n == 0) return;
        const float inf = 1e30f;
        auto gaps_between = [&](int s, int e) {
            float g = 0;
            for (int t = s; t < e - 1; ++t) if (!tr[static_cast<size_t>(t)].collapsed && !tr[static_cast<size_t>(t + 1)].collapsed) g += gap;
            return g;
        };
        for (auto& t : tr) {
            t.base = 0; t.limit = inf; t.flex = false; t.fr = 0; t.lim_acc = -1;
            GBreadth mn = t.def.min, mx = t.def.max;
            if (mn.k == 2 && avail < 0) mn = GBreadth{0, 0};
            if (mx.k == 2 && avail < 0) mx = GBreadth{0, 0};
            if (mn.k == 1) t.base = mn.v;
            else if (mn.k == 2) t.base = mn.v * avail / 100.f;
            if (mx.k == 1) t.limit = mx.v;
            else if (mx.k == 2) t.limit = mx.v * avail / 100.f;
            else if (mx.k == 3) { t.flex = true; t.fr = std::max(0.f, mx.v); }
            if (t.limit < t.base) t.limit = t.base;
            if (t.collapsed) { t.base = 0; t.limit = 0; t.flex = false; }
        }
        auto pct_auto = [&](const GBreadth& b2) { return b2.k == 2 && avail < 0; };
        auto min_intrinsic = [&](const GTr& t) { return !t.collapsed && (t.def.min.k == 0 || t.def.min.k == 4 || t.def.min.k == 5 || pct_auto(t.def.min)); };
        auto max_intrinsic = [&](const GTr& t) { return !t.collapsed && !t.flex && (t.def.max.k == 0 || t.def.max.k == 4 || t.def.max.k == 5 || pct_auto(t.def.max)); };
        auto min_contrib = [&](const GTr& t, const ASpan& a) { return t.def.min.k == 4 ? a.mc : t.def.min.k == 5 ? a.Mc : a.amin; };
        auto max_contrib = [&](const GTr& t, const ASpan& a) { return t.def.max.k == 4 ? a.mc : a.Mc; };
        std::vector<const ASpan*> order;
        for (auto& a : items) if (a.s >= 0 && a.e > a.s && a.e <= static_cast<int>(n)) order.push_back(&a);
        std::stable_sort(order.begin(), order.end(), [](const ASpan* x, const ASpan* y) { return (x->e - x->s) < (y->e - y->s); });
        for (const ASpan* ap : order) {
            const ASpan& a = *ap;
            int span = a.e - a.s;
            if (span == 1) {
                GTr& t = tr[static_cast<size_t>(a.s)];
                if (t.collapsed) continue;
                if (min_intrinsic(t)) t.base = std::max(t.base, min_contrib(t, a));
                if (max_intrinsic(t)) t.lim_acc = std::max(t.lim_acc, max_contrib(t, a));
                if (t.flex) t.lim_acc = std::max(t.lim_acc, a.Mc); // remembered for the flex fraction
                continue;
            }
            bool has_flex = false;
            for (int t = a.s; t < a.e; ++t) if (tr[static_cast<size_t>(t)].flex) has_flex = true;
            if (!has_flex) {
                float sum = gaps_between(a.s, a.e);
                int cnt = 0;
                for (int t = a.s; t < a.e; ++t) { sum += tr[static_cast<size_t>(t)].base; if (min_intrinsic(tr[static_cast<size_t>(t)])) ++cnt; }
                float need = a.amin - sum;
                if (need > 0 && cnt > 0)
                    for (int t = a.s; t < a.e; ++t) if (min_intrinsic(tr[static_cast<size_t>(t)])) tr[static_cast<size_t>(t)].base += need / static_cast<float>(cnt);
                sum = gaps_between(a.s, a.e);
                cnt = 0;
                for (int t = a.s; t < a.e; ++t) {
                    const GTr& x = tr[static_cast<size_t>(t)];
                    sum += std::max(x.base, x.lim_acc >= 0 ? x.lim_acc : (x.limit < inf ? x.limit : 0.f));
                    if (max_intrinsic(x)) ++cnt;
                }
                need = a.Mc - sum;
                if (need > 0 && cnt > 0)
                    for (int t = a.s; t < a.e; ++t)
                        if (max_intrinsic(tr[static_cast<size_t>(t)])) { GTr& x = tr[static_cast<size_t>(t)]; x.lim_acc = std::max(x.base, x.lim_acc) + need / static_cast<float>(cnt); }
            } else {
                int fcount = 0;
                float rest = a.Mc - gaps_between(a.s, a.e);
                for (int t = a.s; t < a.e; ++t) { if (tr[static_cast<size_t>(t)].flex) ++fcount; else rest -= tr[static_cast<size_t>(t)].base; }
                if (fcount > 0 && rest > 0)
                    for (int t = a.s; t < a.e; ++t) if (tr[static_cast<size_t>(t)].flex) tr[static_cast<size_t>(t)].lim_acc = std::max(tr[static_cast<size_t>(t)].lim_acc, rest / static_cast<float>(fcount));
            }
        }
        for (auto& t : tr) {
            if (t.collapsed || t.flex) continue;
            if (max_intrinsic(t)) t.limit = std::max(t.base, t.lim_acc >= 0 ? t.lim_acc : t.base);
            else if (t.limit >= inf) t.limit = t.base;
            if (t.def.fit >= 0) t.limit = std::max(t.base, std::min(t.limit, t.def.fit));
            if (t.limit < t.base) t.limit = t.base;
        }
        float total_gaps = 0;
        {
            bool prev = false;
            for (size_t i = 0; i < n; ++i) { if (tr[i].collapsed) continue; if (prev) total_gaps += gap; prev = true; }
        }
        if (mode == 1) return;
        const bool definite = mode == 0 && avail >= 0;
        auto sum_base = [&]() { float s2 = 0; for (auto& t : tr) s2 += t.base; return s2; };
        if (definite) {
            float free = avail - sum_base() - total_gaps;
            for (int it = 0; it < static_cast<int>(n) + 2 && free > 0.01f; ++it) {
                int cnt = 0;
                for (auto& t : tr) if (!t.flex && !t.collapsed && t.base < t.limit - 0.001f) ++cnt;
                if (!cnt) break;
                float share = free / static_cast<float>(cnt), used = 0;
                for (auto& t : tr)
                    if (!t.flex && !t.collapsed && t.base < t.limit - 0.001f) { float add = std::min(share, t.limit - t.base); t.base += add; used += add; }
                free -= used;
                if (used < 0.001f) break;
            }
        } else {
            for (auto& t : tr) if (!t.flex && t.limit < inf) t.base = std::max(t.base, t.limit);
        }
        float sumfr = 0;
        for (auto& t : tr) if (t.flex) sumfr += t.fr;
        if (sumfr > 0) {
            float fr_size = 0;
            if (definite) {
                std::vector<char> inflex(n, 0);
                for (int iter = 0; iter < static_cast<int>(n) + 2; ++iter) {
                    float left = avail - total_gaps, sf = 0;
                    for (size_t i = 0; i < n; ++i) { if (tr[i].flex && !inflex[i]) sf += tr[i].fr; else left -= tr[i].base; }
                    fr_size = sf > 0 ? std::max(0.f, left) / std::max(sf, 1.f) : 0;
                    bool changed = false;
                    for (size_t i = 0; i < n; ++i)
                        if (tr[i].flex && !inflex[i] && tr[i].fr * fr_size < tr[i].base - 0.001f) { inflex[i] = 1; changed = true; }
                    if (!changed) break;
                }
            } else {
                for (auto& t : tr) {
                    if (!t.flex || t.fr <= 0) continue;
                    fr_size = std::max(fr_size, t.base / std::max(t.fr, 1.f));
                    if (t.lim_acc >= 0) fr_size = std::max(fr_size, t.lim_acc / std::max(t.fr, 1.f));
                }
                for (const ASpan* ap : order) {
                    if (ap->e - ap->s < 2) continue;
                    float sf = 0, rest = ap->Mc - gaps_between(ap->s, ap->e);
                    bool any = false;
                    for (int t = ap->s; t < ap->e; ++t) {
                        if (tr[static_cast<size_t>(t)].flex) { sf += tr[static_cast<size_t>(t)].fr; any = true; }
                        else rest -= tr[static_cast<size_t>(t)].base;
                    }
                    if (any && sf > 0) fr_size = std::max(fr_size, rest / std::max(sf, 1.f));
                }
            }
            for (auto& t : tr) if (t.flex) t.base = std::max(t.base, t.fr * fr_size);
        }
        if (definite && stretch_auto) {
            float free = avail - sum_base() - total_gaps;
            if (free > 0.01f) {
                int cnt = 0;
                for (auto& t : tr) if (!t.collapsed && !t.flex && t.def.max.k == 0 && t.def.fit < 0) ++cnt;
                if (cnt) for (auto& t : tr) if (!t.collapsed && !t.flex && t.def.max.k == 0 && t.def.fit < 0) t.base += free / static_cast<float>(cnt);
            }
        }
    }

    // Edges and column contributions of an item (outer, margins included).
    void grid_contrib(GItem& it, float cw) {
        Box& c = *it.b;
        const Style& cs = *c.style;
        resolve_edges(c, cw < 0 ? 0 : cw);
        it.hp = hpad(c); it.vp = vpad(c);
        it.am_l = cs.margin[3].is_auto(); it.am_r = cs.margin[1].is_auto(); it.am_t = cs.margin[0].is_auto(); it.am_b = cs.margin[2].is_auto();
        it.ml = it.am_l ? 0 : c.margin.left; it.mr = it.am_r ? 0 : c.margin.right;
        it.mt = it.am_t ? 0 : c.margin.top; it.mb = it.am_b ? 0 : c.margin.bottom;
        Intrinsic in = intrinsic(c);
        float edges = it.hp + it.ml + it.mr;
        bool bb = cs.box_sizing == BoxSizing::BorderBox;
        float base = cw < 0 ? 0 : cw;
        auto inner = [&](const Length& l) { float v = l.resolve(base); return bb ? std::max(0.f, v - it.hp) : v; };
        float minw = 0;
        if (cs.min_width.is_px() || cs.min_width.is_pct()) minw = inner(cs.min_width);
        else if (cs.overflow_x == Overflow::Visible) minw = in.min;
        float maxw = (cs.max_width.is_px() || cs.max_width.is_pct()) ? inner(cs.max_width) : 1e30f;
        float mx = in.max, mn = in.min;
        if (cs.width.is_px()) { float w = inner(cs.width); mx = mn = w; if (!(cs.min_width.is_px() || cs.min_width.is_pct())) minw = std::min(minw, w); }
        mx = std::min(mx, maxw); mn = std::min(mn, maxw);
        minw = std::min(minw, maxw);
        it.amin = std::min(std::max(minw, 0.f), std::max(mx, minw)) + edges;
        if (cs.overflow_x != Overflow::Visible && !(cs.min_width.is_px() || cs.min_width.is_pct())) it.amin = edges;
        it.mc = std::max(mn, minw) + edges;
        it.Mc = std::max(mx, minw) + edges;
        it.amin = std::min(it.amin, it.Mc);
    }

    static float grid_span_size(const std::vector<GTr>& tr, int s, int e, float gap) {
        float sum = 0;
        for (int t = s; t < e; ++t) sum += tr[static_cast<size_t>(t)].base;
        for (int t = s; t < e - 1; ++t) if (!tr[static_cast<size_t>(t)].collapsed && !tr[static_cast<size_t>(t + 1)].collapsed) sum += gap;
        return sum;
    }

    Intrinsic grid_intrinsic(Box& b) {
        const Style& s = *b.style;
        GModel m = grid_model(b, -1);
        const float cgap = gap_of(s.column_gap, 0);
        std::vector<ASpan> spans;
        for (auto& it : m.items) { grid_contrib(it, -1); spans.push_back({it.cs, it.ce, it.amin, it.mc, it.Mc}); }
        std::vector<GTr> a = m.cols, c2 = m.cols;
        grid_size_tracks(a, spans, cgap, -1, 1, false);
        grid_size_tracks(c2, spans, cgap, -1, 2, false);
        auto total = [&](const std::vector<GTr>& tr) {
            float sum = 0;
            bool prev = false;
            for (auto& t : tr) { if (t.collapsed) continue; sum += t.base + (prev ? cgap : 0); prev = true; }
            return sum;
        };
        Intrinsic r;
        r.min = total(a);
        r.max = std::max(r.min, total(c2));
        return r;
    }

    // Lays out the grid items of b; returns the content height. Item positions are relative to b's content origin.
    float grid_content(Box& b, float cw, float definite_h, float minh, float maxh) {
        const Style& s = *b.style;
        const GridContainer& g = s.grid ? *s.grid : default_grid();
        const float cgap = gap_of(s.column_gap, cw);
        GModel m = grid_model(b, cw);
        if (m.items.empty() && m.cols.empty() && m.rows.empty()) return definite_h >= 0 ? definite_h : 0;
        // ---- columns
        std::vector<ASpan> spans;
        for (auto& it : m.items) { grid_contrib(it, cw); spans.push_back({it.cs, it.ce, it.amin, it.mc, it.Mc}); }
        const bool cstretch = g.justify_content == 0 || g.justify_content == 4;
        const bool rstretch = g.align_content == 0 || g.align_content == 4;
        grid_size_tracks(m.cols, spans, cgap, cw, 0, cstretch);
        auto distribute = [&](std::vector<GTr>& tr, float gap, float avail, uint8_t content) {
            float total = 0;
            bool prev = false;
            size_t cnt = 0;
            for (auto& t : tr) { if (t.collapsed) continue; total += t.base + (prev ? gap : 0); prev = true; ++cnt; }
            float free = avail - total, off = 0, extra = 0;
            if (free > 0) switch (content) {
                case 2: off = free; break;
                case 3: off = free / 2; break;
                case 5: if (cnt > 1) extra = free / static_cast<float>(cnt - 1); break;
                case 6: if (cnt) { extra = free / static_cast<float>(cnt); off = extra / 2; } break;
                case 7: if (cnt) { extra = free / static_cast<float>(cnt + 1); off = extra; } break;
                default: break;
            }
            float x = off;
            prev = false;
            for (auto& t : tr) {
                if (t.collapsed) { t.pos = x; continue; }
                if (prev) x += gap + extra;
                t.pos = x;
                x += t.base;
                prev = true;
            }
            return total;
        };
        distribute(m.cols, cgap, cw, g.justify_content);
        // ---- item widths and natural heights
        const float rgap = gap_of(s.row_gap, definite_h >= 0 ? definite_h : 0);
        for (auto& it : m.items) {
            Box& c = *it.b;
            const Style& cs = *c.style;
            float area_w = grid_span_size(m.cols, it.cs, it.ce, cgap);
            bool replaced = c.replaced != ReplacedKind::None;
            bool stretch = it.js == 4 || (it.js == 0 && !replaced);
            bool bb = cs.box_sizing == BoxSizing::BorderBox;
            float avail_cw = std::max(0.f, area_w - it.ml - it.mr - it.hp);
            float w;
            if (cs.width.is_px() || cs.width.is_pct()) { w = cs.width.resolve(area_w); if (bb) w = std::max(0.f, w - it.hp); }
            else if (stretch) w = avail_cw;
            else { Intrinsic in = intrinsic(c); w = std::min(std::max(in.min, avail_cw), in.max); }
            float maxw = cs.max_width.is_none() ? -1.f : cs.max_width.resolve(area_w), minw = cs.min_width.resolve(area_w, 0);
            if (bb) { if (maxw >= 0) maxw = std::max(0.f, maxw - it.hp); minw = std::max(0.f, minw - it.hp); }
            if (maxw >= 0 && w > maxw) w = maxw;
            if (w < minw) w = minw;
            it.w = w;
            BlockOut co;
            Opts oo; oo.forced_cw = w; oo.bfc_root = true;
            Ctx dummy;
            layout_block(c, area_w, -1, oo, LCtx{&dummy, 0, 0}, co);
            it.hout = c.h + it.mt + it.mb;
        }
        // ---- rows
        std::vector<ASpan> rspans;
        for (auto& it : m.items) rspans.push_back({it.rs, it.re, it.hout, it.hout, it.hout});
        float avail_h = definite_h >= 0 ? definite_h : (minh > 0 ? minh : -1.f);
        if (maxh >= 0 && avail_h >= 0) avail_h = std::min(avail_h, maxh);
        grid_size_tracks(m.rows, rspans, rgap, avail_h, avail_h >= 0 ? 0 : 2, rstretch);
        float total_rows = distribute(m.rows, rgap, avail_h >= 0 ? avail_h : 0.f, g.align_content);
        // ---- place items
        for (auto& it : m.items) {
            Box& c = *it.b;
            const Style& cs = *c.style;
            bool replaced = c.replaced != ReplacedKind::None;
            float area_w = grid_span_size(m.cols, it.cs, it.ce, cgap), area_h = grid_span_size(m.rows, it.rs, it.re, rgap);
            float ax = m.cols[static_cast<size_t>(it.cs)].pos, ay = m.rows[static_cast<size_t>(it.rs)].pos;
            bool vstretch = (it.as == 4 || (it.as == 0 && !replaced)) && cs.height.is_auto() && !it.am_t && !it.am_b;
            float target = -1;
            if (vstretch) { target = area_h - it.mt - it.mb; if (target <= c.h + 0.01f) target = -1; }
            else if (cs.height.is_pct()) {
                float hh = cs.height.resolve(area_h);
                target = cs.box_sizing == BoxSizing::BorderBox ? hh : hh + it.vp;
                if (std::fabs(target - c.h) < 0.01f) target = -1;
            }
            if (target >= 0) {
                BlockOut co;
                Opts oo; oo.forced_cw = it.w; oo.forced_ch = std::max(0.f, target - it.vp); oo.bfc_root = true;
                Ctx dummy;
                layout_block(c, area_w, area_h, oo, LCtx{&dummy, 0, 0}, co);
            }
            float outer_w = it.w + it.hp + it.ml + it.mr, outer_h = c.h + it.mt + it.mb;
            float free_x = area_w - outer_w, free_y = area_h - outer_h;
            float xoff = 0, yoff = 0;
            float ml = it.ml, mr = it.mr, mt = it.mt, mb = it.mb;
            if (it.am_l || it.am_r) {
                if (free_x > 0) { if (it.am_l && it.am_r) ml = mr = free_x / 2; else if (it.am_l) ml = free_x; else mr = free_x; }
            } else if (it.js == 2) xoff = free_x;
            else if (it.js == 3) xoff = free_x / 2;
            if (it.am_t || it.am_b) {
                if (free_y > 0) { if (it.am_t && it.am_b) mt = mb = free_y / 2; else if (it.am_t) mt = free_y; else mb = free_y; }
            } else if (it.as == 2) yoff = free_y;
            else if (it.as == 3) yoff = free_y / 2;
            c.margin.left = ml; c.margin.right = mr; c.margin.top = mt; c.margin.bottom = mb;
            c.x = ax + xoff + ml;
            c.y = ay + yoff + mt;
        }
        float rows_end = 0;
        if (!m.rows.empty()) { const GTr& last = m.rows.back(); rows_end = last.pos + last.base; }
        float used = std::max(total_rows, rows_end);
        return definite_h >= 0 ? definite_h : used;
    }

    Align item_align(const Box& c, const Style& cont) {
        Align a = c.style->align_self == Align::Auto ? cont.align_items : c.style->align_self;
        return a == Align::Auto ? Align::Stretch : a;
    }

    // Lays out the flex items of b; returns the content height. Item positions are relative to b's content origin.
    float flex_content(Box& b, float cw, float definite_h, float minh, float maxh) {
        const Style& s = *b.style;
        const bool row = s.flex_direction == FlexDirection::Row || s.flex_direction == FlexDirection::RowReverse;
        const bool rev = s.flex_direction == FlexDirection::RowReverse || s.flex_direction == FlexDirection::ColumnReverse;
        const float cgap = gap_of(s.column_gap, cw), rgap = gap_of(s.row_gap, definite_h >= 0 ? definite_h : 0);
        std::vector<FItem> items;
        for (auto& cp : b.children) {
            Box& c = *cp;
            if (c.kind == Box::Kind::Text || c.kind == Box::Kind::LineBreak || c.kind == Box::Kind::Marker) continue;
            if (is_abs(c)) { c.skipped = true; c.w = c.h = 0; abs_.push_back({&c, &b, 0, 0}); continue; }
            FItem it;
            it.b = &c;
            const Style& cs = *c.style;
            resolve_edges(c, cw);
            it.hp = hpad(c); it.vp = vpad(c);
            it.am_l = cs.margin[3].is_auto(); it.am_r = cs.margin[1].is_auto(); it.am_t = cs.margin[0].is_auto(); it.am_b = cs.margin[2].is_auto();
            it.ml = it.am_l ? 0 : c.margin.left; it.mr = it.am_r ? 0 : c.margin.right;
            it.mt = it.am_t ? 0 : c.margin.top; it.mb = it.am_b ? 0 : c.margin.bottom;
            it.grow = cs.flex_grow; it.shrink = cs.flex_shrink; it.order = cs.order;
            it.align = item_align(c, s);
            items.push_back(it);
        }
        if (items.empty()) return 0;
        std::stable_sort(items.begin(), items.end(), [](const FItem& a, const FItem& b2) { return a.order < b2.order; });
        std::vector<FItem*> all;
        for (auto& it : items) all.push_back(&it);
        Ctx dummy;

        if (row) {
            // ---- flex base sizes
            for (auto& it : items) {
                Box& c = *it.b;
                const Style& cs = *c.style;
                bool bb = cs.box_sizing == BoxSizing::BorderBox;
                auto inner = [&](const Length& l) { float v = l.resolve(cw); return bb ? std::max(0.f, v - it.hp) : v; };
                bool specified = false;
                if (cs.flex_basis.is_px() || cs.flex_basis.is_pct()) { it.basis = inner(cs.flex_basis); specified = true; }
                else if (cs.width.is_px() || cs.width.is_pct()) { it.basis = inner(cs.width); specified = true; }
                else it.basis = intrinsic(c).max;
                Intrinsic in = intrinsic(c);
                if (cs.min_width.is_px() || cs.min_width.is_pct()) it.minm = inner(cs.min_width);
                else if (cs.overflow_x == Overflow::Visible) {
                    // automatic minimum size: the content's min-content width, capped by a specified `width`
                    // (not by flex-basis)
                    bool has_w = cs.width.is_px() || cs.width.is_pct();
                    it.minm = has_w ? std::min(content_min_width(c), inner(cs.width)) : in.min;
                }
                else it.minm = 0;
                if (c.replaced != ReplacedKind::None && !specified) it.minm = std::min(it.minm, it.basis);
                it.maxm = (cs.max_width.is_px() || cs.max_width.is_pct()) ? inner(cs.max_width) : 1e30f;
                it.minm = std::min(it.minm, it.maxm);
                it.hyp = std::min(std::max(it.basis, it.minm), it.maxm);
            }
            // ---- lines
            std::vector<std::vector<FItem*>> lines;
            {
                std::vector<FItem*> cur;
                float tot = 0;
                for (auto* it : all) {
                    float o = it->hyp + it->hp + it->ml + it->mr;
                    if (s.flex_wrap != FlexWrap::NoWrap && !cur.empty() && tot + cgap + o > cw + 0.01f) { lines.push_back(cur); cur.clear(); tot = 0; }
                    tot += (cur.empty() ? 0 : cgap) + o;
                    cur.push_back(it);
                }
                if (!cur.empty()) lines.push_back(cur);
            }
            std::vector<float> line_cross(lines.size(), 0);
            // ---- flex, lay out, measure
            for (size_t li = 0; li < lines.size(); ++li) {
                auto& line = lines[li];
                float gaps = cgap * static_cast<float>(line.size() - 1);
                resolve_flex(line, cw - gaps, true);
                float asc_max = 0, desc_max = 0, cross = 0;
                for (auto* it : line) {
                    Box& c = *it->b;
                    BlockOut co;
                    Opts oo; oo.forced_cw = it->main; oo.bfc_root = true;
                    layout_block(c, cw, definite_h, oo, LCtx{&dummy, 0, 0}, co);
                    it->cross_out = c.h + it->mt + it->mb;
                    if (it->align == Align::Baseline) {
                        float fb = first_baseline(c);
                        if (fb >= 0) { it->asc = it->mt + c.border.top + c.padding.top + fb; asc_max = std::max(asc_max, it->asc); desc_max = std::max(desc_max, it->cross_out - it->asc); }
                    }
                    cross = std::max(cross, it->cross_out);
                }
                line_cross[li] = std::max(cross, asc_max + desc_max);
            }
            // ---- cross size of the container
            float total = 0;
            for (float c : line_cross) total += c;
            total += rgap * static_cast<float>(lines.size() - 1);
            float inner_h = definite_h >= 0 ? definite_h : std::max(total, minh);
            if (definite_h < 0 && maxh >= 0) inner_h = std::min(inner_h, maxh);
            // a single-line container with a definite height: its line is exactly that tall (items taller than it
            // overflow; stretched ones are cut to it, which is what lets an overflow:auto item scroll)
            if (lines.size() == 1 && definite_h >= 0 && s.flex_wrap == FlexWrap::NoWrap) line_cross[0] = inner_h;
            else if (lines.size() == 1 && (definite_h >= 0 || minh > 0)) line_cross[0] = std::max(line_cross[0], inner_h);
            float lead = 0, extra_gap = 0;
            if (lines.size() > 1 && inner_h > total) {
                float extra = inner_h - total;
                switch (s.align_content) {
                case Align::FlexEnd: lead = extra; break;
                case Align::Center: lead = extra / 2; break;
                case Align::Stretch: case Align::Auto:
                    for (auto& c : line_cross) c += extra / static_cast<float>(lines.size());
                    break;
                default: break;
                }
                (void)extra_gap;
            }
            // ---- position
            float ly = lead;
            std::vector<float> line_y(lines.size());
            for (size_t li = 0; li < lines.size(); ++li) { line_y[li] = ly; ly += line_cross[li] + rgap; }
            float cont_cross = std::max(total, inner_h);
            if (lines.size() > 1 && definite_h < 0) cont_cross = ly - rgap;
            for (size_t li = 0; li < lines.size(); ++li) {
                auto& line = lines[li];
                float lc = line_cross[li];
                // main axis
                float used = cgap * static_cast<float>(line.size() - 1);
                int auto_margins = 0;
                for (auto* it : line) { used += it->main + it->hp + it->ml + it->mr; auto_margins += (it->am_l ? 1 : 0) + (it->am_r ? 1 : 0); }
                float free_w = cw - used;
                float x = 0, between = 0;
                if (auto_margins && free_w > 0) {
                    float each = free_w / static_cast<float>(auto_margins);
                    for (auto* it : line) { if (it->am_l) it->ml = each; if (it->am_r) it->mr = each; }
                } else {
                    size_t n = line.size();
                    switch (s.justify_content) {
                    case Justify::FlexEnd: x = free_w; break;
                    case Justify::Center: x = free_w / 2; break;
                    case Justify::SpaceBetween: if (n > 1 && free_w > 0) between = free_w / static_cast<float>(n - 1); break;
                    case Justify::SpaceAround: if (free_w > 0) { between = free_w / static_cast<float>(n); x = between / 2; } break;
                    case Justify::SpaceEvenly: if (free_w > 0) { between = free_w / static_cast<float>(n + 1); x = between; } break;
                    default: break;
                    }
                }
                float asc_max = 0;
                for (auto* it : line) if (it->asc >= 0) asc_max = std::max(asc_max, it->asc);
                for (auto* it : line) {
                    Box& c = *it->b;
                    float outer_w = it->main + it->hp + it->ml + it->mr;
                    // stretch
                    if (it->align == Align::Stretch && c.style->height.is_auto() && !it->am_t && !it->am_b) {
                        float target = lc - it->mt - it->mb;
                        if (c.style->max_height.is_px()) target = std::min(target, c.style->max_height.v + (c.style->box_sizing == BoxSizing::BorderBox ? 0.f : it->vp));
                        if (c.style->min_height.is_px()) target = std::max(target, c.style->min_height.v + (c.style->box_sizing == BoxSizing::BorderBox ? 0.f : it->vp));
                        if (target > c.h + 0.01f || (target < c.h - 0.01f && target >= it->vp)) {
                            BlockOut co;
                            Opts oo; oo.forced_cw = it->main; oo.forced_ch = std::max(0.f, target - it->vp); oo.bfc_root = true;
                            layout_block(c, cw, definite_h, oo, LCtx{&dummy, 0, 0}, co);
                            it->cross_out = c.h + it->mt + it->mb;
                        }
                    }
                    float free_c = lc - it->cross_out;
                    float off = 0;
                    if (it->am_t || it->am_b) {
                        float each = it->am_t && it->am_b ? free_c / 2 : free_c;
                        if (free_c > 0) { if (it->am_t) it->mt = each; if (it->am_b) it->mb = each; }
                        off = 0;
                    } else switch (it->align) {
                    case Align::FlexEnd: off = free_c; break;
                    case Align::Center: off = free_c / 2; break;
                    case Align::Baseline: off = it->asc >= 0 ? asc_max - it->asc : 0; break;
                    default: break;
                    }
                    it->xs = x;
                    if (rev) it->xs = cw - x - outer_w;
                    c.x = it->xs + it->ml;
                    float yy = line_y[li] + off;
                    if (s.flex_wrap == FlexWrap::WrapReverse) yy = cont_cross - (line_y[li] + lc) + off;
                    c.y = yy + it->mt;
                    c.margin.left = it->ml; c.margin.right = it->mr; c.margin.top = it->mt; c.margin.bottom = it->mb;
                    x += outer_w + cgap + between;
                }
            }
            return definite_h >= 0 ? definite_h : cont_cross;
        }

        // ---- column
        for (auto& it : items) {
            Box& c = *it.b;
            const Style& cs = *c.style;
            bool bb = cs.box_sizing == BoxSizing::BorderBox;
            float avail_x = std::max(0.f, cw - it.ml - it.mr - it.hp);
            float w;
            if (cs.width.is_px() || cs.width.is_pct()) { w = cs.width.resolve(cw); if (bb) w = std::max(0.f, w - it.hp); }
            else if (it.align == Align::Stretch && !it.am_l && !it.am_r) w = avail_x;
            else { Intrinsic in = intrinsic(c); w = std::min(std::max(in.min, avail_x), in.max); }
            float maxw = cs.max_width.is_none() ? -1.f : cs.max_width.resolve(cw), minw = cs.min_width.resolve(cw, 0);
            if (bb) { if (maxw >= 0) maxw = std::max(0.f, maxw - it.hp); minw = std::max(0.f, minw - it.hp); }
            if (maxw >= 0 && w > maxw) w = maxw;
            if (w < minw) w = minw;
            it.cross_out = w + it.hp + it.ml + it.mr; // outer width
            it.hyp = w;                              // reuse: inner cross (width)
            BlockOut co;
            Opts oo; oo.forced_cw = w; oo.bfc_root = true;
            layout_block(c, cw, -1, oo, LCtx{&dummy, 0, 0}, co);
            float natural = c.h - it.vp;
            if (cs.flex_basis.is_px() || cs.flex_basis.is_pct()) {
                if (definite_h >= 0 || cs.flex_basis.is_px()) { float v = cs.flex_basis.resolve(definite_h >= 0 ? definite_h : 0); it.basis = bb ? std::max(0.f, v - it.vp) : v; }
                else it.basis = natural;
            } else if (cs.height.is_px()) it.basis = bb ? std::max(0.f, cs.height.v - it.vp) : cs.height.v;
            else it.basis = natural;
            it.asc = natural; // remember natural inner height
            float minm = cs.min_height.is_px() ? cs.min_height.v : 0.f;
            if (bb) minm = std::max(0.f, minm - it.vp);
            // min-height: auto on a flex item = its automatic minimum size: the content height (capped by a specified
            // height), so a column that doesn't fit doesn't squash its items on top of each other.
            if (cs.min_height_auto && cs.overflow_y == Overflow::Visible) {
                it.minm = natural;
                if (cs.height.is_px()) it.minm = std::min(natural, bb ? std::max(0.f, cs.height.v - it.vp) : cs.height.v);
            } else {
                it.minm = std::max(minm, 0.f);
            }
            it.maxm = cs.max_height.is_px() ? (bb ? std::max(0.f, cs.max_height.v - it.vp) : cs.max_height.v) : 1e30f;
            it.minm = std::min(it.minm, it.maxm);
            it.hyp = it.basis; // main hypothetical (cross width kept in cross_out)
            it.hyp = std::min(std::max(it.basis, it.minm), it.maxm);
        }
        float sum = rgap * static_cast<float>(items.size() - 1);
        for (auto& it : items) sum += it.hyp + it.vp + it.mt + it.mb;
        float avail = definite_h >= 0 ? definite_h : std::max(sum, minh);
        if (definite_h < 0 && maxh >= 0) avail = std::min(avail, maxh);
        resolve_flex(all, avail - rgap * static_cast<float>(items.size() - 1), false);
        float used = rgap * static_cast<float>(items.size() - 1);
        int auto_margins = 0;
        for (auto& it : items) { used += it.main + it.vp + it.mt + it.mb; auto_margins += (it.am_t ? 1 : 0) + (it.am_b ? 1 : 0); }
        float free_h = avail - used, y = 0, between = 0;
        if (auto_margins && free_h > 0) {
            float each = free_h / static_cast<float>(auto_margins);
            for (auto& it : items) { if (it.am_t) it.mt = each; if (it.am_b) it.mb = each; }
        } else {
            size_t n = items.size();
            switch (s.justify_content) {
            case Justify::FlexEnd: y = free_h; break;
            case Justify::Center: y = free_h / 2; break;
            case Justify::SpaceBetween: if (n > 1 && free_h > 0) between = free_h / static_cast<float>(n - 1); break;
            case Justify::SpaceAround: if (free_h > 0) { between = free_h / static_cast<float>(n); y = between / 2; } break;
            case Justify::SpaceEvenly: if (free_h > 0) { between = free_h / static_cast<float>(n + 1); y = between; } break;
            default: break;
            }
        }
        for (auto& it : items) {
            Box& c = *it.b;
            float natural = it.asc;
            float cross_w = it.cross_out - it.hp - it.ml - it.mr;
            if (std::fabs(it.main - natural) > 0.01f) {
                BlockOut co;
                Opts oo; oo.forced_cw = cross_w; oo.forced_ch = it.main; oo.bfc_root = true;
                layout_block(c, cw, definite_h, oo, LCtx{&dummy, 0, 0}, co);
            }
            float outer_h = it.main + it.vp + it.mt + it.mb;
            float free_x = cw - it.cross_out;
            float xo = 0;
            if (it.am_l || it.am_r) {
                if (free_x > 0) { float each = it.am_l && it.am_r ? free_x / 2 : free_x; if (it.am_l) it.ml = each; if (it.am_r) it.mr = each; }
            } else switch (it.align) {
            case Align::FlexEnd: xo = free_x; break;
            case Align::Center: xo = free_x / 2; break;
            default: break;
            }
            c.x = xo + it.ml;
            it.ys = y;
            y += outer_h + rgap + between;
            c.margin.left = it.ml; c.margin.right = it.mr; c.margin.top = it.mt; c.margin.bottom = it.mb;
        }
        if (rev) for (auto& it : items) { float outer_h = it.main + it.vp + it.mt + it.mb; it.ys = avail - it.ys - outer_h; }
        for (auto& it : items) it.b->y = it.ys + it.mt;
        return avail;
    }

    // ------------------------------------------------------------ tables
    struct TCell {
        Box*  box;
        int   r, c, rs, cs;
        float minw = 0, maxw = 0, pct = -1;
    };
    struct TGrid {
        std::vector<Box*>              captions;
        std::vector<Box*>              rows;
        std::vector<Box*>              row_group; // group box of each row, or null
        std::vector<Box*>              groups;
        std::vector<TCell>             cells;
        std::vector<std::vector<int>>  occ;       // [row][col] -> cell index or -1
        int                            ncols = 0;
        std::vector<float>             col_width, col_pct; // from <col> elements (-1 = none)
        std::vector<float>             colmin, colmax, colpct;
        float                          hs = 0, vs = 0;
        bool                           collapse = false;
    };

    static int attr_int(const Box& b, const char* n, int def, int lo, int hi) {
        const std::string* v = b.attr(n);
        if (!v) return def;
        int x = std::atoi(v->c_str());
        if (x == 0 && !(v->size() && (*v)[0] == '0')) return def;
        return std::clamp(x, lo, hi);
    }

    void build_grid(Box& t, TGrid& g) {
        const Style& ts = *t.style;
        g.collapse = ts.border_collapse;
        g.hs = g.collapse ? 0 : ts.border_spacing_h;
        g.vs = g.collapse ? 0 : ts.border_spacing_v;
        std::vector<Box*> head, body, foot;
        std::vector<std::pair<Box*, Box*>> order; // row, group
        auto add_group = [&](Box* grp, std::vector<std::pair<Box*, Box*>>& dst) {
            for (auto& rc : grp->children)
                if (rc->kind != Box::Kind::Text && rc->style && rc->style->display == Display::TableRow) dst.push_back({rc.get(), grp});
        };
        std::vector<std::pair<Box*, Box*>> hrows, brows, frows;
        for (auto& cp : t.children) {
            Box* c = cp.get();
            if (c->kind == Box::Kind::Text || !c->style) continue;
            switch (c->style->display) {
            case Display::TableCaption: g.captions.push_back(c); break;
            case Display::TableColumnGroup: {
                bool has_cols = false;
                for (auto& cc : c->children)
                    if (cc->style && cc->style->display == Display::TableColumn) {
                        has_cols = true;
                        int span = attr_int(*cc, "span", 1, 1, 1000);
                        for (int k = 0; k < span; ++k) { g.col_width.push_back(cc->style->width.is_px() ? cc->style->width.v : -1); g.col_pct.push_back(cc->style->width.is_pct() ? cc->style->width.v : -1); }
                    }
                if (!has_cols) {
                    int span = attr_int(*c, "span", 1, 1, 1000);
                    for (int k = 0; k < span; ++k) { g.col_width.push_back(c->style->width.is_px() ? c->style->width.v : -1); g.col_pct.push_back(c->style->width.is_pct() ? c->style->width.v : -1); }
                }
                c->skipped = true;
                break;
            }
            case Display::TableColumn: {
                int span = attr_int(*c, "span", 1, 1, 1000);
                for (int k = 0; k < span; ++k) { g.col_width.push_back(c->style->width.is_px() ? c->style->width.v : -1); g.col_pct.push_back(c->style->width.is_pct() ? c->style->width.v : -1); }
                c->skipped = true;
                break;
            }
            case Display::TableHeaderGroup: g.groups.push_back(c); add_group(c, hrows); break;
            case Display::TableRowGroup: g.groups.push_back(c); add_group(c, brows); break;
            case Display::TableFooterGroup: g.groups.push_back(c); add_group(c, frows); break;
            case Display::TableRow: brows.push_back({c, nullptr}); break;
            default: break;
            }
        }
        for (auto* v : {&hrows, &brows, &frows}) order.insert(order.end(), v->begin(), v->end());
        for (auto& rg : order) { g.rows.push_back(rg.first); g.row_group.push_back(rg.second); }
        int nrows = static_cast<int>(g.rows.size());
        g.occ.assign(static_cast<size_t>(nrows), {});
        auto ensure = [&](int r, int c) {
            auto& row = g.occ[static_cast<size_t>(r)];
            if (static_cast<int>(row.size()) <= c) row.resize(static_cast<size_t>(c) + 1, -1);
        };
        for (int r = 0; r < nrows; ++r) {
            int col = 0;
            for (auto& cp : g.rows[static_cast<size_t>(r)]->children) {
                Box* c = cp.get();
                if (c->kind == Box::Kind::Text || !c->style || c->style->display != Display::TableCell) continue;
                ensure(r, col);
                while (g.occ[static_cast<size_t>(r)].size() > static_cast<size_t>(col) && g.occ[static_cast<size_t>(r)][static_cast<size_t>(col)] >= 0) { ++col; ensure(r, col); }
                TCell tc;
                tc.box = c; tc.r = r; tc.c = col;
                tc.cs = attr_int(*c, "colspan", 1, 1, 200);
                tc.rs = attr_int(*c, "rowspan", 1, 1, 200);
                tc.rs = std::min(tc.rs, nrows - r);
                int idx = static_cast<int>(g.cells.size());
                for (int rr = r; rr < r + tc.rs; ++rr)
                    for (int cc = col; cc < col + tc.cs; ++cc) { ensure(rr, cc); g.occ[static_cast<size_t>(rr)][static_cast<size_t>(cc)] = idx; }
                g.cells.push_back(tc);
                col += tc.cs;
                g.ncols = std::max(g.ncols, col);
            }
        }
        g.ncols = std::max(g.ncols, static_cast<int>(g.col_width.size()));
        for (auto& row : g.occ) row.resize(static_cast<size_t>(g.ncols), -1);
        g.col_width.resize(static_cast<size_t>(g.ncols), -1);
        g.col_pct.resize(static_cast<size_t>(g.ncols), -1);

        // rows, groups and columns have no box edges of their own
        for (Box* r : g.rows) { r->margin = r->padding = r->border = Edges{}; r->border_fixed = true; }
        for (Box* r : g.groups) { r->margin = r->padding = r->border = Edges{}; r->border_fixed = true; }

        // cell edges (with collapsed borders when requested)
        const float tbl_cb = 0;
        for (auto& tc : g.cells) {
            Box& c = *tc.box;
            c.border_fixed = false;
            resolve_edges(c, tbl_cb);
            c.margin = Edges{};
        }
        if (g.collapse) {
            auto cell_at = [&](int r, int c) -> TCell* {
                if (r < 0 || c < 0 || r >= nrows || c >= g.ncols) return nullptr;
                int i = g.occ[static_cast<size_t>(r)][static_cast<size_t>(c)];
                return i < 0 ? nullptr : &g.cells[static_cast<size_t>(i)];
            };
            for (auto& tc : g.cells) {
                Box& c = *tc.box;
                TCell* l = cell_at(tc.r, tc.c - 1);
                if (l && l != &tc) { Box& n = *l->box; if (n.border.right >= c.border.left) c.border.left = 0; else n.border.right = 0; }
                else if (tc.c == 0 && t.border.left > 0 && c.border.left <= t.border.left) c.border.left = 0;
                TCell* u = cell_at(tc.r - 1, tc.c);
                if (u && u != &tc) { Box& n = *u->box; if (n.border.bottom >= c.border.top) c.border.top = 0; else n.border.bottom = 0; }
                else if (tc.r == 0 && t.border.top > 0 && c.border.top <= t.border.top) c.border.top = 0;
                if (tc.c + tc.cs >= g.ncols && t.border.right > 0 && c.border.right <= t.border.right) c.border.right = 0;
                if (tc.r + tc.rs >= nrows && t.border.bottom > 0 && c.border.bottom <= t.border.bottom) c.border.bottom = 0;
            }
        }
        for (auto& tc : g.cells) tc.box->border_fixed = true;

        // column constraints
        g.colmin.assign(static_cast<size_t>(g.ncols), 0);
        g.colmax.assign(static_cast<size_t>(g.ncols), 0);
        g.colpct.assign(static_cast<size_t>(g.ncols), 0);
        for (int c = 0; c < g.ncols; ++c) {
            if (g.col_width[static_cast<size_t>(c)] >= 0) g.colmax[static_cast<size_t>(c)] = g.col_width[static_cast<size_t>(c)];
            if (g.col_pct[static_cast<size_t>(c)] >= 0) g.colpct[static_cast<size_t>(c)] = g.col_pct[static_cast<size_t>(c)];
        }
        for (auto& tc : g.cells) {
            Box&         c = *tc.box;
            const Style& cs = *c.style;
            Intrinsic    in = intrinsic_contents(c);
            float        hp = hpad(c);
            tc.minw = in.min + hp;
            tc.maxw = in.max + hp;
            if (cs.min_width.is_px()) tc.minw = std::max(tc.minw, cs.min_width.v + (cs.box_sizing == BoxSizing::BorderBox ? 0 : hp));
            if (cs.width.is_px()) {
                float pref = cs.width.v + (cs.box_sizing == BoxSizing::BorderBox ? 0 : hp);
                tc.maxw = std::max(tc.minw, pref);
            } else if (cs.width.is_pct()) {
                tc.pct = cs.width.v;
            }
            tc.maxw = std::max(tc.maxw, tc.minw);
        }
        for (auto& tc : g.cells) {
            if (tc.cs != 1) continue;
            size_t c = static_cast<size_t>(tc.c);
            g.colmin[c] = std::max(g.colmin[c], tc.minw);
            g.colmax[c] = std::max(g.colmax[c], tc.maxw);
            if (tc.pct >= 0) g.colpct[c] = std::max(g.colpct[c], tc.pct);
        }
        for (auto& tc : g.cells) {
            if (tc.cs == 1) continue;
            float sp = static_cast<float>(tc.cs - 1) * g.hs;
            float cm = 0, cx = 0;
            for (int c = tc.c; c < tc.c + tc.cs; ++c) { cm += g.colmin[static_cast<size_t>(c)]; cx += g.colmax[static_cast<size_t>(c)]; }
            if (tc.minw - sp > cm) { float add = (tc.minw - sp - cm) / static_cast<float>(tc.cs); for (int c = tc.c; c < tc.c + tc.cs; ++c) g.colmin[static_cast<size_t>(c)] += add; }
            if (tc.maxw - sp > cx) { float add = (tc.maxw - sp - cx) / static_cast<float>(tc.cs); for (int c = tc.c; c < tc.c + tc.cs; ++c) g.colmax[static_cast<size_t>(c)] += add; }
            if (tc.pct >= 0) for (int c = tc.c; c < tc.c + tc.cs; ++c) g.colpct[static_cast<size_t>(c)] = std::max(g.colpct[static_cast<size_t>(c)], tc.pct / static_cast<float>(tc.cs));
        }
        for (int c = 0; c < g.ncols; ++c) g.colmax[static_cast<size_t>(c)] = std::max(g.colmax[static_cast<size_t>(c)], g.colmin[static_cast<size_t>(c)]);
    }

    Intrinsic table_intrinsic(Box& b) {
        resolve_edges(b, 0);
        TGrid g;
        build_grid(b, g);
        Intrinsic r;
        float     sp = static_cast<float>(g.ncols + 1) * g.hs;
        r.min = r.max = sp;
        for (int c = 0; c < g.ncols; ++c) { r.min += g.colmin[static_cast<size_t>(c)]; r.max += g.colmax[static_cast<size_t>(c)]; }
        // percentage columns need room for the rest: max-content grows so that pct columns fit their share
        float pct = 0;
        for (float p : g.colpct) pct += p;
        if (pct > 0 && pct < 100) {
            float fixed_max = 0;
            for (int c = 0; c < g.ncols; ++c) if (g.colpct[static_cast<size_t>(c)] <= 0) fixed_max += g.colmax[static_cast<size_t>(c)];
            r.max = std::max(r.max, fixed_max * 100.f / (100.f - pct) + sp);
        }
        for (Box* cap : g.captions) { Intrinsic ci = intrinsic(*cap); r.min = std::max(r.min, ci.min); r.max = std::max(r.max, ci.max); }
        return r;
    }

    std::vector<float> distribute_cols(const TGrid& g, float avail) {
        size_t n = static_cast<size_t>(g.ncols);
        std::vector<float> w(n, 0);
        std::vector<bool>  fixed(n, false);
        float rem = avail;
        float ptotal = 0;
        for (float p : g.colpct) ptotal += p;
        float pscale = ptotal > 100 ? 100.f / ptotal : 1.f;
        for (size_t c = 0; c < n; ++c)
            if (g.colpct[c] > 0) { w[c] = std::max(g.colmin[c], avail * g.colpct[c] * pscale / 100.f); fixed[c] = true; rem -= w[c]; }
        float smin = 0, smax = 0;
        for (size_t c = 0; c < n; ++c) if (!fixed[c]) { smin += g.colmin[c]; smax += g.colmax[c]; }
        for (size_t c = 0; c < n; ++c) {
            if (fixed[c]) continue;
            if (rem <= smin) w[c] = g.colmin[c];
            else if (rem >= smax) {
                float extra = rem - smax;
                w[c] = g.colmax[c] + (smax > 0 ? extra * g.colmax[c] / smax : extra / static_cast<float>(n));
            } else {
                w[c] = g.colmin[c] + (g.colmax[c] - g.colmin[c]) * (rem - smin) / (smax - smin);
            }
        }
        return w;
    }

    std::vector<float> fixed_cols(const TGrid& g, float avail) {
        size_t n = static_cast<size_t>(g.ncols);
        std::vector<float> w(n, -1);
        for (size_t c = 0; c < n; ++c) {
            if (g.col_width[c] >= 0) w[c] = g.col_width[c];
            else if (g.col_pct[c] >= 0) w[c] = avail * g.col_pct[c] / 100.f;
        }
        if (!g.rows.empty()) {
            for (auto& tc : g.cells) {
                if (tc.r != 0) continue;
                const Style& cs = *tc.box->style;
                float v = -1;
                if (cs.width.is_px()) v = cs.width.v + (cs.box_sizing == BoxSizing::BorderBox ? 0 : hpad(*tc.box));
                else if (cs.width.is_pct()) v = avail * cs.width.v / 100.f;
                if (v < 0) continue;
                float sp = static_cast<float>(tc.cs - 1) * g.hs;
                for (int c = tc.c; c < tc.c + tc.cs; ++c) if (w[static_cast<size_t>(c)] < 0) w[static_cast<size_t>(c)] = std::max(0.f, (v - sp) / static_cast<float>(tc.cs));
            }
        }
        float used = 0;
        int   unset = 0;
        for (float v : w) { if (v >= 0) used += v; else ++unset; }
        if (unset) {
            float each = std::max(0.f, (avail - used) / static_cast<float>(unset));
            for (float& v : w) if (v < 0) v = each;
        }
        return w;
    }

    void layout_table(Box& b, float cb_w, float cb_h, const Opts& o, BlockOut& out) {
        (void)cb_h;
        const Style& s = *b.style;
        TGrid g;
        build_grid(b, g);
        const float h_extra = hpad(b);
        const int   nrows = static_cast<int>(g.rows.size());
        const float sp = static_cast<float>(g.ncols + 1) * g.hs;
        float sum_min = sp, sum_max = sp;
        for (int c = 0; c < g.ncols; ++c) { sum_min += g.colmin[static_cast<size_t>(c)]; sum_max += g.colmax[static_cast<size_t>(c)]; }
        float cap_min = 0;
        for (Box* cap : g.captions) cap_min = std::max(cap_min, intrinsic(*cap).min);

        float W = -1;
        bool  explicit_w = false;
        if (o.forced_cw >= 0) { W = o.forced_cw; explicit_w = true; }
        else if (s.width.is_px() || s.width.is_pct()) {
            W = s.width.resolve(cb_w);
            if (s.box_sizing == BoxSizing::BorderBox) W = std::max(0.f, W - h_extra);
            explicit_w = true;
        }
        if (W < 0) {
            Intrinsic ti = table_intrinsic(b);
            float avail = std::max(0.f, cb_w - b.margin.left - b.margin.right - h_extra);
            W = std::min(std::max(std::max(ti.min, sum_min), avail), std::max(ti.max, sum_max));
        }
        float maxw = s.max_width.is_none() ? -1.f : s.max_width.resolve(cb_w);
        if (s.box_sizing == BoxSizing::BorderBox && maxw >= 0) maxw = std::max(0.f, maxw - h_extra);
        if (maxw >= 0 && W > maxw) W = maxw;
        W = std::max(W, std::max(sum_min, cap_min));
        {
            float minw = s.min_width.resolve(cb_w, 0);
            if (s.box_sizing == BoxSizing::BorderBox) minw = std::max(0.f, minw - h_extra);
            W = std::max(W, minw);
        }

        std::vector<float> cw;
        float avail_cols = W - sp;
        if (s.table_fixed && explicit_w) cw = fixed_cols(g, avail_cols);
        else cw = distribute_cols(g, avail_cols);
        float used_cols = 0;
        for (float v : cw) used_cols += v;
        if (explicit_w || true) W = std::max(W, used_cols + sp);

        // horizontal placement
        if (!o.shrink && o.forced_cw < 0) {
            bool am_l = s.margin[3].is_auto(), am_r = s.margin[1].is_auto();
            float slack = cb_w - (W + h_extra + b.margin.left + b.margin.right);
            if (slack > 0) {
                if (am_l && am_r) b.margin.left = b.margin.right = slack / 2;
                else if (am_l) b.margin.left = slack;
                else if (am_r) b.margin.right = slack;
                else legacy_place(s, slack, b.margin.left, b.margin.right);
            }
        }
        b.x = b.margin.left + o.xshift;

        std::vector<float> colx(static_cast<size_t>(g.ncols), 0);
        {
            float x = g.hs;
            for (int c = 0; c < g.ncols; ++c) { colx[static_cast<size_t>(c)] = x; x += cw[static_cast<size_t>(c)] + g.hs; }
        }

        // captions above
        float cy = 0;
        Ctx   dummy;
        std::vector<Box*> bottom_caps;
        for (Box* cap : g.captions) {
            if (cap->style->caption_bottom) { bottom_caps.push_back(cap); continue; }
            BlockOut co;
            layout_block(*cap, W, -1, Opts{}, LCtx{&dummy, 0, 0}, co);
            cap->y = cy + cap->margin.top;
            cy = cap->y + cap->h + cap->margin.bottom;
        }
        const float gy = cy;

        // cells
        std::vector<float> rh(static_cast<size_t>(nrows), 0), rowasc(static_cast<size_t>(nrows), 0), rowdesc(static_cast<size_t>(nrows), 0);
        for (int r = 0; r < nrows; ++r) {
            const Style& rs = *g.rows[static_cast<size_t>(r)]->style;
            if (rs.height.is_px()) rh[static_cast<size_t>(r)] = rs.height.v;
        }
        struct CellInfo { float asc = -1; };
        std::vector<CellInfo> info(g.cells.size());
        for (size_t ci = 0; ci < g.cells.size(); ++ci) {
            TCell& tc = g.cells[ci];
            Box&   c = *tc.box;
            float width = static_cast<float>(tc.cs - 1) * g.hs;
            for (int k = tc.c; k < tc.c + tc.cs; ++k) width += cw[static_cast<size_t>(k)];
            BlockOut co;
            Opts     oo;
            oo.forced_cw = std::max(0.f, width - hpad(c));
            layout_block(c, width, -1, oo, LCtx{&dummy, 0, 0}, co);
            c.x = colx[static_cast<size_t>(tc.c)];
            c.w = width;
            VAlign va = c.style->vertical_align;
            bool   baseline = !(va == VAlign::Top || va == VAlign::Middle || va == VAlign::Bottom);
            if (baseline && tc.rs == 1) {
                float fb = first_baseline(c);
                if (fb >= 0) info[ci].asc = c.border.top + c.padding.top + fb;
            }
            if (tc.rs == 1) {
                size_t r = static_cast<size_t>(tc.r);
                if (info[ci].asc >= 0) { rowasc[r] = std::max(rowasc[r], info[ci].asc); rowdesc[r] = std::max(rowdesc[r], c.h - info[ci].asc); }
                rh[r] = std::max(rh[r], c.h);
            }
        }
        for (int r = 0; r < nrows; ++r) rh[static_cast<size_t>(r)] = std::max(rh[static_cast<size_t>(r)], rowasc[static_cast<size_t>(r)] + rowdesc[static_cast<size_t>(r)]);
        for (auto& tc : g.cells) {
            if (tc.rs == 1) continue;
            float span = static_cast<float>(tc.rs - 1) * g.vs;
            for (int r = tc.r; r < tc.r + tc.rs; ++r) span += rh[static_cast<size_t>(r)];
            if (tc.box->h > span) { float add = (tc.box->h - span) / static_cast<float>(tc.rs); for (int r = tc.r; r < tc.r + tc.rs; ++r) rh[static_cast<size_t>(r)] += add; }
        }
        std::vector<float> ry(static_cast<size_t>(nrows), 0);
        float y = g.vs;
        for (int r = 0; r < nrows; ++r) { ry[static_cast<size_t>(r)] = y; y += rh[static_cast<size_t>(r)] + g.vs; }
        float Hgrid = nrows ? y : 0;
        // explicit table height: grow the rows
        {
            float want = -1;
            if (s.height.is_px()) want = s.height.v - (s.box_sizing == BoxSizing::BorderBox ? vpad(b) : 0);
            float cap_h = cy;
            for (Box* cap : bottom_caps) { (void)cap; }
            if (want >= 0 && nrows && want - cap_h > Hgrid) {
                float add = (want - cap_h - Hgrid) / static_cast<float>(nrows);
                for (auto& v : rh) v += add;
                y = g.vs;
                for (int r = 0; r < nrows; ++r) { ry[static_cast<size_t>(r)] = y; y += rh[static_cast<size_t>(r)] + g.vs; }
                Hgrid = y;
            }
        }

        // place cells inside rows, rows inside groups / the table
        std::vector<float> row_top(static_cast<size_t>(nrows), 0);
        for (int r = 0; r < nrows; ++r) {
            Box* row = g.rows[static_cast<size_t>(r)];
            Box* grp = g.row_group[static_cast<size_t>(r)];
            row->skipped = false;
            row->x = 0;
            row->w = W;
            row->h = rh[static_cast<size_t>(r)];
            row_top[static_cast<size_t>(r)] = gy + ry[static_cast<size_t>(r)];
            row->y = grp ? 0 : row_top[static_cast<size_t>(r)]; // fixed up below for grouped rows
        }
        for (Box* grp : g.groups) {
            int first = -1, last = -1;
            for (int r = 0; r < nrows; ++r) if (g.row_group[static_cast<size_t>(r)] == grp) { if (first < 0) first = r; last = r; }
            grp->skipped = false;
            grp->x = 0;
            grp->w = W;
            if (first < 0) { grp->y = gy; grp->h = 0; continue; }
            grp->y = row_top[static_cast<size_t>(first)];
            grp->h = row_top[static_cast<size_t>(last)] + rh[static_cast<size_t>(last)] - grp->y;
            for (int r = first; r <= last; ++r) if (g.row_group[static_cast<size_t>(r)] == grp) g.rows[static_cast<size_t>(r)]->y = row_top[static_cast<size_t>(r)] - grp->y;
        }
        for (size_t ci = 0; ci < g.cells.size(); ++ci) {
            TCell& tc = g.cells[ci];
            Box&   c = *tc.box;
            float  H = static_cast<float>(tc.rs - 1) * g.vs;
            for (int r = tc.r; r < tc.r + tc.rs; ++r) H += rh[static_cast<size_t>(r)];
            float natural = c.h;
            c.h = H;
            c.y = 0;
            float dy = 0;
            VAlign va = c.style->vertical_align;
            if (info[ci].asc >= 0) dy = rowasc[static_cast<size_t>(tc.r)] - info[ci].asc;
            else if (va == VAlign::Middle) dy = (H - natural) / 2;
            else if (va == VAlign::Bottom) dy = H - natural;
            c.content_dy = std::max(0.f, dy);
        }
        // captions below
        float yend = gy + Hgrid;
        for (Box* cap : bottom_caps) {
            BlockOut co;
            layout_block(*cap, W, -1, Opts{}, LCtx{&dummy, 0, 0}, co);
            cap->y = yend + cap->margin.top;
            yend = cap->y + cap->h + cap->margin.bottom;
        }
        float content_h = yend;
        b.w = W + h_extra;
        b.h = content_h + vpad(b);
        if (nrows) b.last_baseline = b.border.top + b.padding.top + gy + ry[0] + (rowasc[0] > 0 ? rowasc[0] : 0);
        else b.last_baseline = -1;
        out.mt = b.margin.top;
        out.mb = b.margin.bottom;
        out.empty_through = false;
    }

    // ------------------------------------------------------------ absolute coordinates
    void finalize(Box& b, float ox, float oy) {
        if (b.skipped) { b.x = ox; b.y = oy; return; }
        float dx = 0, dy = 0;
        if (b.style && b.style->position == Position::Relative) {
            const Style& s = *b.style;
            if (s.inset[3].is_px()) dx = s.inset[3].v; else if (s.inset[1].is_px()) dx = -s.inset[1].v;
            if (s.inset[0].is_px()) dy = s.inset[0].v; else if (s.inset[2].is_px()) dy = -s.inset[2].v;
        }
        if (b.style && (b.style->tr_x_px != 0 || b.style->tr_x_pct != 0 || b.style->tr_y_px != 0 || b.style->tr_y_pct != 0)) {
            dx += b.style->tr_x_px + b.style->tr_x_pct * 0.01f * b.w;
            dy += b.style->tr_y_px + b.style->tr_y_pct * 0.01f * b.h;
        }
        b.x += ox + dx;
        b.y += oy + dy;
        float cx = b.x + b.border.left + b.padding.left;
        float cy = b.y + b.border.top + b.padding.top + b.content_dy;
        if (b.has_marker) { b.marker_x += cx; b.marker_y += cy; }
        for (auto& L : b.lines) {
            L.x += cx; L.y += cy;
            for (auto& f : L.frags) {
                f.x += cx;
                if (f.kind == Frag::Kind::Text) f.y += cy;
            }
            for (auto& sp : L.spans) { sp.x += cx; sp.baseline += L.y; }
        }
        for (auto& L : b.lines)
            for (auto& f : L.frags)
                if (f.kind == Frag::Kind::Atomic) {
                    Box& x = *f.box;
                    // x.x/y were relative to this container's content origin
                    finalize_atomic(x, cx, cy);
                    f.x = x.x; f.y = x.y;
                }
        for (Box* f : b.inline_floats) finalize(*f, cx, cy);
        if (!b.lines.empty() || !b.inline_floats.empty()) return; // inline container: children are inline content
        for (auto& c : b.children) {
            if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak || c->kind == Box::Kind::Marker) continue;
            if (c->skipped) continue;
            finalize(*c, cx, cy);
        }
    }
    void finalize_atomic(Box& x, float cx, float cy) {
        // already positioned relative to (cx, cy): reuse finalize without double-adding the margin
        finalize(x, cx, cy);
    }
};

} // namespace

TextMeasurer& fake_text_measurer() {
    static FakeMeasurer m;
    return m;
}

void layout_document(Box& root, const LayoutEnv& env) {
    Layouter l(env);
    l.run(root);
}

void update_ink(Box& root) {
    Layouter::clear_ink(root);
    Layouter::compute_ink(root);
}

} // namespace quill::engine
