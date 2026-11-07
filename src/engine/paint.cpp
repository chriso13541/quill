// Painting. See paint.h.
#include "paint.h"

#include "font.h"
#include "image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <memory>

namespace quill::engine {
namespace {

quill::Font* font_for(const Style& s) {
    uint64_t gen = FontManager::instance().generation();
    if (s.font_memo && s.font_memo_fam == static_cast<const void*>(s.font_family.data()) && s.font_memo_n == s.font_family.size() &&
        s.font_memo_size == s.font_size && s.font_memo_wt == s.font_weight && s.font_memo_it == s.italic && s.font_memo_gen == gen)
        return static_cast<quill::Font*>(s.font_memo);
    quill::Font* f = nullptr;
    {
        std::string fam;
        for (size_t i = 0; i < s.font_family.size(); ++i) fam += (i ? ", " : "") + s.font_family[i];
        if (fam.empty()) fam = "sans-serif";
        f = FontManager::instance().get(fam, s.font_size, s.font_weight, s.italic, s.font_scope);
    }
    // FontManager keeps its fonts for the life of the program, so the pointer stays valid
    s.font_memo      = f;
    s.font_memo_fam  = s.font_family.data();
    s.font_memo_n    = s.font_family.size();
    s.font_memo_size = s.font_size;
    s.font_memo_wt   = s.font_weight;
    s.font_memo_it   = s.italic;
    s.font_memo_gen  = gen;
    return f;
}

// ---- gradients: rendered into a w x h image (straight alpha), then drawn like a background image
struct RStop { float t; float r, g, b, a; }; // premultiplied
std::vector<RStop> resolve_stops(const Gradient& g, float len) {
    std::vector<RStop> out;
    size_t n = g.stops.size();
    std::vector<float> pos(n, 0);
    std::vector<bool>  has(n, false);
    for (size_t i = 0; i < n; i++) {
        if (g.stops[i].has_pos) {
            pos[i] = len > 0 ? g.stops[i].pos.resolve(len) / len : 0;
            has[i] = true;
        }
    }
    if (n && !has[0]) { pos[0] = 0; has[0] = true; }
    if (n > 1 && !has[n - 1]) { pos[n - 1] = 1; has[n - 1] = true; }
    for (size_t i = 1; i < n; i++) if (has[i] && pos[i] < pos[i - 1]) pos[i] = pos[i - 1]; // never go backwards
    for (size_t i = 0; i < n;) { // spread stops without a position evenly between their neighbours
        size_t j = i + 1;
        while (j < n && !has[j]) j++;
        if (j < n)
            for (size_t k = i + 1; k < j; k++) pos[k] = pos[i] + (pos[j] - pos[i]) * static_cast<float>(k - i) / static_cast<float>(j - i);
        i = j;
    }
    for (size_t i = 1; i < n; i++) if (pos[i] < pos[i - 1]) pos[i] = pos[i - 1];
    for (size_t i = 0; i < n; i++) {
        Color c = g.stops[i].color;
        float a = static_cast<float>((c >> 24) & 255) / 255.f;
        out.push_back({pos[i], ((c >> 16) & 255) * a, ((c >> 8) & 255) * a, (c & 255) * a, a * 255.f});
    }
    return out;
}

uint32_t stop_color(const std::vector<RStop>& st, float t, bool repeating) {
    if (st.empty()) return 0;
    if (repeating && st.size() > 1) {
        float t0 = st.front().t, t1 = st.back().t, span = t1 - t0;
        if (span > 1e-6f) { t = std::fmod(t - t0, span); if (t < 0) t += span; t += t0; }
    }
    const RStop* a = &st.front();
    const RStop* b = a;
    if (t <= st.front().t) b = a;
    else if (t >= st.back().t) a = b = &st.back();
    else
        for (size_t i = 1; i < st.size(); i++)
            if (t <= st[i].t) { a = &st[i - 1]; b = &st[i]; break; }
    float f = b->t > a->t ? (t - a->t) / (b->t - a->t) : 1.f;
    float al = a->a + (b->a - a->a) * f;
    if (al < 0.5f) return 0;
    float r = a->r + (b->r - a->r) * f, gg = a->g + (b->g - a->g) * f, bb = a->b + (b->b - a->b) * f;
    float k = 255.f / al; // back to straight alpha
    auto  ch = [](float v) { return static_cast<uint32_t>(std::clamp(v + 0.5f, 0.f, 255.f)); };
    return (ch(al) << 24) | (ch(r * k) << 16) | (ch(gg * k) << 8) | ch(bb * k);
}

std::shared_ptr<quill::Image> render_gradient(const Gradient& g, int w, int h) {
    auto img = std::make_shared<quill::Image>();
    img->w   = w;
    img->h   = h;
    img->px.assign(static_cast<size_t>(w) * h, 0);
    float W = static_cast<float>(w), H = static_cast<float>(h);
    if (g.kind == 0) {
        float ang = g.angle;
        if (g.corner_x || g.corner_y) ang = std::atan2(g.corner_x * H, -g.corner_y * W) * 57.2957795f; // perpendicular to the other diagonal
        float r = ang * 0.0174532925f, dx = std::sin(r), dy = -std::cos(r);
        float len = std::fabs(W * dx) + std::fabs(H * dy);
        if (len < 1e-3f) len = 1;
        std::vector<RStop> st = resolve_stops(g, len);
        if (g.repeating) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float t = ((x + 0.5f - W / 2) * dx + (y + 0.5f - H / 2) * dy) / len + 0.5f;
                    img->px[static_cast<size_t>(y) * w + x] = stop_color(st, t, true);
                }
            return img;
        }
        // The colour along the line, 4 samples per pixel of length, as running sums of premultiplied colour: each pixel
        // gets the average over the stretch of the line it covers, so hard stops and thin lines come out anti-aliased.
        int n = std::clamp(static_cast<int>(len * 4) + 1, 2, 32768);
        std::vector<float> sum(static_cast<size_t>(n + 1) * 4, 0.f);
        for (int i = 0; i < n; i++) {
            uint32_t c = stop_color(st, (i + 0.5f) / n, false);
            float    a = static_cast<float>(c >> 24);
            float    v[4] = {((c >> 16) & 255) * a / 255.f, ((c >> 8) & 255) * a / 255.f, (c & 255) * a / 255.f, a};
            for (int k = 0; k < 4; k++) sum[static_cast<size_t>(i + 1) * 4 + k] = sum[static_cast<size_t>(i) * 4 + k] + v[k];
        }
        float hw = 0.5f * (std::fabs(dx) + std::fabs(dy)) / len; // half a pixel, in t
        auto  at = [&](float t, int k) { // running sum at t (clamped: before 0 / after 1 the end colours extend)
            float f = t * n;
            if (f <= 0) return f * (sum[4 + static_cast<size_t>(k)] - sum[static_cast<size_t>(k)]);
            if (f >= n) return sum[static_cast<size_t>(n) * 4 + k] + (f - n) * (sum[static_cast<size_t>(n) * 4 + k] - sum[static_cast<size_t>(n - 1) * 4 + k]);
            int   i = static_cast<int>(f);
            float r = f - i;
            return sum[static_cast<size_t>(i) * 4 + k] + r * (sum[static_cast<size_t>(i + 1) * 4 + k] - sum[static_cast<size_t>(i) * 4 + k]);
        };
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float t = ((x + 0.5f - W / 2) * dx + (y + 0.5f - H / 2) * dy) / len + 0.5f;
                float t0 = t - hw, t1 = t + hw, span = (t1 - t0) * n;
                float v[4];
                for (int k = 0; k < 4; k++) v[k] = (at(t1, k) - at(t0, k)) / span;
                uint32_t out = 0;
                if (v[3] >= 0.5f) {
                    float k2 = 255.f / v[3];
                    auto  ch = [](float q) { return static_cast<uint32_t>(std::clamp(q + 0.5f, 0.f, 255.f)); };
                    out = (ch(v[3]) << 24) | (ch(v[0] * k2) << 16) | (ch(v[1] * k2) << 8) | ch(v[2] * k2);
                }
                img->px[static_cast<size_t>(y) * w + x] = out;
            }
        return img;
    }
    float cx = g.cx.resolve(W), cy = g.cy.resolve(H);
    if (g.kind == 2) { // conic
        std::vector<RStop> st = resolve_stops(g, 100.f); // positions are % of a turn
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float a = std::atan2(x + 0.5f - cx, -(y + 0.5f - cy)) * 57.2957795f - g.angle; // 0 = up, clockwise
                a = std::fmod(a, 360.f);
                if (a < 0) a += 360.f;
                img->px[static_cast<size_t>(y) * w + x] = stop_color(st, a / 360.f, g.repeating);
            }
        return img;
    }
    // radial
    float dl = cx, dr = W - cx, dt = cy, db = H - cy;
    float rx, ry;
    auto  corner = [&](bool far) {
        float bx = far ? std::max(std::fabs(dl), std::fabs(dr)) : std::min(std::fabs(dl), std::fabs(dr));
        float by = far ? std::max(std::fabs(dt), std::fabs(db)) : std::min(std::fabs(dt), std::fabs(db));
        return std::make_pair(bx, by);
    };
    switch (g.size_kw) {
    case 0: case 1: case 2: case 3: {
        bool far = g.size_kw >= 2;
        auto side = corner(far);
        if (g.circle) {
            float r = g.size_kw == 0 ? std::min(side.first, side.second) : g.size_kw == 2 ? std::max(side.first, side.second)
                                                                           : std::hypot(side.first, side.second);
            rx = ry = r;
        } else {
            rx = side.first;
            ry = side.second;
            if (g.size_kw == 1 || g.size_kw == 3) { rx *= 1.41421356f; ry *= 1.41421356f; } // through the corner, same aspect
        }
        break;
    }
    default:
        rx = g.rx.resolve(W);
        ry = g.circle ? rx : g.ry.resolve(H);
    }
    rx = std::max(rx, 0.5f);
    ry = std::max(ry, 0.5f);
    std::vector<RStop> st = resolve_stops(g, rx);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float ex = (x + 0.5f - cx) / rx, ey = (y + 0.5f - cy) / ry;
            img->px[static_cast<size_t>(y) * w + x] = stop_color(st, std::sqrt(ex * ex + ey * ey), g.repeating);
        }
    return img;
}

