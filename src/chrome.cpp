#include "chrome.h"

#include "font.h"
#include "svg.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace quill::ui {

namespace {

// ---------------------------------------------------------------- palette (light, in the spirit of Firefox Proton)
const Color kStrip(235, 235, 240);       // tab strip
const Color kToolbar(249, 249, 251);     // toolbar
const Color kActiveTab(255, 255, 255);
const Color kHoverTab(220, 220, 227);
const Color kBtnHover(224, 224, 230);
const Color kField(240, 240, 244);
const Color kFieldHover(232, 232, 237);
const Color kFocusRing(0, 97, 224);
const Color kText(21, 20, 26);
const Color kTextDim(91, 91, 102);
const Color kTextFaint(150, 150, 160);
const Color kDivider(207, 207, 216);
const Color kGreen(42, 150, 80);
const Color kRed(197, 40, 48);
const Color kAmber(176, 112, 0);
const Color kSelection(180, 210, 255);

Font* font(float px, int weight = 400) { return FontManager::instance().get("sans-serif", px, weight, false); }

// ---------------------------------------------------------------- icons
// Material Icons by Google (https://github.com/google/material-design-icons), Apache License 2.0. 24x24 paths.
const std::map<std::string, std::string>& builtin_icons() {
    static const std::map<std::string, std::string> m = {
        {"back", "M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"},
        {"forward", "M12 4l-1.41 1.41L16.17 11H4v2h12.17l-5.58 5.59L12 20l8-8z"},
        {"reload", "M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 "
                   "2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"},
        {"close", "M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z"},
        {"add", "M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z"},
        {"home", "M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"},
        {"shield-check", "M12 1L3 5v6c0 5.55 3.84 10.74 9 12 5.16-1.26 9-6.45 9-12V5l-9-4zm-2 16l-4-4 1.41-1.41L10 14.17l6.59-6.59L18 "
                         "9l-8 8z"},
        {"lock", "M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 "
                 "9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z"},
        {"lock-open", "M12 17c1.1 0 2-.9 2-2s-.9-2-2-2-2 .9-2 2 .9 2 2 2zm6-9h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6h1.9c0-1.71 1.39-3.1 "
                      "3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm0 "
                      "12H6V10h12v10z"},
        {"key", "M12.65 10C11.83 7.67 9.61 6 7 6c-3.31 0-6 2.69-6 6s2.69 6 6 6c2.61 0 4.83-1.67 5.65-4H17v4h4v-4h2v-4H12.65zM7 14c-1.1 "
                "0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2z"},
        {"info", "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"},
        {"warning", "M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z"},
        {"search", "M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 "
                   "3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 "
                   "9.5 14z"},
    };
    return m;
}

struct IconCache {
    std::mutex                                     mu;
    std::string                                    dir;
    std::map<std::string, std::shared_ptr<SvgDoc>> docs;  // by name (nullptr = not available)
    std::map<std::string, std::vector<uint8_t>>    masks; // by name + size
};
IconCache& cache() {
    static IconCache c;
    return c;
}

std::shared_ptr<SvgDoc> icon_doc(IconCache& c, const std::string& name) {
    auto it = c.docs.find(name);
    if (it != c.docs.end()) return it->second;
    std::shared_ptr<SvgDoc> doc;
    if (!c.dir.empty() && name.find('/') == std::string::npos && name.find('.') == std::string::npos) {
        std::ifstream f(c.dir + "/" + name + ".svg", std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string s = ss.str();
            if (s.size() < (1u << 20)) doc = SvgDoc::parse(reinterpret_cast<const unsigned char*>(s.data()), s.size());
        }
    }
    if (!doc) {
        auto b = builtin_icons().find(name);
        if (b != builtin_icons().end()) {
            std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\"><path d=\"" +
                              b->second + "\" fill=\"#000\"/></svg>";
            doc = SvgDoc::parse(reinterpret_cast<const unsigned char*>(svg.data()), svg.size());
        }
    }
    c.docs[name] = doc;
    return doc;
}

int blend_ch(int dst, int src, int a) { return (src * a + dst * (255 - a) + 127) / 255; }

// ---------------------------------------------------------------- small drawing helpers
void soft_shadow(Framebuffer& fb, const IRect& r, int radius, int spread) {
    for (int i = spread; i >= 1; --i) {
        uint8_t a = static_cast<uint8_t>(std::max(2, 18 / i));
        fb.fill_round_rect(IRect(r.x - i, r.y - i + 1, r.w + 2 * i, r.h + 2 * i), radius + i, radius + i, radius + i, radius + i,
                           Color(0, 0, 0, a));
    }
}

void icon_button(Framebuffer& fb, const IRect& r, const char* icon, bool enabled, bool hover) {
    if (hover && enabled) fb.fill_round_rect(r, 6, 6, 6, 6, kBtnHover);
    int s = 20;
    draw_icon(fb, icon, r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, enabled ? Color(60, 60, 70) : Color(190, 190, 198));
}

void text_at(Framebuffer& fb, Font* f, const std::string& s, int x, const IRect& box, Color c) {
    f->draw(fb, s, x, box.y + (box.h + f->ascent - f->descent) / 2, c);
}

// a stable pleasant colour per host, for the tab's letter badge
Color badge_color(const std::string& host) {
    static const Color kPal[] = {Color(0, 116, 217), Color(46, 139, 87), Color(196, 80, 20), Color(128, 64, 191),
                                 Color(0, 137, 138), Color(186, 40, 90), Color(90, 110, 30), Color(70, 90, 160)};
    uint32_t h = 2166136261u;
    for (char ch : host) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return kPal[h % (sizeof(kPal) / sizeof(kPal[0]))];
}

// first letter of the registrable-ish part of a host ("www.wikipedia.org" -> 'W')
std::string badge_letter(const std::string& host) {
    // the site's own name: "en.wikipedia.org" -> wikipedia, "news.bbc.co.uk" -> bbc (IP addresses keep their digits)
    std::vector<std::string> labels;
    size_t                   pos = 0;
    while (pos <= host.size()) {
        size_t d = host.find('.', pos);
        if (d == std::string::npos) d = host.size();
        labels.push_back(host.substr(pos, d - pos));
        pos = d + 1;
    }
    bool ip = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ':'; });
    std::string h = host;
    if (!ip && labels.size() >= 2) {
        size_t i = labels.size() - 2;
        static const char* kSecond[] = {"co", "com", "org", "net", "ac", "gov", "edu", "or", "ne"};
        if (labels.size() >= 3 && labels.back().size() == 2)
            for (const char* sl : kSecond)
                if (labels[i] == sl) { i--; break; }
        h = labels[i];
    }
    for (char ch : h)
        if (std::isalnum(static_cast<unsigned char>(ch))) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    return "?";
}