quill::Color qc(Color c) {
    return quill::Color(static_cast<uint8_t>((c >> 16) & 0xff), static_cast<uint8_t>((c >> 8) & 0xff), static_cast<uint8_t>(c & 0xff),
                        static_cast<uint8_t>((c >> 24) & 0xff));
}
Color shade(Color c, float f) { // f < 1 darkens, > 1 lightens (towards white)
    auto ch = [&](int sh) {
        float v = static_cast<float>((c >> sh) & 0xff);
        v = f < 1 ? v * f : v + (255 - v) * (f - 1);
        return static_cast<uint32_t>(std::clamp(v, 0.f, 255.f));
    };
    return (c & 0xff000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

struct Painter {
    quill::Framebuffer& fb;
    float               sx, sy;
    const PaintEnv&     env;
    float               fx = 0, fy = 0; // framebuffer origin of position:fixed content
    const Box*          canvas_owner = nullptr; // its background was propagated to the canvas: don't paint it again

    // Culling: a subtree / line whose ink lies entirely above or below the clip is skipped, so a frame costs what is
    // on screen rather than the whole document. kCullSlack covers glyph overhang past the line box.
    static constexpr float kCullSlack = 48;
    bool off_screen(float y0, float y1) const {
        quill::IRect c = fb.clip();
        return y1 - sy < static_cast<float>(c.y) - kCullSlack || y0 - sy > static_cast<float>(c.bottom()) + kCullSlack;
    }
    bool culled(const Box& b) const { return env.cull && b.ink_valid && &b != noclip && off_screen(b.ink_y0, b.ink_y1); }
    // A marquee painted by a shifted painter: its own clip and background were done unshifted by the caller.
    const Box* noclip = nullptr;

    // The frame of an animated image to draw now (still images unchanged); records when the next one is due.
    const quill::Image* frame(const quill::Image* img) const {
        if (!img || env.anim_ms < 0 || !img->animated()) return img;
        int next = -1;
        const quill::Image& f = img->frame_at(env.anim_ms, &next);
        if (env.anim_next_ms && next >= 0 && (*env.anim_next_ms < 0 || next < *env.anim_next_ms)) *env.anim_next_ms = next;
        return &f;
    }
    // Same, but only if the box is on screen: off-screen animations must not keep waking the UI.
    const quill::Image* frame_if_visible(const quill::Image* img, const quill::IRect& r) const {
        if (!img || !img->animated()) return img;
        if (r.intersect(quill::IRect(0, 0, fb.w, fb.h)).empty()) return img;
        return frame(img);
    }

    int X(float v) const { return static_cast<int>(std::lround(v - sx)); }
    int Y(float v) const { return static_cast<int>(std::lround(v - sy)); }
    quill::IRect R(float x, float y, float w, float h) const {
        int x0 = X(x), y0 = Y(y), x1 = X(x + w), y1 = Y(y + h);
        return quill::IRect(x0, y0, x1 - x0, y1 - y0);
    }


    // ---- rounded rects (elliptical radii), shadows
    struct RR {
        quill::IRect r;
        float rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0}; // tl tr br bl
        bool round() const { for (int i = 0; i < 4; ++i) if (rx[i] > 0 && ry[i] > 0) return true; return false; }
    };
    static bool has_radius(const Style& s) {
        for (auto& r : s.radius) if ((r.x.v > 0 && r.y.v > 0) || (r.x.is_pct() && r.x.v > 0)) return true;
        return false;
    }
    RR make_rr(const Style& s, const quill::IRect& r) const {
        RR o;
        o.r = r;
        float w = static_cast<float>(r.w), h = static_cast<float>(r.h);
        for (int i = 0; i < 4; ++i) {
            o.rx[i] = std::max(0.f, s.radius[i].x.resolve(w));
            o.ry[i] = std::max(0.f, s.radius[i].y.resolve(h));
        }
        float f = 1;
        auto lim = [&](float side, float sum) { if (sum > side && sum > 0) f = std::min(f, side / sum); };
        lim(w, o.rx[0] + o.rx[1]); lim(w, o.rx[3] + o.rx[2]);
        lim(h, o.ry[0] + o.ry[3]); lim(h, o.ry[1] + o.ry[2]);
        for (int i = 0; i < 4; ++i) { o.rx[i] *= f; o.ry[i] *= f; }
        return o;
    }
    // Shrinks (d > 0) or grows (d < 0) a rounded rect by per-side amounts t r b l.
    static RR inset_rr(const RR& a, float t, float r, float b, float l) {
        RR o;
        o.r = quill::IRect(a.r.x + static_cast<int>(l), a.r.y + static_cast<int>(t), a.r.w - static_cast<int>(l + r), a.r.h - static_cast<int>(t + b));
        const float dx[4] = {l, r, r, l}, dy[4] = {t, t, b, b};
        for (int i = 0; i < 4; ++i) {
            o.rx[i] = a.rx[i] > 0 ? std::max(0.f, a.rx[i] - dx[i]) : 0;
            o.ry[i] = a.ry[i] > 0 ? std::max(0.f, a.ry[i] - dy[i]) : 0;
        }
        return o;
    }
    // Coverage 0..1 of pixel (px,py) by the rounded rect.
    static float cov(const RR& a, int px, int py) {
        const quill::IRect& r = a.r;
        if (r.empty()) return 0;
        float x = px + 0.5f, y = py + 0.5f;
        float ex = std::clamp(std::min(x - r.x, r.right() - x) + 0.5f, 0.f, 1.f);
        float ey = std::clamp(std::min(y - r.y, r.bottom() - y) + 0.5f, 0.f, 1.f);
        float c = ex * ey;
        if (c <= 0) return 0;
        for (int k = 0; k < 4; ++k) {
            float rx = a.rx[k], ry = a.ry[k];
            if (rx <= 0 || ry <= 0) continue;
            float cx = (k == 0 || k == 3) ? r.x + rx : r.right() - rx;
            float cy = (k == 0 || k == 1) ? r.y + ry : r.bottom() - ry;
            float dx = (k == 0 || k == 3) ? cx - x : x - cx;
            float dy = (k == 0 || k == 1) ? cy - y : y - cy;
            if (dx <= 0 || dy <= 0) continue;
            float d = std::hypot(dx / rx, dy / ry);
            if (d <= 0.0001f) return 1;
            float L = std::hypot(dx, dy);
            float dist = L / d - L; // + inside
            return std::clamp(0.5f + dist, 0.f, 1.f);
        }
        return c;
    }
    // [x0, x1): pixels of row y that the rounded rect covers fully (cov() is exactly 1 there), so a caller can fill or
    // skip them as one run and work out coverage only near the corners. Kept a pixel short of the exact bound on
    // each side, so rounding can never put a partly covered pixel inside.
    static void full_span(const RR& a, int y, int& x0, int& x1) {
        const quill::IRect& r = a.r;
        x0 = r.x;
        x1 = r.right();
        if (r.empty() || y < r.y || y >= r.bottom()) { x1 = x0; return; }
        float py = y + 0.5f;
        for (int k = 0; k < 4; ++k) {
            float rx = a.rx[k], ry = a.ry[k];
            if (rx <= 0 || ry <= 0) continue;
            bool top = k == 0 || k == 1;
            float cy = top ? r.y + ry : r.bottom() - ry;
            if (top ? !(py < cy) : !(py > cy)) continue; // this row is clear of that corner's curve
            if (k == 0 || k == 3) x0 = std::max(x0, static_cast<int>(std::ceil(r.x + rx - 0.5f)) + 1);
            else x1 = std::min(x1, static_cast<int>(std::floor(r.right() - rx - 0.5f)));
        }
        if (x1 < x0) x1 = x0;
    }
    void fill_rr(const RR& a, quill::Color c) {
        if (!a.round()) { fb.fill_rect(a.r, c); return; }
        quill::IRect v = a.r.intersect(fb.clip());
        for (int y = v.y; y < v.bottom(); ++y) {
            int s0, s1;
            full_span(a, y, s0, s1);
            s0 = std::clamp(s0, v.x, v.right());
            s1 = std::clamp(s1, s0, v.right());
            for (int x = v.x; x < v.right(); ++x) {
                if (x == s0 && s1 > s0) {
                    fb.fill_rect(quill::IRect(s0, y, s1 - s0, 1), c); // == blend_px(.., 255) pixel by pixel
                    x = s1 - 1;
                    continue;
                }
                float k = cov(a, x, y);
                if (k > 0) fb.blend_px(x, y, c, static_cast<int>(k * 255 + 0.5f));
            }
        }
    }
    // Ring between outer and inner rounded rects, split among the four border sides.
    void ring_rr(const RR& outer, const RR& inner, const float* bws, const quill::Color* cols, const bool* draw) {
        quill::IRect v = outer.r.intersect(fb.clip());
        const quill::IRect& o = outer.r;
        for (int y = v.y; y < v.bottom(); ++y) {
            int s0, s1; // inside the inner edge: nothing of the ring there
            full_span(inner, y, s0, s1);
            for (int x = v.x; x < v.right(); ++x) {
                if (x >= s0 && x < s1) { x = s1 - 1; continue; }
                float k = cov(outer, x, y);
                if (k <= 0) continue;
                k *= 1.f - cov(inner, x, y);
                if (k <= 0) continue;
                // nearest side by normalised distance (mitre approximation)
                float dt = bws[0] > 0 ? (y + 0.5f - o.y) / bws[0] : 1e9f, db = bws[2] > 0 ? (o.bottom() - y - 0.5f) / bws[2] : 1e9f;
                float dl = bws[3] > 0 ? (x + 0.5f - o.x) / bws[3] : 1e9f, dr = bws[1] > 0 ? (o.right() - x - 0.5f) / bws[1] : 1e9f;
                int side = 0; float best = dt;
                if (dr < best) { best = dr; side = 1; }
                if (db < best) { best = db; side = 2; }
                if (dl < best) { best = dl; side = 3; }
                if (!draw[side]) continue;
                fb.blend_px(x, y, cols[side], static_cast<int>(k * 255 + 0.5f));
            }
        }
    }
    // Rounded clipping approximation: remember the pixels under the box, restore the corners afterwards.
    struct Snap { quill::IRect r; std::vector<uint32_t> px; RR rr; bool on = false; };
    Snap begin_clip(const Box& b) {
        Snap sn;
        const Style& s = *b.style;
        quill::IRect cr = clip_rect(b);
        if (has_radius(s) && cr.w > 0 && cr.h > 0) {
            RR outer = make_rr(s, R(b.x, b.y, b.w, b.h));
            sn.rr = inset_rr(outer, b.border.top, b.border.right, b.border.bottom, b.border.left);
            sn.r = sn.rr.r.intersect(quill::IRect(0, 0, fb.w, fb.h));
            if (sn.rr.round() && !sn.r.empty() && static_cast<long>(sn.r.w) * sn.r.h < 8000000) {
                sn.on = true;
                sn.px.resize(static_cast<size_t>(sn.r.w) * sn.r.h);
                for (int y = 0; y < sn.r.h; ++y)
                    std::copy_n(&fb.px[static_cast<size_t>(sn.r.y + y) * fb.w + sn.r.x], sn.r.w, &sn.px[static_cast<size_t>(y) * sn.r.w]);
            }
        }
        fb.push_clip(cr);
        return sn;
    }
    void end_clip(const Snap& sn) {
        fb.pop_clip();
        if (!sn.on) return;
        restore_outside(sn);
    }
    void restore_outside(const Snap& sn) {
        quill::IRect v = sn.r.intersect(fb.clip());
        for (int y = v.y; y < v.bottom(); ++y) {
            int s0, s1; // fully inside: the box's own pixels stay
            full_span(sn.rr, y, s0, s1);
            for (int x = v.x; x < v.right(); ++x) {
                if (x >= s0 && x < s1) { x = s1 - 1; continue; }
                float k = cov(sn.rr, x, y);
                if (k >= 1) continue;
                uint32_t old = sn.px[static_cast<size_t>(y - sn.r.y) * sn.r.w + (x - sn.r.x)];
                uint32_t& cur = fb.px[static_cast<size_t>(y) * fb.w + x];
                if (k <= 0) { cur = old; continue; }
                int a = static_cast<int>(k * 256);
                uint32_t out = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int o = (old >> sh) & 0xff, n = (cur >> sh) & 0xff;
                    out |= static_cast<uint32_t>((o * (256 - a) + n * a) >> 8) << sh;
                }
                cur = out;
            }
        }
    }

    static void box_blur(std::vector<float>& a, int w, int h, int r) {
        if (r <= 0) return;
        std::vector<float> t(a.size());
        float inv = 1.f / (2 * r + 1);
        for (int y = 0; y < h; ++y) { // horizontal
            const float* in = &a[static_cast<size_t>(y) * w];
            float* out = &t[static_cast<size_t>(y) * w];
            float sum = 0;
            for (int x = -r; x <= r; ++x) sum += (x >= 0 && x < w) ? in[x] : 0;
            for (int x = 0; x < w; ++x) {
                out[x] = sum * inv;
                int add = x + r + 1, del = x - r;
                sum += (add < w ? in[add] : 0) - (del >= 0 ? in[del] : 0);
            }
        }
        for (int x = 0; x < w; ++x) { // vertical
            float sum = 0;
            for (int y = -r; y <= r; ++y) sum += (y >= 0 && y < h) ? t[static_cast<size_t>(y) * w + x] : 0;
            for (int y = 0; y < h; ++y) {
                a[static_cast<size_t>(y) * w + x] = sum * inv;
                int add = y + r + 1, del = y - r;
                sum += (add < h ? t[static_cast<size_t>(add) * w + x] : 0) - (del >= 0 ? t[static_cast<size_t>(del) * w + x] : 0);
            }
        }
    }
    // Blurs `a` approximating a gaussian of the given sigma with three box passes.
    static void gauss(std::vector<float>& a, int w, int h, float sigma) {
        if (sigma < 0.3f) return;
        int r = std::max(1, static_cast<int>(std::lround(std::sqrt(12 * sigma * sigma / 3 + 1) / 2)));
        for (int i = 0; i < 3; ++i) box_blur(a, w, h, r);
    }

    void paint_shadows(const Box& b, const quill::IRect& border_box, bool inset_pass) {
        const Style& s = *b.style;
        if (s.shadows.empty()) return;
        RR bb = make_rr(s, border_box);
        for (size_t idx = s.shadows.size(); idx-- > 0;) { // first shadow on top
            const BoxShadow& sh = s.shadows[idx];
            if (sh.inset != inset_pass) continue;
            quill::Color col = qc(sh.current ? s.color : sh.color);
            if (col.a == 0) continue;
            float sigma = sh.blur / 2;
            int ext = static_cast<int>(std::ceil(sigma * 1.9f)) + 2;
            int ox = static_cast<int>(std::lround(sh.ox)), oy = static_cast<int>(std::lround(sh.oy)), sp = static_cast<int>(std::lround(sh.spread));
            RR shape;
            quill::IRect area; // pixels affected
            if (!inset_pass) {
                shape = inset_rr(bb, -sp, -sp, -sp, -sp);
                shape.r.x += ox; shape.r.y += oy;
                for (int i = 0; i < 4; ++i) if (bb.rx[i] <= 0 || bb.ry[i] <= 0) { shape.rx[i] = shape.ry[i] = 0; }
                area = quill::IRect(shape.r.x - ext, shape.r.y - ext, shape.r.w + 2 * ext, shape.r.h + 2 * ext);
            } else {
                RR pad = inset_rr(bb, b.border.top, b.border.right, b.border.bottom, b.border.left);
                // the blur reaches in from outside the padding box: give it that apron, or the edges come out bare
                area = quill::IRect(pad.r.x - ext, pad.r.y - ext, pad.r.w + 2 * ext, pad.r.h + 2 * ext);
                shape = inset_rr(pad, sp, sp, sp, sp);
                shape.r.x += ox; shape.r.y += oy;
            }
            quill::IRect clipr = fb.clip();
            // limit work to the visible part (plus the blur apron)
            quill::IRect vis = area.intersect(quill::IRect(clipr.x - ext, clipr.y - ext, clipr.w + 2 * ext, clipr.h + 2 * ext));
            if (vis.empty() || static_cast<long>(vis.w) * vis.h > 6000000) continue;
            std::vector<float> m(static_cast<size_t>(vis.w) * vis.h);
            for (int y = 0; y < vis.h; ++y) {
                int s0, s1;
                full_span(shape, vis.y + y, s0, s1);
                for (int x = 0; x < vis.w; ++x) {
                    int   ax = vis.x + x;
                    float c  = ax >= s0 && ax < s1 ? 1.f : cov(shape, ax, vis.y + y);
                    m[static_cast<size_t>(y) * vis.w + x] = inset_pass ? 1.f - c : c;
                }
            }
            gauss(m, vis.w, vis.h, sigma);
            RR pad = inset_rr(bb, b.border.top, b.border.right, b.border.bottom, b.border.left);
            quill::IRect write = vis.intersect(clipr);
            for (int y = write.y; y < write.bottom(); ++y) {
                int s0, s1; // an outer shadow is hidden under the box itself
                if (inset_pass) s0 = s1 = 0;
                else full_span(bb, y, s0, s1);
                for (int x = write.x; x < write.right(); ++x) {
                    if (x >= s0 && x < s1) { x = s1 - 1; continue; }
                    float k = m[static_cast<size_t>(y - vis.y) * vis.w + (x - vis.x)];
                    k *= inset_pass ? cov(pad, x, y) : 1.f - cov(bb, x, y);
                    if (k > 0.003f) fb.blend_px(x, y, col, static_cast<int>(k * 255 + 0.5f));
                }
            }
        }
    }

    // ---- borders / backgrounds
    void draw_border_side(int side, const Style& s, const float* bws, float x, float y, float w, float h) {
        float bw = bws[side];
        if (bw <= 0) return;
        BorderStyle bs = s.border_style[side];
        Color       c  = s.border_color[side];
        if (bs == BorderStyle::Inset || bs == BorderStyle::Outset || bs == BorderStyle::Groove || bs == BorderStyle::Ridge) {
            bool top_left = side == 0 || side == 3;
            bool dark = (bs == BorderStyle::Inset || bs == BorderStyle::Groove) ? top_left : !top_left;
            c = shade(c, dark ? 0.6f : 1.0f);
            if (!dark && (bs == BorderStyle::Outset || bs == BorderStyle::Ridge)) c = shade(s.border_color[side], 1.0f);
        }
        quill::Color col = qc(c);
        quill::IRect r;
        float l = bws[3], t = bws[0], rr = bws[1], b = bws[2];
        switch (side) {
        case 0: r = R(x, y, w, bw); break;
        case 2: r = R(x, y + h - bw, w, bw); break;
        case 3: r = R(x, y + t, bw, h - t - b); break;
        default: r = R(x + w - bw, y + t, bw, h - t - b); break;
        }
        if (bs != BorderStyle::Dashed && bs != BorderStyle::Dotted && bs != BorderStyle::Double) {
            // Mitered corners: each side is a trapezoid, the corner split diagonally with its neighbour. Besides looking
            // right with differently coloured sides, this is what makes the CSS triangle trick (a 0x0 box with one
            // coloured and three transparent borders, used for menu and dropdown arrows) come out as a triangle.
            bool miter = (side == 0 || side == 2) ? (l > 0 || rr > 0) : (t > 0 || b > 0);
            if (miter) {
                int n = static_cast<int>(std::ceil(bw));
                for (int k = 0; k < n && k < 4096; ++k) { // k-th pixel row / column counted from the outer edge
                    float f   = (k + 0.5f) / bw;
                    float thk = std::min(1.f, bw - k);
                    switch (side) {
                    case 0: { float a = x + l * f, e = x + w - rr * f; if (e > a) fb.fill_rect(R(a, y + k, e - a, thk), col); break; }
                    case 2: { float a = x + l * f, e = x + w - rr * f; if (e > a) fb.fill_rect(R(a, y + h - k - thk, e - a, thk), col); break; }
                    case 3: { float a = y + t * f, e = y + h - b * f; if (e > a) fb.fill_rect(R(x + k, a, thk, e - a), col); break; }
                    default: { float a = y + t * f, e = y + h - b * f; if (e > a) fb.fill_rect(R(x + w - k - thk, a, thk, e - a), col); break; }
                    }
                }
                return;
            }
        }
        if (bs == BorderStyle::Dashed || bs == BorderStyle::Dotted) {
            int dash = std::max(1, static_cast<int>(bw)) * (bs == BorderStyle::Dashed ? 3 : 1), gap = std::max(1, static_cast<int>(bw)) * (bs == BorderStyle::Dashed ? 3 : 1);
            if (side == 0 || side == 2) for (int px = r.x; px < r.right(); px += dash + gap) fb.fill_rect(quill::IRect(px, r.y, std::min(dash, r.right() - px), r.h), col);
            else for (int py = r.y; py < r.bottom(); py += dash + gap) fb.fill_rect(quill::IRect(r.x, py, r.w, std::min(dash, r.bottom() - py)), col);
            return;
        }
        if (bs == BorderStyle::Double && bw >= 3) {
            int third = std::max(1, static_cast<int>(bw / 3));
            if (side == 0 || side == 2) {
                fb.fill_rect(quill::IRect(r.x, r.y, r.w, third), col);
                fb.fill_rect(quill::IRect(r.x, r.bottom() - third, r.w, third), col);
            } else {
                fb.fill_rect(quill::IRect(r.x, r.y, third, r.h), col);
                fb.fill_rect(quill::IRect(r.right() - third, r.y, third, r.h), col);
            }
            return;
        }
        fb.fill_rect(r, col);
    }

    void paint_background_and_border(const Box& b, float x, float y, float w, float h) {
        const Style& s = *b.style;
        const float bws[4] = {b.border.top, b.border.right, b.border.bottom, b.border.left};
        if (s.visibility != Visibility::Visible) return;
        quill::IRect bbox = R(x, y, w, h);
        bool rounded = has_radius(s);
        RR outer = make_rr(s, bbox);
        if (!s.shadows.empty()) paint_shadows(b, bbox, false);
        if (&b == canvas_owner) {
            for (int side = 0; side < 4; ++side) draw_border_side(side, s, bws, x, y, w, h);
            return;
        }
        if (!s.mask.url.empty()) { if ((s.background_color >> 24) != 0) paint_masked(s, bbox, qc(s.background_color)); }
        else if ((s.background_color >> 24) != 0) fill_rr(outer, qc(s.background_color));
        Snap bgsnap;
        bool any_image = !s.background_image.empty() || !s.gradients.empty();
        if (rounded && any_image && outer.round() && !bbox.empty() && static_cast<long>(bbox.w) * bbox.h < 8000000) {
            bgsnap.on = true; bgsnap.rr = outer; bgsnap.r = bbox.intersect(quill::IRect(0, 0, fb.w, fb.h));
            if (bgsnap.r.empty()) bgsnap.on = false;
            else {
                bgsnap.px.resize(static_cast<size_t>(bgsnap.r.w) * bgsnap.r.h);
                for (int yy = 0; yy < bgsnap.r.h; ++yy)
                    std::copy_n(&fb.px[static_cast<size_t>(bgsnap.r.y + yy) * fb.w + bgsnap.r.x], bgsnap.r.w, &bgsnap.px[static_cast<size_t>(yy) * bgsnap.r.w]);
            }
        }
        if (!s.gradients.empty() && !s.gradient_over_image) paint_gradients(s, x, y, w, h, bws);
        if (!s.background_image.empty() && env.image) {
            if (const quill::Image* img = frame_if_visible(env.image(s.background_image), bbox)) {
                if (img->w > 0 && img->h > 0) {
                    float bx = x + bws[3], by = y + bws[0];
                    float bw = w - bws[3] - bws[1], bh = h - bws[0] - bws[2];
                    float tw, th;
                    resolve_size(s.bg_size.kind, s.bg_size.w, s.bg_size.h, img->w * env.zoom, img->h * env.zoom, bw, bh, tw, th);
                    int itw = std::max(1, static_cast<int>(std::lround(tw))), ith = std::max(1, static_cast<int>(std::lround(th)));
                    const quill::Image* draw = img;
                    if ((itw != img->w || ith != img->h) && itw <= 8192 && ith <= 8192) draw = &img->scaled(itw, ith);
                    float ox = bx + (s.background_pos_x.is_pct() ? (bw - draw->w) * s.background_pos_x.v / 100.f : s.background_pos_x.v);
                    float oy = by + (s.background_pos_y.is_pct() ? (bh - draw->h) * s.background_pos_y.v / 100.f : s.background_pos_y.v);
                    bool rx = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatX;
                    bool ry = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatY;
                    fb.blit_tiled(*draw, X(ox), Y(oy), rx, ry, R(bx, by, bw, bh));
                }
            }
        }
        if (!s.gradients.empty() && s.gradient_over_image) paint_gradients(s, x, y, w, h, bws);
        if (bgsnap.on) restore_outside(bgsnap);
        if (!s.shadows.empty()) {
            bool any = false;
            for (auto& sh : s.shadows) any |= sh.inset;
            if (any) paint_shadows(b, bbox, true);
        }
        if (outer.round()) {
            RR inner = inset_rr(outer, bws[0], bws[1], bws[2], bws[3]);
            quill::Color cols[4]; bool draw[4];
            for (int i = 0; i < 4; ++i) {
                cols[i] = qc(s.border_color[i]);
                draw[i] = bws[i] > 0 && s.border_style[i] != BorderStyle::None && s.border_style[i] != BorderStyle::Hidden;
            }
            ring_rr(outer, inner, bws, cols, draw);
            return;
        }
        for (int side = 0; side < 4; ++side) draw_border_side(side, s, bws, x, y, w, h);
    }


    // Gradient layers (last listed at the bottom), positioned in the padding box like background images and tiled by
    // background-size / -repeat / -position. Rendered tiles are cached on the style.
    void paint_gradients(const Style& s, float x, float y, float w, float h, const float bws[4]) {
        float bx = x + bws[3], by = y + bws[0];
        float bw = w - bws[3] - bws[1], bh = h - bws[0] - bws[2];
        if (bw < 1 || bh < 1) return;
        quill::IRect area = R(x, y, w, h); // painted over the border box (background-clip: border-box)
        if (area.intersect(fb.clip()).empty()) return;
        float tw = bw, th = bh;
        if (s.bg_size.kind == 3) {
            if (!s.bg_size.w.is_auto()) tw = s.bg_size.w.resolve(bw);
            if (!s.bg_size.h.is_auto()) th = s.bg_size.h.resolve(bh);
        }
        int itw = std::clamp(static_cast<int>(std::lround(tw)), 1, 4096), ith = std::clamp(static_cast<int>(std::lround(th)), 1, 4096);
        if (s.grad_cw != itw || s.grad_ch != ith || s.grad_cache.size() != s.gradients.size()) {
            s.grad_cache.assign(s.gradients.size(), nullptr);
            s.grad_cw = itw;
            s.grad_ch = ith;
        }
        float ox = bx + (s.background_pos_x.is_pct() ? (bw - itw) * s.background_pos_x.v / 100.f : s.background_pos_x.v);
        float oy = by + (s.background_pos_y.is_pct() ? (bh - ith) * s.background_pos_y.v / 100.f : s.background_pos_y.v);
        bool  rx = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatX;
        bool  ry = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatY;
        // tile from the padding box, but over the border box (so a repeating tile reaches under the borders)
        if (rx) ox -= std::ceil((ox - x) / itw) * itw;
        if (ry) oy -= std::ceil((oy - y) / ith) * ith;
        for (size_t i = s.gradients.size(); i-- > 0;) {
            if (!s.grad_cache[i]) s.grad_cache[i] = render_gradient(s.gradients[i], itw, ith);
            fb.blit_tiled(*s.grad_cache[i], X(ox), Y(oy), rx, ry, area);
        }
    }

    void paint_gradients_canvas(const Style& s, float x, float y, float w, float h, const quill::IRect& area, const float bws[4]) {
        // like paint_gradients, but the tiles repeat over the whole canvas (below and beside a short root element too)
        float tw = w, th = h;
        if (s.bg_size.kind == 3) {
            if (!s.bg_size.w.is_auto()) tw = s.bg_size.w.resolve(w);
            if (!s.bg_size.h.is_auto()) th = s.bg_size.h.resolve(h);
        }
        (void)bws;
        int itw = std::clamp(static_cast<int>(std::lround(tw)), 1, 4096), ith = std::clamp(static_cast<int>(std::lround(th)), 1, 4096);
        if (s.grad_cw != itw || s.grad_ch != ith || s.grad_cache.size() != s.gradients.size()) {
            s.grad_cache.assign(s.gradients.size(), nullptr);
            s.grad_cw = itw;
            s.grad_ch = ith;
        }
        float ox = x + (s.background_pos_x.is_pct() ? (w - itw) * s.background_pos_x.v / 100.f : s.background_pos_x.v);
        float oy = y + (s.background_pos_y.is_pct() ? (h - ith) * s.background_pos_y.v / 100.f : s.background_pos_y.v);
        bool  rx = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatX;
        bool  ry = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatY;
        for (size_t i = s.gradients.size(); i-- > 0;) {
            if (!s.grad_cache[i]) s.grad_cache[i] = render_gradient(s.gradients[i], itw, ith);
            fb.blit_tiled(*s.grad_cache[i], X(ox), Y(oy), rx, ry, area);
        }
    }

    // The propagated root / body background image, tiled from the document origin over `area` (framebuffer pixels).
    void paint_canvas_image(const Box& owner, const Box& root, const quill::IRect& area) {
        const Style& s = *owner.style;
        if (!s.gradients.empty()) { // gradients on the canvas: the root element's box is the positioning area
            const float none[4] = {0, 0, 0, 0};
            fb.push_clip(area);
            float ch = std::max(root.h, 1.f);
            // paint_gradients clips to the box it is given: give it the whole document height, from the root's top
            paint_gradients_canvas(s, root.x, root.y, std::max(root.w, 1.f), ch, area, none);
            fb.pop_clip();
        }
        if (s.background_image.empty() || !env.image) return;
        const quill::Image* img = frame(env.image(s.background_image));
        if (!img || img->w <= 0 || img->h <= 0) return;
        float cw = std::max(root.w, static_cast<float>(area.w)), ch = std::max(root.h, static_cast<float>(area.h));
        float tw, th;
        resolve_size(s.bg_size.kind, s.bg_size.w, s.bg_size.h, img->w * env.zoom, img->h * env.zoom, cw, ch, tw, th);
        int itw = std::max(1, static_cast<int>(std::lround(tw))), ith = std::max(1, static_cast<int>(std::lround(th)));
        const quill::Image* draw = img;
        if ((itw != img->w || ith != img->h) && itw <= 8192 && ith <= 8192) draw = &img->scaled(itw, ith);
        float ox = s.background_pos_x.is_pct() ? (cw - draw->w) * s.background_pos_x.v / 100.f : s.background_pos_x.v;
        float oy = s.background_pos_y.is_pct() ? (ch - draw->h) * s.background_pos_y.v / 100.f : s.background_pos_y.v;
        bool rx = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatX;
        bool ry = s.background_repeat == BgRepeat::Repeat || s.background_repeat == BgRepeat::RepeatY;
        fb.blit_tiled(*draw, X(ox), Y(oy), rx, ry, area);
    }

    // background-size / mask-size for an image of natural size iw x ih in an area W x H.
    static void resolve_size(uint8_t kind, const Length& lw, const Length& lh, float iw, float ih, float W, float H, float& tw, float& th) {
        tw = iw; th = ih;
        if (kind == 1) { float k = std::min(W / iw, H / ih); tw = iw * k; th = ih * k; }
        else if (kind == 2) { float k = std::max(W / iw, H / ih); tw = iw * k; th = ih * k; }
        else if (kind == 3) {
            bool aw = lw.is_auto(), ah = lh.is_auto();
            if (!aw) tw = lw.resolve(W);
            if (!ah) th = lh.resolve(H);
            if (aw && !ah) tw = th * iw / ih;
            else if (ah && !aw) th = tw * ih / iw;
        }
    }

    // ---- mask-image: the element's background colour drawn through the mask's alpha
    bool paint_masked(const Style& s, const quill::IRect& bb, quill::Color col) {
        const quill::Image* img = env.image ? env.image(s.mask.url) : nullptr;
        if (!img || img->w <= 0 || img->h <= 0 || bb.empty()) return true; // an unavailable mask hides the element
        float W = static_cast<float>(bb.w), H = static_cast<float>(bb.h);
        float iw = img->w * env.zoom, ih = img->h * env.zoom;
        float tw, th;
        const Mask& m = s.mask;
        resolve_size(m.size_kind, m.w, m.h, iw, ih, W, H, tw, th);
        int itw = std::max(1, static_cast<int>(std::lround(tw))), ith = std::max(1, static_cast<int>(std::lround(th)));
        if (itw > 4096 || ith > 4096) return true;
        const quill::Image& sc = img->scaled(itw, ith);
        float ox = m.pos_x.is_pct() ? (W - tw) * m.pos_x.v / 100.f : m.pos_x.v;
        float oy = m.pos_y.is_pct() ? (H - th) * m.pos_y.v / 100.f : m.pos_y.v;
        int px0 = bb.x + static_cast<int>(std::lround(ox)), py0 = bb.y + static_cast<int>(std::lround(oy));
        bool rx = m.repeat == BgRepeat::Repeat || m.repeat == BgRepeat::RepeatX, ry = m.repeat == BgRepeat::Repeat || m.repeat == BgRepeat::RepeatY;
        quill::IRect v = bb.intersect(fb.clip());
        for (int y = v.y; y < v.bottom(); ++y) {
            int ty = y - py0;
            if (ry) { ty %= sc.h; if (ty < 0) ty += sc.h; } else if (ty < 0 || ty >= sc.h) continue;
            for (int x = v.x; x < v.right(); ++x) {
                int tx = x - px0;
                if (rx) { tx %= sc.w; if (tx < 0) tx += sc.w; } else if (tx < 0 || tx >= sc.w) continue;
                int a = (sc.px[static_cast<size_t>(ty) * sc.w + tx] >> 24) & 0xff;
                if (a) fb.blend_px(x, y, col, a);
            }
        }
        return true;
    }

    // Inline <svg>: the serialised markup, currentColor resolved, decoded once and re-rendered per size.
    const quill::Image* svg_image(const Box& b) {
        static std::unordered_map<std::string, std::shared_ptr<quill::Image>> cache;
        std::string markup = b.text;
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%06x", b.style->color & 0xffffff);
        for (const char* cc : {"currentColor", "currentcolor"}) {
            size_t pos = 0;
            while ((pos = markup.find(cc, pos)) != std::string::npos) { markup.replace(pos, std::strlen(cc), hex); pos += 7; }
        }
        auto it = cache.find(markup);
        if (it == cache.end()) {
            if (cache.size() > 512) cache.clear();
            auto img = quill::decode_image(reinterpret_cast<const unsigned char*>(markup.data()), markup.size());
            it = cache.emplace(markup, img).first;
        }
        return it->second.get();
    }

    // ---- text
    uint8_t effective_decoration(const Box* b) const {
        uint8_t bits = 0;
        for (const Box* p = b; p; p = p->parent) {
            if (!p->style) continue;
            bits |= p->style->text_decoration;
            const Style& s = *p->style;
            if (s.display == Display::InlineBlock || s.display == Display::InlineTable || s.float_ != Float::None ||
                s.position == Position::Absolute || s.position == Position::Fixed)
                break;
        }
        return bits;
    }

    void draw_text(const Style& s, const std::string& text, float x, float baseline, float width, uint8_t deco, Color color) {
        if (text.empty()) return;
        if ((deco & 8) && env.anim_ms >= 0) { // text-decoration: blink — on for 500 ms, off for 500 ms
            int next = 500 - static_cast<int>(env.anim_ms % 500);
            if (env.anim_next_ms && (*env.anim_next_ms < 0 || next < *env.anim_next_ms)) *env.anim_next_ms = next;
            if ((env.anim_ms / 500) % 2 == 1) return;
        }
        quill::Font* f = font_for(s);
        if (!f) return;
        if (s.letter_spacing != 0) {
            float cx = x;
            for (size_t i = 0; i < text.size();) {
                size_t j = i;
                quill::utf8_next(text, j);
                std::string ch = text.substr(i, j - i);
                f->draw(fb, ch, X(cx), Y(baseline), qc(color));
                cx += f->text_width(ch) + s.letter_spacing;
                i = j;
            }
        } else {
            f->draw(fb, text, X(x), Y(baseline), qc(color));
        }
        int th = std::max(1, static_cast<int>(std::lround(s.font_size / 14.f)));
        if (deco & 1) fb.fill_rect(quill::IRect(X(x), Y(baseline) + std::max(1, f->descent / 3), X(x + width) - X(x), th), qc(color));
        if (deco & 2) fb.fill_rect(quill::IRect(X(x), Y(baseline) - f->ascent, X(x + width) - X(x), th), qc(color));
        if (deco & 4) fb.fill_rect(quill::IRect(X(x), Y(baseline) - f->x_height / 2 - th / 2, X(x + width) - X(x), th), qc(color));
    }

    // Selected text: a highlight over the selected part of the line, the text drawn again on it in white.
    void paint_selected(const Line& L, const Frag& f, const Style& ts, size_t b0, size_t b1) {
        quill::Font* fn = font_for(ts);
        if (!fn || b1 <= b0) return;
        b1           = std::min(b1, f.text.size());
        float full   = fn->text_width(f.text);
        float k      = full > 0 ? f.w / full : 1.f; // justified text is spread wider than measured
        float x0     = f.x + (b0 ? fn->text_width(f.text.substr(0, b0)) * k : 0.f);
        float x1     = b1 >= f.text.size() ? f.x + f.w : f.x + fn->text_width(f.text.substr(0, b1)) * k;
        quill::IRect r(X(x0), Y(L.y), std::max(1, X(x1) - X(x0)), std::max(1, Y(L.y + L.h) - Y(L.y)));
        fb.fill_rect(r, quill::Color(51, 142, 255));
        fb.push_clip(r);
        draw_text(ts, f.text, f.x, f.y, f.w, effective_decoration(f.box), 0xffffffffu);
        fb.pop_clip();
    }

    // ---- replaced content
    // A broken <img>: its alt text in the element's own font and colour (so a broken image inside a link reads like
    // the link), clipped to the box; a thin frame when the page gave it a size, a small marker when there is no alt.
    void paint_broken_image(const Box& b, float cx, float cy, float cw, float ch) {
        const Style&       s   = *b.style;
        const std::string* alt = b.attr("alt");
        quill::IRect       box = R(cx, cy, cw, ch);
        if (box.empty()) return;
        if (!alt) {
            quill::Color g(0x99, 0x99, 0x99);
            fb.fill_rect(quill::IRect(box.x, box.y, box.w, 1), g);
            fb.fill_rect(quill::IRect(box.x, box.bottom() - 1, box.w, 1), g);
            fb.fill_rect(quill::IRect(box.x, box.y, 1, box.h), g);
            fb.fill_rect(quill::IRect(box.right() - 1, box.y, 1, box.h), g);
            if (box.w >= 8 && box.h >= 8) fb.fill_rect(quill::IRect(box.x + 3, box.y + 3, box.w / 3, box.h / 3), quill::Color(0xcc, 0x33, 0x33));
            return;
        }
        if (alt->empty()) return;
        quill::Font* f = font_for(s);
        if (!f) return;
        bool sized = s.width.is_px() || s.width.is_pct() || s.height.is_px() || b.attr("width") || b.attr("height");
        if (sized && box.w >= 4 && box.h >= 4) {
            quill::Color g(0xa0, 0xa0, 0xa0);
            fb.fill_rect(quill::IRect(box.x, box.y, box.w, 1), g);
            fb.fill_rect(quill::IRect(box.x, box.bottom() - 1, box.w, 1), g);
            fb.fill_rect(quill::IRect(box.x, box.y, 1, box.h), g);
            fb.fill_rect(quill::IRect(box.right() - 1, box.y, 1, box.h), g);
        }
        if (sized) {
            fb.push_clip(box);
            f->draw(fb, *alt, X(cx + 2), Y(cy + 2 + f->ascent), qc(s.color));
            fb.pop_clip();
        } else {
            // sized like a word: the box's bottom edge sits on the line's baseline, so the text does too
            f->draw(fb, *alt, X(cx + 2), Y(cy + ch), qc(s.color));
        }
    }

    void paint_replaced(const Box& b) {
        const Style& s = *b.style;
        if (s.visibility != Visibility::Visible) return;
        float cx = b.x + b.border.left + b.padding.left, cy = b.y + b.border.top + b.padding.top;
        float cw = b.w - b.border.left - b.border.right - b.padding.left - b.padding.right;
        float ch = b.h - b.border.top - b.border.bottom - b.padding.top - b.padding.bottom;
        auto attr = [&](const char* n) { const std::string* v = b.attr(n); return v ? *v : std::string(); };
        switch (b.replaced) {
        case ReplacedKind::Image: {
            const std::string* src = b.attr("src");
            const quill::Image* img = src && env.image ? frame_if_visible(env.image(*src), R(cx, cy, cw, ch)) : nullptr;
            if (img && img->w > 0 && cw >= 1 && ch >= 1) {
                const quill::Image& sc = img->scaled(std::max(1, static_cast<int>(std::lround(cw))), std::max(1, static_cast<int>(std::lround(ch))));
                Snap isn;
                if (has_radius(s)) {
                    RR rr = make_rr(s, R(b.x, b.y, b.w, b.h));
                    isn.rr = inset_rr(rr, b.border.top, b.border.right, b.border.bottom, b.border.left);
                    isn.r = isn.rr.r.intersect(quill::IRect(0, 0, fb.w, fb.h));
                    if (isn.rr.round() && !isn.r.empty()) {
                        isn.on = true;
                        isn.px.resize(static_cast<size_t>(isn.r.w) * isn.r.h);
                        for (int yy = 0; yy < isn.r.h; ++yy)
                            std::copy_n(&fb.px[static_cast<size_t>(isn.r.y + yy) * fb.w + isn.r.x], isn.r.w, &isn.px[static_cast<size_t>(yy) * isn.r.w]);
                    }
                }
                fb.blit_tiled(sc, X(cx), Y(cy), false, false, R(cx, cy, cw, ch));
                if (isn.on) restore_outside(isn);
            } else if (env.image_broken && (!src || src->empty() || env.image_broken(*src))) {
                paint_broken_image(b, cx, cy, cw, ch);
            } else if (cw >= 2 && ch >= 2) {
                fb.fill_rect(R(cx, cy, cw, ch), quill::Color(0xee, 0xee, 0xee));
                fb.fill_rect(R(cx, cy, cw, 1), quill::Color(0xbb, 0xbb, 0xbb));
                fb.fill_rect(R(cx, cy + ch - 1, cw, 1), quill::Color(0xbb, 0xbb, 0xbb));
                fb.fill_rect(R(cx, cy, 1, ch), quill::Color(0xbb, 0xbb, 0xbb));
                fb.fill_rect(R(cx + cw - 1, cy, 1, ch), quill::Color(0xbb, 0xbb, 0xbb));
            }
            break;
        }
        case ReplacedKind::Svg: {
            if (const quill::Image* img = svg_image(b)) if (cw >= 1 && ch >= 1) {
                const quill::Image& sc = img->scaled(std::max(1, static_cast<int>(std::lround(cw))), std::max(1, static_cast<int>(std::lround(ch))));
                fb.blit_tiled(sc, X(cx), Y(cy), false, false, R(cx, cy, cw, ch));
            }
            break;
        }
        case ReplacedKind::Input: {
            std::string type = attr("type");
            for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (type != "checkbox" && type != "radio" && env.control &&
                env.control(b, fb, static_cast<int>(std::lround(-sx)), static_cast<int>(std::lround(-sy)))) break;
            if (type == "checkbox" || type == "radio") {
                bool radio = type == "radio", checked = b.attr("checked") != nullptr;
                if (env.checked) { int v = env.checked(b); if (v >= 0) checked = v != 0; }
                quill::IRect r = R(cx, cy, 13, 13);
                if (radio) fb.fill_round_rect(r, 6, 6, 6, 6, quill::Color(0x76, 0x76, 0x76));
                else fb.fill_rect(r, quill::Color(0x76, 0x76, 0x76));
                quill::IRect in(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
                if (radio) fb.fill_round_rect(in, 5, 5, 5, 5, quill::Color(255, 255, 255));
                else fb.fill_rect(in, quill::Color(255, 255, 255));
                if (checked) {
                    if (radio) fb.fill_round_rect(quill::IRect(r.x + 4, r.y + 4, 5, 5), 2, 2, 2, 2, quill::Color(0x1a, 0x73, 0xe8));
                    else fb.fill_rect(quill::IRect(r.x + 3, r.y + 3, 7, 7), quill::Color(0x1a, 0x73, 0xe8));
                }
                break;
            }
            if (type == "hidden") break;
            std::string value = attr("value"), shown = value;
            bool placeholder = false;
            if (type == "submit" || type == "button" || type == "reset") {
                // value="" is an (empty) label too; only a missing value attribute gets the default text
                if (!b.attr("value")) shown = type == "submit" ? "Submit" : type == "reset" ? "Reset" : "";
            } else if (type == "password") {
                shown = std::string(utf8_count(value), '*');
            }
            if (type != "submit" && type != "button" && type != "reset" && shown.empty() && !attr("placeholder").empty()) { shown = attr("placeholder"); placeholder = true; }
            quill::Font* f = font_for(s);
            if (!f || shown.empty()) break;
            float tw = f->text_width(shown);
            float tx = (type == "submit" || type == "button" || type == "reset") ? cx + (cw - tw) / 2 : cx;
            float base = cy + (ch - (f->ascent + f->descent)) / 2 + f->ascent;
            fb.push_clip(R(cx, cy, cw, ch));
            f->draw(fb, shown, X(tx), Y(base), placeholder ? quill::Color(0x75, 0x75, 0x75) : qc(s.color));
            fb.pop_clip();
            break;
        }
        case ReplacedKind::Textarea: case ReplacedKind::Select: {
            if (env.control && env.control(b, fb, static_cast<int>(std::lround(-sx)), static_cast<int>(std::lround(-sy)))) break;
            quill::Font* f = font_for(s);
            if (!f) break;
            fb.push_clip(R(cx, cy, cw, ch));
            if (b.replaced == ReplacedKind::Select) {
                f->draw(fb, b.text, X(cx), Y(cy + (ch - (f->ascent + f->descent)) / 2 + f->ascent), qc(s.color));
                if (!s.appearance_none) { // appearance:none: the page draws its own arrow
                    int ax = X(cx + cw - 12), ay = Y(cy + ch / 2) - 2;
                    for (int i = 0; i < 5; ++i) fb.fill_rect(quill::IRect(ax + i, ay + i / 1, 9 - 2 * i, 1), qc(s.color));
                }
            } else {
                int line = 0;
                size_t i = 0;
                const std::string& t = b.text;
                while (i <= t.size()) {
                    size_t j = t.find('\n', i);
                    if (j == std::string::npos) j = t.size();
                    f->draw(fb, t.substr(i, j - i), X(cx), Y(cy + f->ascent + line * static_cast<float>(f->line_height)), qc(s.color));
                    ++line;
                    i = j + 1;
                }
            }
            fb.pop_clip();
            break;
        }
        case ReplacedKind::Iframe: {
            quill::IRect r = R(cx, cy, cw, ch);
            if (r.empty()) break;
            if (env.frame) env.frame(b, fb, r);
            break;
        }
        case ReplacedKind::Meter: case ReplacedKind::Progress:
            fb.fill_rect(R(cx, cy, cw, ch), quill::Color(0xe6, 0xe6, 0xe6));
            break;
        default: break; // svg / canvas / iframe / video / object: nothing to draw yet
        }
    }

    static size_t utf8_count(const std::string& s) {
        size_t n = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    // ---- boxes: CSS 2.1 appendix E in miniature. A stacking context paints its own background, negative
    // z-index contexts, then in-flow block backgrounds, floats and inline content, then positioned
    // descendants in z-order. Positioned boxes are collected by collect() and skipped by the flow walkers.
    static bool is_real(const Box& b) { return b.style && b.kind != Box::Kind::Text && b.kind != Box::Kind::LineBreak && b.kind != Box::Kind::Marker; }
    static bool is_positioned(const Box& b) {
        if (!is_real(b)) return false;
        bool inline_plain = b.style->display == Display::Inline && b.replaced == ReplacedKind::None;
        if (inline_plain) return false;
        return b.style->position != Position::Static || b.style->opacity < 1.f || b.style->has_xform || b.style->marquee || b.scrollable(); // opacity < 1 / transform / marquee / scrolled content make a layer
    }
    static bool forms_context(const Box& b) {
        const Style& s = *b.style;
        return s.position == Position::Fixed || s.position == Position::Sticky || !s.z_auto || s.opacity < 1.f || s.has_xform || s.marquee || b.scrollable();
    }
    static bool is_floating(const Box& b) { return is_real(b) && b.style->float_ != Float::None && !is_positioned(b); }
    static bool clips(const Box& b) { return b.style->overflow_x != Overflow::Visible || b.style->overflow_y != Overflow::Visible; }
    static bool inline_container(const Box& b) { return !b.lines.empty() || !b.inline_floats.empty(); }
    quill::IRect clip_rect(const Box& b) const { return R(b.x + b.border.left, b.y + b.border.top, b.w - b.border.left - b.border.right, b.h - b.border.top - b.border.bottom); }

    struct Entry { const Box* box; int z; size_t seq; };
    void collect(const Box& b, std::vector<Entry>& out, size_t& seq) const {
        for (auto& cp : b.children) {
            const Box& c = *cp;
            if (!is_real(c)) continue;
            if (is_positioned(c)) {
                bool forms = forms_context(c);
                out.push_back({&c, forms ? c.style->z_index : 0, seq++});
                if (!forms) collect(c, out, seq);
                continue;
            }
            collect(c, out, seq);
        }
    }

    // Does c take part in the flow walkers of its parent?
    static bool flow_child(const Box& c) { return is_real(c) && !is_positioned(c) && !is_floating(c); }

    void walk_bg(const Box& b, bool self) {
        if (b.skipped || !is_real(b) || culled(b)) return;
        const Style& s = *b.style;
        bool inline_box = b.replaced == ReplacedKind::None && s.display == Display::Inline && !is_positioned(b) && !is_floating(b);
        if (self && !inline_box) paint_background_and_border(b, b.x, b.y, b.w, b.h);
        if (b.replaced != ReplacedKind::None || inline_container(b)) return;
        bool clip = clips(b) && &b != noclip;
        Snap snap;
        if (clip) snap = begin_clip(b);
        for (auto& c : b.children)
            if (flow_child(*c) && !c->skipped) walk_bg(*c, true);
        if (clip) end_clip(snap);
    }

    void walk_float(const Box& b) {
        if (b.skipped || !is_real(b) || b.replaced != ReplacedKind::None || culled(b)) return;
        bool clip = clips(b) && &b != noclip;
        Snap snap;
        if (clip) snap = begin_clip(b);
        if (inline_container(b)) {
            for (const Box* f : b.inline_floats) if (!f->skipped) paint_flow(*f);
        } else {
            for (auto& c : b.children) {
                if (!is_real(*c) || c->skipped || is_positioned(*c)) continue;
                if (is_floating(*c)) paint_flow(*c);
                else walk_float(*c);
            }
        }
        if (clip) end_clip(snap);
    }

    void walk_content(const Box& b) {
        if (b.skipped || !is_real(b) || culled(b)) return;
        const Style& s = *b.style;
        if (b.replaced != ReplacedKind::None) { paint_replaced(b); return; }
        bool clip = clips(b) && &b != noclip;
        Snap snap;
        if (clip) snap = begin_clip(b);
        if (b.has_marker && s.visibility == Visibility::Visible) {
            for (auto& c : b.children)
                if (c->kind == Box::Kind::Marker) {
                    draw_text(*c->style, c->text, b.marker_x, b.marker_y, b.marker_w, 0, c->style->color);
                    break;
                }
        }
        if (inline_container(b)) {
            for (auto& L : b.lines) {
                if (env.cull && b.ink_valid && off_screen(L.ink_y0, L.ink_y1)) continue;
                for (auto& sp : L.spans) paint_span(sp);
                for (auto& f : L.frags) {
                    if (f.kind == Frag::Kind::Atomic) {
                        if (!f.box->skipped && !is_positioned(*f.box)) paint_flow(*f.box);
                        continue;
                    }
                    const Style& ts = *f.box->style;
                    if (ts.visibility != Visibility::Visible) continue;
                    draw_text(ts, f.text, f.x, f.y, f.w, effective_decoration(f.box), ts.color);
                    if (env.selection && !env.selection->empty()) {
                        auto it = env.selection->find(&f);
                        if (it != env.selection->end()) paint_selected(L, f, ts, it->second.first, it->second.second);
                    }
                }
            }
        } else {
            for (auto& c : b.children)
                if (flow_child(*c) && !c->skipped) walk_content(*c);
        }
        if (clip) end_clip(snap);
    }

    // A box painted as if it were its own stacking context, minus its positioned descendants.
    void paint_flow(const Box& b) {
        walk_bg(b, true);
        walk_float(b);
        walk_content(b);
    }

    void paint_context(const Box& root) {
        std::vector<Entry> entries;
        size_t seq = 0;
        collect(root, entries, seq);
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.z != b.z ? a.z < b.z : a.seq < b.seq; });
        if (&root != noclip) walk_bg_self(root);
        for (auto& e : entries) if (e.z < 0) paint_entry(*e.box);
        if (!root.skipped && is_real(root) && root.replaced == ReplacedKind::None && !inline_container(root)) {
            bool clip = clips(root) && &root != noclip;
            Snap snap;
            if (clip) snap = begin_clip(root);
            for (auto& c : root.children) if (flow_child(*c) && !c->skipped) walk_bg(*c, true);
            if (clip) end_clip(snap);
        }
        walk_float(root);
        walk_content(root);
        for (auto& e : entries) if (e.z >= 0) paint_entry(*e.box);
    }
    void walk_bg_self(const Box& b) {
        if (b.skipped || !is_real(b)) return;
        const Style& s = *b.style;
        bool inline_box = b.replaced == ReplacedKind::None && s.display == Display::Inline && !is_positioned(b);
        if (!inline_box) paint_background_and_border(b, b.x, b.y, b.w, b.h);
    }
    // The visible viewport (document y of its top, height): sticky boxes stick to it. Unknown (0 height): they don't.
    float view_y = 0, view_h = 0;

    void paint_entry(const Box& b) {
        if (b.skipped) return;
        Position pos = b.style->position;
        if ((pos == Position::Fixed || pos == Position::Sticky) && env.skip_fixed) return; // painted per frame on top
        if (pos == Position::Sticky && view_h > 0) {
            float dy = sticky_offset(b, view_y, view_h);
            if (dy != 0) {
                Painter q{fb, sx, sy - dy, env};
                q.fx = fx; q.fy = fy; q.canvas_owner = canvas_owner; q.view_y = view_y; q.view_h = 0; // nested sticky: not again
                q.paint_entry_inner(b);
                return;
            }
        }
        if (pos != Position::Fixed && culled(b)) return; // fixed boxes are culled by their own painter
        paint_entry_inner(b);
    }
    void paint_entry_inner(const Box& b) {
        float op = b.style->opacity;
        if (op <= 0.f) return;
        if (b.style->has_xform) { paint_transformed(b, op); return; }
        if (b.style->marquee && paint_marquee(b)) return;
        if (b.scrollable()) { paint_scroller(b); return; }
        if (op < 1.f) { paint_layer(b, op); return; }
        paint_entry_opaque(b);
    }

    // ---- a scroll container: background and border in place, content moved by its scroll position, clipped, then a
    // thin scroll bar along each axis that scrolls
    void paint_scroller(const Box& b) {
        float ox = 0, oy = 0;
        box_scroll_of(b, env.box_scroll, ox, oy);
        walk_bg_self(b);
        Snap snap = begin_clip(b);
        Painter q{fb, sx + ox, sy + oy, env};
        q.fx = fx; q.fy = fy; q.canvas_owner = canvas_owner; q.view_y = view_y; q.view_h = view_h;
        q.noclip = &b;
        q.paint_context(b);
        end_clip(snap);
        if (b.style->visibility != Visibility::Visible) return;
        quill::IRect cr = clip_rect(b);
        quill::Color track(0, 0, 0, 18), thumb(0, 0, 0, 110);
        const int    t = 6;
        if (b.scroll_max_y > 0 && cr.h > 3 * t) {
            float vis = static_cast<float>(cr.h), all = vis + b.scroll_max_y;
            int   th  = std::max(2 * t, static_cast<int>(vis * vis / all));
            int   ty  = cr.y + static_cast<int>((vis - th) * (oy / b.scroll_max_y));
            fb.fill_rect(quill::IRect(cr.right() - t - 1, cr.y, t, cr.h), track);
            fb.fill_round_rect(quill::IRect(cr.right() - t - 1, ty, t, th), t / 2, t / 2, t / 2, t / 2, thumb);
        }
        if (b.scroll_max_x > 0 && cr.w > 3 * t) {
            float vis = static_cast<float>(cr.w), all = vis + b.scroll_max_x;
            int   tw  = std::max(2 * t, static_cast<int>(vis * vis / all));
            int   tx  = cr.x + static_cast<int>((vis - tw) * (ox / b.scroll_max_x));
            fb.fill_rect(quill::IRect(cr.x, cr.bottom() - t - 1, cr.w, t), track);
            fb.fill_round_rect(quill::IRect(tx, cr.bottom() - t - 1, tw, t), t / 2, t / 2, t / 2, t / 2, thumb);
        }
    }

    // ---- <marquee>: its background and border where they are, its content shifted inside its padding box
    bool paint_marquee(const Box& b) {
        float dx = 0, dy = 0;
        int   nx = -1;
        bool  on = marquee_offset(b, env.anim_ms, dx, dy, &nx);
        if (nx >= 0 && env.anim_next_ms && (*env.anim_next_ms < 0 || nx < *env.anim_next_ms)) *env.anim_next_ms = nx;
        if (!on) return false;
        walk_bg_self(b);
        fb.push_clip(clip_rect(b));
        Painter q{fb, sx - dx, sy - dy, env};
        q.fx = fx; q.fy = fy; q.canvas_owner = canvas_owner; q.view_y = view_y; q.view_h = view_h;
        q.noclip = &b;
        q.paint_context(b);
        fb.pop_clip();
        return true;
    }

    // ---- transform (rotate / scale / skew / matrix): the subtree is painted untransformed twice, over white and over
    // black (which gives both its colours and its coverage), then resampled through the inverse matrix.
    static void bounds_walk(const Box& b, float& x0, float& y0, float& x1, float& y1, int depth) {
        if (depth > 60 || b.skipped || !b.style) return;
        if (b.kind != Box::Kind::Text) {
            x0 = std::min(x0, b.x); y0 = std::min(y0, b.y);
            x1 = std::max(x1, b.x + b.w); y1 = std::max(y1, b.y + b.h);
            for (auto& sh : b.style->shadows) {
                float m = std::fabs(sh.ox) + std::fabs(sh.oy) + sh.blur + std::max(0.f, sh.spread);
                x0 = std::min(x0, b.x - m); y0 = std::min(y0, b.y - m); x1 = std::max(x1, b.x + b.w + m); y1 = std::max(y1, b.y + b.h + m);
            }
            if (b.style->overflow_x != Overflow::Visible || b.style->overflow_y != Overflow::Visible) return;
        }
        for (auto& c : b.children) if (c->style && c->style->position != Position::Fixed) bounds_walk(*c, x0, y0, x1, y1, depth + 1);
        for (auto& L : b.lines) {
            y0 = std::min(y0, L.y); y1 = std::max(y1, L.y + L.h);
            for (auto& f : L.frags) {
                x0 = std::min(x0, f.x); x1 = std::max(x1, f.x + f.w);
                if (f.kind == Frag::Kind::Atomic && f.box && f.box->parent != &b) bounds_walk(*f.box, x0, y0, x1, y1, depth + 1);
            }
        }
    }
    void paint_transformed(const Box& b, float op) {
        const Style& s = *b.style;
        float a = s.xf[0], bb = s.xf[1], c = s.xf[2], d = s.xf[3];
        float det = a * d - bb * c;
        if (std::fabs(det) < 1e-6f) return; // flattened to nothing
        float x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
        bounds_walk(b, x0, y0, x1, y1, 0);
        x0 = std::floor(x0) - 1; y0 = std::floor(y0) - 1;
        int W = static_cast<int>(std::ceil(x1 - x0)) + 2, H = static_cast<int>(std::ceil(y1 - y0)) + 2;
        float ox = b.x + s.xf_origin_x.resolve(b.w), oy = b.y + s.xf_origin_y.resolve(b.h);
        // where it lands (framebuffer coordinates)
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int k = 0; k < 4; k++) {
            float px = (k & 1) ? x0 + W : x0, py = (k & 2) ? y0 + H : y0;
            float qx = ox + a * (px - ox) + c * (py - oy), qy = oy + bb * (px - ox) + d * (py - oy);
            minx = std::min(minx, qx - sx); maxx = std::max(maxx, qx - sx);
            miny = std::min(miny, qy - sy); maxy = std::max(maxy, qy - sy);
        }
        quill::IRect dst(static_cast<int>(std::floor(minx)), static_cast<int>(std::floor(miny)), static_cast<int>(std::ceil(maxx - minx)) + 1,
                         static_cast<int>(std::ceil(maxy - miny)) + 1);
        dst = dst.intersect(fb.clip()).intersect(quill::IRect(0, 0, fb.w, fb.h));
        if (dst.empty()) return;
        if (W <= 0 || H <= 0 || static_cast<long long>(W) * H > 4000000) { // too big to resample: draw it untransformed
            paint_entry_opaque(b);
            return;
        }
        quill::Framebuffer fw, fk;
        fw.resize(W, H);
        fk.resize(W, H);
        fw.clear(quill::Color(255, 255, 255));
        fk.clear(quill::Color(0, 0, 0));
        for (quill::Framebuffer* o : {&fw, &fk}) {
            Painter p{*o, x0, y0, env};
            p.fx = fx; p.fy = fy; p.canvas_owner = canvas_owner;
            p.paint_entry_opaque(b);
        }
        float ia = d / det, ib = -bb / det, ic = -c / det, id = a / det;
        int   alpha_op = static_cast<int>(std::lround(std::clamp(op, 0.f, 1.f) * 256));
        auto  sample = [&](const quill::Framebuffer& f, float u, float v, float out[3]) {
            int   iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
            float fu = u - iu, fv = v - iv;
            out[0] = out[1] = out[2] = 0;
            for (int k = 0; k < 4; k++) {
                int   xx = iu + (k & 1), yy = iv + (k >> 1);
                float w = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                uint32_t px = (xx >= 0 && yy >= 0 && xx < f.w && yy < f.h) ? f.px[static_cast<size_t>(yy) * f.w + xx]
                                                                             : (&f == &fw ? 0xffffffffu : 0xff000000u);
                out[0] += w * ((px >> 16) & 255); out[1] += w * ((px >> 8) & 255); out[2] += w * (px & 255);
            }
        };
        for (int py = dst.y; py < dst.bottom(); py++)
            for (int px = dst.x; px < dst.right(); px++) {
                float qx = px + 0.5f + sx - ox, qy = py + 0.5f + sy - oy; // document point relative to the origin
                float u = ox + ia * qx + ic * qy - x0 - 0.5f, v = oy + ib * qx + id * qy - y0 - 0.5f;
                if (u < -1 || v < -1 || u > W || v > H) continue;
                float cw[3], ck[3];
                sample(fw, u, v, cw);
                sample(fk, u, v, ck);
                float cov = 255.f - ((cw[0] - ck[0]) + (cw[1] - ck[1]) + (cw[2] - ck[2])) / 3.f; // 0..255
                if (cov < 0.5f) continue;
                int   A  = static_cast<int>(cov * alpha_op / 256.f + 0.5f);
                uint32_t& dpx = fb.px[static_cast<size_t>(py) * fb.w + px];
                uint32_t  res = 0xff000000u;
                for (int ch = 0; ch < 3; ch++) {
                    int sh = 16 - 8 * ch;
                    float pre = ck[ch] * alpha_op / 256.f; // colour over black = premultiplied colour
                    int dc = (dpx >> sh) & 255;
                    int v2 = static_cast<int>(pre + dc * (255 - A) / 255.f + 0.5f);
                    res |= static_cast<uint32_t>(std::clamp(v2, 0, 255)) << sh;
                }
                dpx = res;
            }
    }
    // Group opacity: paint the subtree normally, then blend the result with what was there before.
    void paint_layer(const Box& b, float op) {
        quill::IRect r = fb.clip().intersect(quill::IRect(0, 0, fb.w, fb.h));
        if (r.empty()) return;
        std::vector<uint32_t> saved(static_cast<size_t>(r.w) * r.h);
        for (int yy = 0; yy < r.h; ++yy) std::copy_n(&fb.px[static_cast<size_t>(r.y + yy) * fb.w + r.x], r.w, &saved[static_cast<size_t>(yy) * r.w]);
        paint_entry_opaque(b);
        int a = static_cast<int>(std::lround(std::clamp(op, 0.f, 1.f) * 256.f));
        for (int yy = 0; yy < r.h; ++yy) {
            uint32_t* cur = &fb.px[static_cast<size_t>(r.y + yy) * fb.w + r.x];
            const uint32_t* old = &saved[static_cast<size_t>(yy) * r.w];
            for (int xx = 0; xx < r.w; ++xx) {
                if (cur[xx] == old[xx]) continue;
                uint32_t o = old[xx], n = cur[xx], res = 0xff000000u;
                for (int sh = 0; sh <= 16; sh += 8) {
                    int oc = (o >> sh) & 255, nc = (n >> sh) & 255;
                    res |= static_cast<uint32_t>((oc * (256 - a) + nc * a) >> 8) << sh;
                }
                cur[xx] = res;
            }
        }
    }
    void paint_entry_opaque(const Box& b) {
        bool forms = forms_context(b);
        if (b.style->position == Position::Fixed) {
            Painter fixed{fb, fx, fy, env};
            fixed.fx = fx; fixed.fy = fy;
            fixed.paint_context(b);
        } else if (forms) paint_context(b);
        else paint_flow(b);
    }

    void paint_span(const Span& sp) {
        const Style& s = *sp.box->style;
        if (s.visibility != Visibility::Visible) return;
        float top = sp.baseline - sp.ascent - s.padding[0].resolve(0) - s.border_width[0];
        // spans carry line-relative baseline: convert using the owning line's y (stored in baseline as absolute below)
        (void)top;
        float x = sp.x, w = sp.w;
        float y0 = sp.baseline - sp.ascent - (s.padding[0].is_px() ? s.padding[0].v : 0) - s.border_width[0];
        float h  = sp.ascent + sp.descent + (s.padding[0].is_px() ? s.padding[0].v : 0) + (s.padding[2].is_px() ? s.padding[2].v : 0) + s.border_width[0] + s.border_width[2];
        if ((s.background_color >> 24) != 0) fb.fill_rect(R(x, y0, w, h), qc(s.background_color));
        Style tmp = s;
        if (!sp.first) tmp.border_width[3] = 0;
        if (!sp.last) tmp.border_width[1] = 0;
        for (int side = 0; side < 4; ++side) draw_border_side(side, tmp, tmp.border_width, x, y0, w, h);
    }
};

} // namespace