void wrap_text(Font* f, const std::string& text, int maxw, std::vector<std::string>& lines) {
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && f->text_width(cand) > maxw) {
            lines.push_back(line);
            line = word;
        } else line = cand;
        word.clear();
    };
    for (char ch : text) {
        if (ch == ' ') flush_word();
        else word += ch;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
}

} // namespace

// ---------------------------------------------------------------- icons
void set_icon_dir(const std::string& dir) {
    IconCache&                  c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    c.dir = dir;
    c.docs.clear();
    c.masks.clear();
}

bool have_icon(const std::string& name) {
    IconCache&                  c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    return icon_doc(c, name) != nullptr;
}

void draw_icon(Framebuffer& fb, const std::string& name, int x, int y, int size, Color col) {
    if (size <= 0 || size > 512) return;
    IconCache&                  c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    std::string                 key = name + "@" + std::to_string(size);
    auto                        it  = c.masks.find(key);
    if (it == c.masks.end()) {
        std::vector<uint8_t> mask;
        if (auto doc = icon_doc(c, name)) {
            std::vector<uint32_t> px;
            if (doc->render(size, size, px)) {
                mask.resize(px.size());
                for (size_t i = 0; i < px.size(); i++) mask[i] = static_cast<uint8_t>(px[i] >> 24);
            }
        }
        it = c.masks.emplace(key, std::move(mask)).first;
    }
    const std::vector<uint8_t>& m = it->second;
    if (m.size() != static_cast<size_t>(size) * size) return;
    IRect clip = fb.clip().intersect(IRect(0, 0, fb.w, fb.h));
    for (int yy = 0; yy < size; yy++) {
        int py = y + yy;
        if (py < clip.y || py >= clip.bottom()) continue;
        for (int xx = 0; xx < size; xx++) {
            int px = x + xx;
            if (px < clip.x || px >= clip.right()) continue;
            int a = m[static_cast<size_t>(yy) * size + xx] * col.a / 255;
            if (!a) continue;
            uint32_t& d = fb.px[static_cast<size_t>(py) * fb.w + px];
            int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            d = 0xFF000000u | (static_cast<uint32_t>(blend_ch(dr, col.r, a)) << 16) | (static_cast<uint32_t>(blend_ch(dg, col.g, a)) << 8) |
                static_cast<uint32_t>(blend_ch(db, col.b, a));
        }
    }
}