quill::Font* style_font(const Style& s) { return font_for(s); }

float FontTextMeasurer::width(const Style& style, const std::string& utf8) {
    quill::Font* f = font_for(style);
    return f ? f->text_width(utf8) : static_cast<float>(utf8.size()) * style.font_size * 0.5f;
}

FontMetrics FontTextMeasurer::metrics(const Style& style) {
    quill::Font* f = font_for(style);
    if (!f) return {style.font_size * 0.8f, style.font_size * 0.2f, 0};
    float gap = static_cast<float>(f->line_height - f->ascent - f->descent);
    return {static_cast<float>(f->ascent), static_cast<float>(f->descent), std::max(0.f, gap)};
}

// CSS 2.1 14.2: the canvas takes the root element's background, or the body's when the root has none
// (<body bgcolor=... background=stars.gif> fills the whole window, tiled from the top-left of the page).
static const Box* canvas_owner(const Box& root) {
    auto has_bg = [](const Box* b) { return b && b->style && ((b->style->background_color >> 24) || !b->style->background_image.empty() || !b->style->gradients.empty()); };
    if (has_bg(&root)) return &root;
    for (auto& c : root.children)
        if (c->kind == Box::Kind::Element && c->tag == "body") return has_bg(c.get()) ? c.get() : nullptr;
    return nullptr;
}

static void gather_fixed(const Box& b, std::vector<const Box*>& out) {
    for (auto& c : b.children) {
        if (c->skipped || !c->style) continue;
        if (c->style->position == Position::Fixed || c->style->position == Position::Sticky) { out.push_back(c.get()); continue; }
        gather_fixed(*c, out);
    }
    for (auto& L : b.lines)
        for (auto& f : L.frags)
            if (f.kind == Frag::Kind::Atomic && f.box && f.box->parent != &b && f.box->style && !f.box->skipped) {
                if (f.box->style->position == Position::Fixed || f.box->style->position == Position::Sticky) out.push_back(f.box);
                else gather_fixed(*f.box, out);
            }
}

// position: sticky (CSS Positioned Layout 3), against the viewport: how far the box moves down (or up) from where it
// was laid out, given the document y of the viewport's top and its height. It never leaves its containing block.
namespace {
void content_extent(const Box& b, float& x0, float& y0, float& x1, float& y1, int depth) {
    if (depth > 60 || b.skipped || !b.style) return;
    for (auto& L : b.lines) {
        y0 = std::min(y0, L.y); y1 = std::max(y1, L.y + L.h);
        for (auto& f : L.frags) { x0 = std::min(x0, f.x); x1 = std::max(x1, f.x + f.w); }
    }
    for (auto& c : b.children) {
        if (!c->style || c->skipped || c->kind == Box::Kind::Text) continue;
        if (c->style->position == Position::Fixed || c->style->position == Position::Absolute) continue;
        x0 = std::min(x0, c->x); y0 = std::min(y0, c->y); x1 = std::max(x1, c->x + c->w); y1 = std::max(y1, c->y + c->h);
        if (c->style->overflow_x == Overflow::Visible && c->style->overflow_y == Overflow::Visible) content_extent(*c, x0, y0, x1, y1, depth + 1);
    }
}
} // namespace