std::string ellipsize(const std::string& s, float px, int maxw, int weight) {
    Font* f = font(px, weight);
    if (!f || f->text_width(s) <= maxw) return s;
    std::string out;
    size_t      i = 0;
    while (i < s.size()) {
        size_t n = i + 1;
        while (n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n++;
        std::string cand = out + s.substr(i, n - i);
        if (f->text_width(cand + "\xE2\x80\xA6") > maxw) break;
        out = cand;
        i   = n;
    }
    return out + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------- tabs
namespace {
constexpr int kTabMaxW = 240, kTabMinW = 36, kNewTabW = 30, kTabGap = 4, kStripPad = 6;
int tab_width(int n, int W) {
    int avail = W - kNewTabW - 2 * kStripPad - 8;
    int w     = n > 0 ? avail / n : kTabMaxW;
    return std::max(kTabMinW, std::min(kTabMaxW, w));
}
bool shows_close(int tw, bool active) { return active || tw >= 90; }
IRect close_rect(const IRect& r) { return IRect(r.right() - 26, r.y + (r.h - 20) / 2, 20, 20); }
IRect plus_rect(int n, int W) {
    int tw = tab_width(n, W);
    int x  = std::min(kStripPad + n * tw + 4, W - kNewTabW - kStripPad);
    return IRect(x, 6 + (kTabH - 6 - 26) / 2, kNewTabW, 26);
}
} // namespace

IRect tab_rect(int i, int n, int W) {
    int tw = tab_width(n, W);
    return IRect(kStripPad + i * tw, 5, tw - kTabGap, kTabH - 9);
}

TabHit tab_hit(int x, int y, int n, int W, int active) {
    TabHit h;
    if (y < 0 || y >= kTabH) return h;
    for (int i = 0; i < n; i++) {
        IRect r = tab_rect(i, n, W);
        if (x >= r.x && x < r.x + r.w + kTabGap) {
            h.index = i;
            h.close = shows_close(tab_width(n, W), i == active) && close_rect(r).contains(x, std::max(r.y, std::min(y, r.bottom() - 1)));
            return h;
        }
    }
    if (n < kMaxTabs && plus_rect(n, W).contains(x, std::max(plus_rect(n, W).y, std::min(y, plus_rect(n, W).bottom() - 1)))) h.plus = true;
    return h;
}

void draw_tabs(Framebuffer& fb, const std::vector<TabInfo>& tabs, int active, int hover, bool hover_close, bool hover_plus) {
    int W = fb.w;
    fb.fill_rect(IRect(0, 0, W, kTabH), kStrip);
    int   n  = static_cast<int>(tabs.size());
    int   tw = tab_width(n, W);
    Font* f  = font(13);
    for (int i = 0; i < n; i++) {
        IRect r   = tab_rect(i, n, W);
        bool  act = i == active;
        if (act) {
            soft_shadow(fb, r, 8, 3);
            fb.fill_round_rect(r, 8, 8, 8, 8, kActiveTab);
        } else if (i == hover) {
            fb.fill_round_rect(r, 8, 8, 8, 8, kHoverTab);
        } else if (i + 1 < n && i + 1 != active && i + 1 != hover) {
            fb.fill_rect(IRect(r.right() + kTabGap / 2 - 1, r.y + 8, 1, r.h - 16), kDivider); // separator
        }
        const TabInfo& t  = tabs[static_cast<size_t>(i)];
        int            lx = r.x + 10;
        // site badge (or a spinner-ish ring while loading)
        IRect badge(lx, r.y + (r.h - 16) / 2, 16, 16);
        if (r.w >= 48) {
            if (t.loading) {
                fb.stroke_round_rect(badge, 8, 8, 8, 8, 2, Color(190, 190, 200));
                fb.fill_rect(IRect(badge.x + 6, badge.y, 6, 3), kFocusRing); // the "moving" part of the ring
            } else if (t.icon && t.icon->w > 0 && t.icon->h > 0) {
                const Image& sc = t.icon->scaled(16, 16);
                fb.blit_tiled(sc, badge.x, badge.y, false, false, badge);
            } else if (t.host.empty()) {
                draw_icon(fb, "home", badge.x, badge.y, 16, kTextDim);
            } else {
                fb.fill_round_rect(badge, 4, 4, 4, 4, badge_color(t.host));
                Font*       bf = font(11, 700);
                std::string L  = badge_letter(t.host);
                int         bw = static_cast<int>(bf->text_width(L));
                text_at(fb, bf, L, badge.x + (16 - bw) / 2, badge, Color(255, 255, 255));
            }
            lx += 22;
        }
        bool close   = shows_close(tw, act);
        int  label_w = r.right() - lx - (close ? 30 : 10);
        if (label_w > 10) {
            fb.push_clip(IRect(lx, r.y, label_w, r.h));
            text_at(fb, f, ellipsize(t.title, 13, label_w), lx, r, act ? kText : kTextDim);
            fb.pop_clip();
        }
        if (close) {
            IRect cr = close_rect(r);
            if (i == hover && hover_close) fb.fill_round_rect(cr, 4, 4, 4, 4, act ? kBtnHover : Color(205, 205, 213));
            draw_icon(fb, "close", cr.x + 3, cr.y + 3, 14, kTextDim);
        }
    }
    if (n < kMaxTabs) {
        IRect b = plus_rect(n, W);
        if (hover_plus) fb.fill_round_rect(b, 6, 6, 6, 6, kHoverTab);
        draw_icon(fb, "add", b.x + (b.w - 18) / 2, b.y + (b.h - 18) / 2, 18, Color(60, 60, 70));
    }
}

// ---------------------------------------------------------------- toolbar
ToolbarLayout toolbar_layout(int W, int zoom_pct, int default_pct) {
    ToolbarLayout l;
    int y = kTabH + (kToolbarH - 32) / 2, x = 8;
    l.back    = IRect(x, y, 32, 32); x += 34;
    l.forward = IRect(x, y, 32, 32); x += 34;
    l.reload  = IRect(x, y, 32, 32); x += 34;
    l.home    = IRect(x, y, 32, 32); x += 40;
    l.field   = IRect(x, y, std::max(80, W - x - 10), 32);
    l.identity = IRect(l.field.x + 4, l.field.y + 4, 26, 24);
    l.text     = IRect(l.identity.right() + 4, l.field.y, l.field.right() - l.identity.right() - 14, l.field.h);
    if (zoom_pct != default_pct) {
        int zw = static_cast<int>(font(12)->text_width(std::to_string(zoom_pct) + "%")) + 16;
        l.zoom = IRect(l.field.right() - zw - 5, l.field.y + 5, zw, l.field.h - 10);
        l.text.w = std::max(20, l.zoom.x - 6 - l.text.x);
    }
    return l;
}

Button toolbar_hit(const ToolbarLayout& l, int x, int y) {
    if (l.back.contains(x, y)) return Button::Back;
    if (l.forward.contains(x, y)) return Button::Forward;
    if (l.reload.contains(x, y)) return Button::Reload;
    if (l.home.contains(x, y)) return Button::Home;
    if (l.identity.contains(x, y)) return Button::Identity;
    if (!l.zoom.empty() && l.zoom.contains(x, y)) return Button::Zoom;
    if (l.field.contains(x, y)) return Button::Field;
    return Button::None;
}

size_t text_index_at(const std::string& text, int px) {
    Font*  f = font(14);
    size_t i = 0;
    float  prev = 0;
    while (i < text.size()) {
        size_t n = i + 1;
        while (n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n++;
        float w = f->text_width(text.substr(0, n));
        if (px < (w + prev) / 2) return i;
        prev = w;
        i    = n;
    }
    return text.size();
}

int text_scroll(const ToolbarState& s, int avail) {
    if (!s.editing) return 0;
    Font* f       = font(14);
    int   caret_x = static_cast<int>(f->text_width(s.text.substr(0, std::min(s.cursor, s.text.size()))));
    return caret_x > avail - 4 ? caret_x - (avail - 4) : 0;
}

void draw_toolbar(Framebuffer& fb, const ToolbarState& s) {
    int           W = fb.w;
    ToolbarLayout l = toolbar_layout(W, s.zoom, s.default_zoom);
    fb.fill_rect(IRect(0, kTabH, W, kToolbarH), kToolbar);
    fb.fill_rect(IRect(0, kTop - 1, W, 1), kDivider);
    icon_button(fb, l.back, "back", s.can_back, s.hover == Button::Back);
    icon_button(fb, l.forward, "forward", s.can_forward, s.hover == Button::Forward);
    icon_button(fb, l.reload, s.loading ? "close" : "reload", true, s.hover == Button::Reload);
    icon_button(fb, l.home, "home", true, s.hover == Button::Home);

    // address field
    if (s.editing) {
        fb.fill_round_rect(IRect(l.field.x - 2, l.field.y - 2, l.field.w + 4, l.field.h + 4), 9, 9, 9, 9, kFocusRing);
        fb.fill_round_rect(l.field, 7, 7, 7, 7, Color(255, 255, 255));
    } else {
        bool hov = s.hover == Button::Field || s.hover == Button::Identity;
        fb.fill_round_rect(l.field, 7, 7, 7, 7, hov ? kFieldHover : kField);
    }

    // security indicator
    const char* icon = nullptr;
    Color       ic   = kTextDim;
    switch (s.security) {
    case Security::Secure: icon = "shield-check"; ic = kGreen; break;
    case Security::TrustedKey: icon = "key"; ic = kAmber; break;
    case Security::LocalHttp: icon = "lock-open"; ic = kTextDim; break;
    case Security::Insecure: icon = "lock-open"; ic = kRed; break;
    case Security::Internal: icon = "info"; ic = kTextDim; break;
    case Security::None: icon = s.editing ? "search" : nullptr; break;
    }
    if (icon) {
        if (s.info_open || (s.hover == Button::Identity && s.security != Security::None))
            fb.fill_round_rect(l.identity, 5, 5, 5, 5, s.info_open ? Color(214, 214, 222) : kBtnHover);
        draw_icon(fb, icon, l.identity.x + (l.identity.w - 18) / 2, l.identity.y + (l.identity.h - 18) / 2, 18, ic);
    }

    Font* f = font(14);
    fb.push_clip(l.text);
    if (s.editing) {
        int off = text_scroll(s, l.text.w);
        int tx  = l.text.x - off;
        if (s.select_all && !s.text.empty()) {
            int w = static_cast<int>(f->text_width(s.text));
            fb.fill_rect(IRect(tx, l.field.y + 7, w, l.field.h - 14), kSelection);
        }
        text_at(fb, f, s.text, tx, l.field, kText);
        if (!s.select_all) {
            int cx = tx + static_cast<int>(f->text_width(s.text.substr(0, std::min(s.cursor, s.text.size()))));
            fb.fill_rect(IRect(cx, l.field.y + 7, 1, l.field.h - 14), kText);
        }
    } else if (s.url.empty()) {
        text_at(fb, f, "Search or enter address", l.text.x, l.field, kTextFaint);
    } else {
        // Like Firefox: https:// hidden, the host in full contrast (that's what identifies the site), the rest dimmed.
        std::string u = s.url, scheme, host, rest;
        size_t      p = u.find("://");
        if (p != std::string::npos && (u.compare(0, p, "https") == 0 || u.compare(0, p, "http") == 0)) {
            scheme   = u.substr(0, p + 3);
            size_t e = u.find_first_of("/?#", p + 3);
            host     = u.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3);
            rest     = e == std::string::npos ? "" : u.substr(e);
            if (rest == "/") rest.clear();
        } else {
            rest = u;
        }
        int x = l.text.x;
        if (!scheme.empty() && scheme != "https://") { // plain http stays visible: it's the warning
            text_at(fb, f, scheme, x, l.field, s.security == Security::Insecure ? kRed : kTextDim);
            x += static_cast<int>(f->text_width(scheme));
        }
        if (!host.empty()) {
            text_at(fb, f, host, x, l.field, kText);
            x += static_cast<int>(f->text_width(host));
        }
        text_at(fb, f, rest, x, l.field, host.empty() ? kText : kTextDim);
    }
    fb.pop_clip();
    if (!l.zoom.empty()) { // the zoom badge: click to reset
        bool hov = s.hover == Button::Zoom;
        fb.fill_round_rect(l.zoom, l.zoom.h / 2, l.zoom.h / 2, l.zoom.h / 2, l.zoom.h / 2, hov ? Color(200, 200, 210) : Color(222, 222, 230));
        Font* zf = font(12);
        std::string t = std::to_string(s.zoom) + "%";
        int tw = static_cast<int>(zf->text_width(t));
        zf->draw(fb, t, l.zoom.x + (l.zoom.w - tw) / 2, l.zoom.y + (l.zoom.h + zf->ascent - zf->descent) / 2, kText);
    }
}

// ---------------------------------------------------------------- status bubble
void draw_status(Framebuffer& fb, const IRect& content, const std::string& text) {
    if (text.empty()) return;
    Font*       f = font(12);
    int         maxw = std::max(120, content.w * 2 / 3);
    std::string t = ellipsize(text, 12, maxw - 16);
    int         w = static_cast<int>(f->text_width(t)) + 16, h = 22;
    IRect       r(content.x + 4, content.bottom() - h - 4, w, h);
    soft_shadow(fb, r, 5, 2);
    fb.fill_round_rect(r, 5, 5, 5, 5, Color(252, 252, 253));
    fb.stroke_round_rect(r, 5, 5, 5, 5, 1, kDivider);
    text_at(fb, f, t, r.x + 8, r, Color(50, 50, 60));
}

// ---------------------------------------------------------------- site information panel
namespace {
struct InfoText {
    const char* icon;
    Color       color;
    std::string title, body;
};
InfoText info_text(const SiteInfo& i) {
    switch (i.security) {
    case Security::Secure:
        return {"shield-check", kGreen, "Connection secure",
                "Quill verified the certificate of " + i.host + " and the connection is encrypted (TLS 1.2 or newer)." +
                    (i.hsts ? " This site only allows secure connections (HSTS), so it is never loaded over plain http." : "")};
    case Security::TrustedKey:
        return {"key", kAmber, "Trusted by you",
                i.host + " is a device on your own network. Its certificate isn't from a public authority, but it matches the key "
                         "you chose to trust, so the connection is encrypted and it is that same device."};
    case Security::LocalHttp:
        return {"lock-open", kTextDim, "Local network, not encrypted",
                i.host + " is on your own network and was loaded over plain http. Others on the same network could see "
                         "this traffic."};
    case Security::Insecure:
        return {"lock-open", kRed, "Connection not secure",
                i.host + " was loaded over plain http. Anyone on the network path can read or change it, so don't type "
                         "passwords or personal details here."};
    case Security::Internal:
        return {"info", kTextDim, "Quill page", "This page is part of Quill itself. Nothing was loaded from the internet."};
    case Security::None: break;
    }
    return {"info", kTextDim, "", ""};
}
constexpr int kInfoW = 340;
} // namespace

IRect site_info_rect(int W, const SiteInfo& info) {
    ToolbarLayout            l = toolbar_layout(W);
    InfoText                 t = info_text(info);
    std::vector<std::string> lines;
    wrap_text(font(13), t.body, kInfoW - 32, lines);
    int h = 16 + 26 + 8 + static_cast<int>(lines.size()) * 19 + 16;
    return IRect(std::min(l.field.x, W - kInfoW - 8), kTop + 2, kInfoW, h);
}

void draw_site_info(Framebuffer& fb, const SiteInfo& info) {
    IRect    r = site_info_rect(fb.w, info);
    InfoText t = info_text(info);
    soft_shadow(fb, r, 8, 5);
    fb.fill_round_rect(r, 8, 8, 8, 8, Color(255, 255, 255));
    fb.stroke_round_rect(r, 8, 8, 8, 8, 1, kDivider);
    draw_icon(fb, t.icon, r.x + 16, r.y + 17, 22, t.color);
    Font* tf = font(15, 700);
    text_at(fb, tf, t.title, r.x + 46, IRect(r.x, r.y + 14, r.w, 28), kText);
    Font*                    bf = font(13);
    std::vector<std::string> lines;
    wrap_text(bf, t.body, kInfoW - 32, lines);
    int y = r.y + 16 + 26 + 8;
    for (auto& ln : lines) {
        text_at(fb, bf, ln, r.x + 16, IRect(r.x, y, r.w, 19), kTextDim);
        y += 19;
    }
}

// ---------------------------------------------------------------- context menu
int Menu::row_y(size_t i) const {
    int yy = y + kPad;
    for (size_t k = 0; k < i && k < items.size(); k++) yy += items[k].id ? kRowH : kSepH;
    return yy;
}
int Menu::height() const { return row_y(items.size()) - y + kPad; }
int Menu::item_at(int mx, int my) const {
    if (mx < x || mx >= x + w) return -1;
    for (size_t i = 0; i < items.size(); i++) {
        int top = row_y(i), hh = items[i].id ? kRowH : kSepH;
        if (my >= top && my < top + hh) return items[i].id ? static_cast<int>(i) : -1;
    }
    return -1;
}
void Menu::place(int mx, int my, int W, int H) {
    Font* f  = font(13);
    float mw = 180;
    for (auto& it : items) mw = std::max(mw, f->text_width(it.label) + 44);
    w     = static_cast<int>(mw);
    x     = std::max(0, std::min(mx, W - w - 4));
    y     = std::max(0, std::min(my, H - height() - 4));
    hover = -1;
    open  = true;
}

void draw_menu(Framebuffer& fb, const Menu& m) {
    IRect r(m.x, m.y, m.w, m.height());
    soft_shadow(fb, r, 8, 5);
    fb.fill_round_rect(r, 8, 8, 8, 8, Color(255, 255, 255));
    fb.stroke_round_rect(r, 8, 8, 8, 8, 1, kDivider);
    Font* f = font(13);
    for (size_t i = 0; i < m.items.size(); i++) {
        const MenuItem& it = m.items[i];
        int             yy = m.row_y(i);
        if (!it.id) {
            fb.fill_rect(IRect(m.x + 10, yy + 4, m.w - 20, 1), Color(228, 228, 234));
            continue;
        }
        bool hot = static_cast<int>(i) == m.hover && it.enabled;
        if (hot) fb.fill_round_rect(IRect(m.x + 4, yy, m.w - 8, Menu::kRowH), 5, 5, 5, 5, Color(235, 235, 241));
        text_at(fb, f, it.label, m.x + 16, IRect(m.x, yy, m.w, Menu::kRowH), it.enabled ? kText : kTextFaint);
    }
}

} // namespace quill::ui