bool marquee_offset(const Box& b, long long ms, float& dx, float& dy, int* next_ms) {
    dx = dy = 0;
    if (!b.style || !b.style->marquee || ms < 0) return false;
    const Style& s = *b.style;
    float cx = b.x + b.border.left + b.padding.left, cy = b.y + b.border.top + b.padding.top;
    float cw = b.w - b.border.left - b.border.right - b.padding.left - b.padding.right;
    float ch = b.h - b.border.top - b.border.bottom - b.padding.top - b.padding.bottom;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    content_extent(b, x0, y0, x1, y1, 0);
    if (x1 < x0 || y1 < y0 || cw <= 0 || ch <= 0) return false;
    bool  vert  = s.marquee_dir >= 2;
    bool  fwd   = s.marquee_dir == 1 || s.marquee_dir == 3; // right / down: moves towards larger coordinates
    float W     = vert ? ch : cw;                           // the window
    float w     = vert ? (y1 - y0) : (x1 - x0);             // the content
    float nat   = vert ? (y0 - cy) : (x0 - cx);             // where the content is now, relative to the window
    int   delay = std::max<int>(1, s.marquee_delay);
    long long steps = ms / delay;
    double    moved = static_cast<double>(steps) * std::max<int>(0, s.marquee_amount);
    bool      done  = s.marquee_amount <= 0;
    double    p     = 0; // where the content's leading edge goes, relative to the window
    int       loops = s.marquee_loop;
    if (s.marquee == 2) { // slide: in once, then stays against the far edge
        double end = fwd ? W - w : 0;
        p = fwd ? std::min<double>(end, -w + moved) : std::max<double>(end, W - moved);
        if (p == end) done = true;
    } else if (s.marquee == 3) { // alternate: bounces between the two edges
        double lo = std::min<double>(0, W - w), hi = std::max<double>(0, W - w), span = hi - lo;
        if (span < 1) { p = nat; done = true; }
        else {
            double trips = moved / span;
            if (loops > 0 && trips >= loops) { moved = loops * span; done = true; }
            double u   = std::fmod(moved, 2 * span);
            double pos = u < span ? u : 2 * span - u;
            p = fwd ? lo + pos : hi - pos;
        }
    } else { // scroll: in from one side, out of the other, again
        double D = W + w;
        if (loops > 0 && moved >= D * loops) { p = nat; done = true; }
        else {
            double u = std::fmod(moved, D);
            p = fwd ? -w + u : W - u;
        }
    }
    float off = static_cast<float>(p - nat);
    (vert ? dy : dx) = off;
    if (next_ms && !done) *next_ms = delay - static_cast<int>(ms % delay);
    return true;
}

float sticky_offset(const Box& b, float view_y, float view_h) {
    const Style& s = *b.style;
    const Box*   cb = b.parent;
    while (cb && (!cb->style || cb->kind != Box::Kind::Element || cb->style->display == Display::Inline)) cb = cb->parent;
    float top_lim = -1e9f, bot_lim = 1e9f;
    if (cb) {
        top_lim = cb->y + cb->border.top + cb->padding.top;
        bot_lim = cb->y + cb->h - cb->border.bottom - cb->padding.bottom;
    }
    float mt = s.margin[0].resolve(0), mb = s.margin[2].resolve(0);
    float dy = 0;
    if (!s.inset[0].is_auto()) {
        float want = view_y + s.inset[0].resolve(view_h);
        if (b.y < want) dy = std::min(want - b.y, std::max(0.f, bot_lim - mb - (b.y + b.h)));
    } else if (!s.inset[2].is_auto()) {
        float want = view_y + view_h - s.inset[2].resolve(view_h);
        if (b.y + b.h > want) dy = -std::min(b.y + b.h - want, std::max(0.f, b.y - mt - top_lim));
    }
    return dy;
}

bool has_fixed_boxes(const Box& root) {
    std::vector<const Box*> v;
    gather_fixed(root, v);
    return !v.empty();
}

void paint_fixed_layer(const Box& root, quill::Framebuffer& fb, const quill::IRect& area, float scroll_x, float scroll_y,
                       const PaintEnv& env) {
    std::vector<const Box*> fixed;
    gather_fixed(root, fixed);
    if (fixed.empty()) return;
    std::stable_sort(fixed.begin(), fixed.end(), [](const Box* a, const Box* b) { return a->style->z_index < b->style->z_index; });
    PaintEnv e   = env;
    e.skip_fixed = false;
    fb.push_clip(area);
    Painter p{fb, scroll_x - area.x, scroll_y - area.y, e};
    p.fx = -static_cast<float>(area.x);
    p.fy = -static_cast<float>(area.y);
    p.canvas_owner = nullptr;
    p.view_y = scroll_y;
    p.view_h = static_cast<float>(area.h);
    for (const Box* b : fixed) p.paint_entry(*b);
    fb.pop_clip();
}

static Color canvas_color(const Box* owner) {
    if (owner && (owner->style->background_color >> 24)) {
        Color c = owner->style->background_color;
        uint32_t a = c >> 24; // the canvas itself is opaque: blend a translucent background over white
        auto ch = [&](int sh) { return (((c >> sh) & 255) * a + 255 * (255 - a)) / 255; };
        return 0xff000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
    }
    return 0xffffffff;
}

void paint_document(const Box& root, quill::Framebuffer& fb, float scroll_x, float scroll_y, const PaintEnv& env) {
    const Box* owner = canvas_owner(root);
    fb.clear(qc(canvas_color(owner)));
    Painter p{fb, scroll_x, scroll_y, env};
    p.canvas_owner = owner;
    p.view_y = scroll_y;
    p.view_h = env.view_h > 0 ? env.view_h : static_cast<float>(fb.h);
    if (owner) p.paint_canvas_image(*owner, root, quill::IRect(0, 0, fb.w, fb.h));
    p.paint_context(root);
}

void paint_document_area(const Box& root, quill::Framebuffer& fb, const quill::IRect& area, float scroll_x, float scroll_y, const PaintEnv& env) {
    fb.push_clip(area);
    const Box* owner = canvas_owner(root);
    fb.fill_rect(area, qc(canvas_color(owner)));
    Painter p{fb, scroll_x - area.x, scroll_y - area.y, env};
    p.fx = static_cast<float>(area.x);
    p.fy = static_cast<float>(area.y);
    p.fx = -p.fx; p.fy = -p.fy; // Painter::X() subtracts the origin, so the fixed origin is -area
    p.canvas_owner = owner;
    p.view_y = scroll_y;
    p.view_h = env.view_h > 0 ? env.view_h : static_cast<float>(area.h);
    if (owner) p.paint_canvas_image(*owner, root, area);
    p.paint_context(root);
    fb.pop_clip();
}

namespace {
struct Hit {
    static bool real(const Box& b) { return b.style && b.kind != Box::Kind::Text && b.kind != Box::Kind::LineBreak && b.kind != Box::Kind::Marker; }
    static bool positioned(const Box& b) {
        if (!real(b)) return false;
        if (b.style->display == Display::Inline && b.replaced == ReplacedKind::None) return false;
        return b.style->position != Position::Static || b.style->opacity < 1.f || b.style->has_xform || b.style->marquee || b.scrollable();
    }
    float     scroll_y = 0, view_h = 0;
    long long anim_ms = -1;
    const BoxScroll* box_scroll = nullptr;
    static bool inside(const Box& b, float x, float y) { return x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h; }

    const Box* run(const Box& b, float x, float y) const {
        if (b.skipped || !real(b)) return nullptr;
        const Style& s = *b.style;
        bool clip = s.overflow_x != Overflow::Visible || s.overflow_y != Overflow::Visible;
        if (clip && !inside(b, x, y)) return nullptr;
        bool vis = s.visibility == Visibility::Visible;
        float bx = x, by = y; // the point in the box's own terms; its content may be moved (marquee)
        if (s.marquee && anim_ms >= 0) {
            float dx, dy;
            if (marquee_offset(b, anim_ms, dx, dy)) { x -= dx; y -= dy; }
        }
        if (b.scrollable()) {
            float ox, oy;
            box_scroll_of(b, box_scroll, ox, oy);
            x += ox;
            y += oy;
        }
        // children in paint order: z-sorted positioned boxes last
        struct E { const Box* c; int z; size_t seq; };
        std::vector<E> es;
        size_t         seq = 0;
        for (auto& c : b.children) {
            if (!real(*c) || c->skipped) continue;
            bool pos = positioned(*c);
            es.push_back({c.get(), pos ? (c->style->position == Position::Fixed || !c->style->z_auto ? c->style->z_index : 0) : -1000000, seq++});
        }
        for (const Box* f : b.inline_floats) if (!f->skipped) es.push_back({f, -999999, seq++});
        std::stable_sort(es.begin(), es.end(), [](const E& a, const E& b2) { return a.z != b2.z ? a.z < b2.z : a.seq < b2.seq; });
        for (size_t i = es.size(); i-- > 0;) {
            const Box* c = es[i].c;
            if (c->replaced == ReplacedKind::None || true) {
                // inline children of inline containers are reached through frags, not here
                if (!b.lines.empty() && !positioned(*c) && c->style->float_ == Float::None && c->style->display != Display::InlineBlock) continue;
                float cy = y;
                if (view_h > 0 && c->style->position == Position::Fixed) cy = y - scroll_y; // laid out in viewport coordinates
                else if (view_h > 0 && c->style->position == Position::Sticky) cy = y - sticky_offset(*c, scroll_y, view_h);
                if (const Box* r = run(*c, x, cy)) return r;
            }
        }
        if (vis) {
            for (auto& L : b.lines) {
                if (y < L.y || y >= L.y + L.h) continue;
                for (auto& f : L.frags) {
                    if (f.kind == Frag::Kind::Atomic) { if (const Box* r = run(*f.box, x, y)) return r; continue; }
                    if (x >= f.x && x < f.x + f.w && f.box && f.box->style && f.box->style->visibility == Visibility::Visible) return f.box;
                }
            }
            if (b.replaced != ReplacedKind::None || (s.display != Display::Inline && inside(b, bx, by))) return inside(b, bx, by) ? &b : nullptr;
        }
        return nullptr;
    }
};
} // namespace

// Floats paint above all in-flow block backgrounds of their stacking context, wherever they sit in the tree, so a float
// nested in an earlier sibling (Wikipedia's infobox) must win over a later full-width block that merely overlaps it.
static const Box* float_hit(const Box& b, float x, float y, const Hit& h, int depth) {
    if (depth > 200 || b.skipped || !b.style) return nullptr;
    for (auto& c : b.children) {
        if (!Hit::real(*c) || c->skipped) continue;
        if (Hit::positioned(*c)) continue; // its own stacking context
        if (c->style->float_ != Float::None) {
            if (const Box* r = h.run(*c, x, y)) return r;
        } else if (const Box* r = float_hit(*c, x, y, h, depth + 1)) return r;
    }
    for (const Box* f : b.inline_floats)
        if (!f->skipped && !Hit::positioned(*f))
            if (const Box* r = h.run(*f, x, y)) return r;
    return nullptr;
}

void box_scroll_of(const Box& b, const BoxScroll* map, float& x, float& y) {
    x = y = 0;
    if (!map || !b.node || !b.scrollable()) return;
    auto it = map->find(b.node);
    if (it == map->end()) return;
    x = std::max(0.f, std::min(b.scroll_max_x, it->second.first));
    y = std::max(0.f, std::min(b.scroll_max_y, it->second.second));
}

const Box* hit_test(const Box& root, float x, float y, float scroll_y, float view_h, long long anim_ms, const BoxScroll* box_scroll) {
    Hit h;
    h.box_scroll = box_scroll;
    h.scroll_y = scroll_y;
    h.view_h   = view_h;
    h.anim_ms  = anim_ms;
    const Box* r = h.run(root, x, y);
    if (!r) return r;
    for (const Box* p = r; p; p = p->parent)
        if (p->style && p->style->float_ != Float::None) return r; // already inside a float
    const Box* ctx = &root; // nearest positioned ancestor: the layer whose floats compete with r
    for (const Box* p = r; p; p = p->parent)
        if (Hit::positioned(*p)) { ctx = p; break; }
    if (ctx == r) return r;
    if (const Box* f = float_hit(*ctx, x, y, h, 0)) return f;
    return r;
}

static void extent_walk(const Box& b, float& bottom, int depth) {
    if (b.skipped || !b.style || depth > 200) return;
    if (b.kind != Box::Kind::Text && b.kind != Box::Kind::LineBreak && b.kind != Box::Kind::Marker) {
        if (b.style->position == Position::Fixed) return;
        if (b.style->visibility == Visibility::Visible || true) bottom = std::max(bottom, b.y + b.h);
        if (b.style->overflow_x != Overflow::Visible || b.style->overflow_y != Overflow::Visible) return;
    }
    for (auto& c : b.children) extent_walk(*c, bottom, depth + 1);
    for (const Box* f : b.inline_floats) extent_walk(*f, bottom, depth + 1);
    for (auto& L : b.lines) {
        bottom = std::max(bottom, L.y + L.h);
        for (auto& f : L.frags)
            if (f.kind == Frag::Kind::Atomic && f.box && f.box->parent != &b) extent_walk(*f.box, bottom, depth + 1);
    }
}

float scroll_extent(const Box& root) {
    float bottom = root.y + root.h;
    extent_walk(root, bottom, 0);
    return bottom;
}

} // namespace quill::engine
