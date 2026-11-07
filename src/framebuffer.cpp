#include "framebuffer.h"

#include <algorithm>
#include <cmath>

namespace quill {

IRect IRect::intersect(const IRect& o) const {
    int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    if (x1 <= x0 || y1 <= y0) return IRect(x0, y0, 0, 0);
    return IRect(x0, y0, x1 - x0, y1 - y0);
}

static inline uint32_t blend(uint32_t dst, Color c, int a255) {
    // a255 in 0..255 : effective source alpha
    if (a255 <= 0) return dst;
    if (a255 >= 255) return 0xFF000000u | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
    int ia = 255 - a255;
    int dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    int r = (c.r * a255 + dr * ia + 127) / 255;
    int g = (c.g * a255 + dg * ia + 127) / 255;
    int b = (c.b * a255 + db * ia + 127) / 255;
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

void Framebuffer::resize(int nw, int nh) {
    w = std::max(0, nw);
    h = std::max(0, nh);
    px.assign(static_cast<size_t>(w) * h, 0xFFFFFFFFu);
    clip_ = IRect(0, 0, w, h);
    stack_.clear();
}

void Framebuffer::clear(Color c) {
    std::fill(px.begin(), px.end(), 0xFF000000u | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b);
    clip_ = IRect(0, 0, w, h);
    stack_.clear();
}

void Framebuffer::push_clip(const IRect& r) {
    stack_.push_back(clip_);
    clip_ = clip_.intersect(r);
}

void Framebuffer::pop_clip() {
    if (stack_.empty()) {
        clip_ = IRect(0, 0, w, h);
        return;
    }
    clip_ = stack_.back();
    stack_.pop_back();
}

void Framebuffer::blend_px(int x, int y, Color c, int cov) {
    if (!clip_.contains(x, y)) return;
    int a = (c.a * cov + 127) / 255;
    uint32_t& d = px[static_cast<size_t>(y) * w + x];
    d           = blend(d, c, a);
}

void Framebuffer::fill_rect(const IRect& r, Color c) {
    IRect v = r.intersect(clip_);
    if (v.empty() || c.a == 0) return;
    if (c.a == 255) {
        uint32_t p = 0xFF000000u | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
        for (int y = v.y; y < v.bottom(); y++) {
            uint32_t* row = &px[static_cast<size_t>(y) * w];
            std::fill(row + v.x, row + v.right(), p);
        }
        return;
    }
    for (int y = v.y; y < v.bottom(); y++) {
        uint32_t* row = &px[static_cast<size_t>(y) * w];
        for (int x = v.x; x < v.right(); x++) row[x] = blend(row[x], c, c.a);
    }
}

// Coverage of pixel centre (px,py) by a circle quadrant corner. (cx,cy) is the circle centre.
static inline int corner_cov(float px, float py, float cx, float cy, float r) {
    float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
    float d  = std::sqrt(dx * dx + dy * dy);
    float c  = r - d + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return static_cast<int>(c * 255.0f);
}

void Framebuffer::fill_round_rect(const IRect& r, int rtl, int rtr, int rbr, int rbl, Color c) {
    int maxr = std::min(r.w, r.h) / 2;
    rtl = std::min(std::max(rtl, 0), maxr);
    rtr = std::min(std::max(rtr, 0), maxr);
    rbr = std::min(std::max(rbr, 0), maxr);
    rbl = std::min(std::max(rbl, 0), maxr);
    if (rtl == 0 && rtr == 0 && rbr == 0 && rbl == 0) {
        fill_rect(r, c);
        return;
    }
    IRect v = r.intersect(clip_);
    if (v.empty() || c.a == 0) return;
    for (int y = v.y; y < v.bottom(); y++) {
        for (int x = v.x; x < v.right(); x++) {
            int cov = 255;
            if (x < r.x + rtl && y < r.y + rtl) cov = corner_cov(x, y, r.x + rtl, r.y + rtl, rtl);
            else if (x >= r.right() - rtr && y < r.y + rtr) cov = corner_cov(x, y, r.right() - rtr, r.y + rtr, rtr);
            else if (x >= r.right() - rbr && y >= r.bottom() - rbr) cov = corner_cov(x, y, r.right() - rbr, r.bottom() - rbr, rbr);
            else if (x < r.x + rbl && y >= r.bottom() - rbl) cov = corner_cov(x, y, r.x + rbl, r.bottom() - rbl, rbl);
            if (cov > 0) {
                uint32_t& d = px[static_cast<size_t>(y) * w + x];
                d           = blend(d, c, (c.a * cov + 127) / 255);
            }
        }
    }
}

void Framebuffer::stroke_round_rect(const IRect& r, int rtl, int rtr, int rbr, int rbl, int t, Color c) {
    int maxr = std::min(r.w, r.h) / 2;
    rtl = std::min(std::max(rtl, 0), maxr);
    rtr = std::min(std::max(rtr, 0), maxr);
    rbr = std::min(std::max(rbr, 0), maxr);
    rbl = std::min(std::max(rbl, 0), maxr);
    t = std::max(1, std::min(t, maxr > 0 ? std::max(maxr, 1) : t));
    // straight edges between the corner squares
    fill_rect(IRect(r.x + rtl, r.y, r.w - rtl - rtr, t), c);
    fill_rect(IRect(r.x + rbl, r.bottom() - t, r.w - rbl - rbr, t), c);
    fill_rect(IRect(r.x, r.y + rtl, t, r.h - rtl - rbl), c);
    fill_rect(IRect(r.right() - t, r.y + rtr, t, r.h - rtr - rbr), c);
    // corners: ring = outer disc minus inner disc
    struct Corner { int R, sx, sy; };            // radius, square origin
    const Corner cs[4] = {{rtl, r.x, r.y}, {rtr, r.right() - rtr, r.y}, {rbr, r.right() - rbr, r.bottom() - rbr}, {rbl, r.x, r.bottom() - rbl}};
    for (int k = 0; k < 4; k++) {
        int R = cs[k].R;
        if (R == 0) { // square corner: fill the t x t block
            int bx = (k == 0 || k == 3) ? r.x : r.right() - t;
            int by = (k == 0 || k == 1) ? r.y : r.bottom() - t;
            fill_rect(IRect(bx, by, t, t), c);
            continue;
        }
        float cx = (k == 0 || k == 3) ? cs[k].sx + R : cs[k].sx;
        float cy = (k == 0 || k == 1) ? cs[k].sy + R : cs[k].sy;
        for (int y = cs[k].sy; y < cs[k].sy + R; y++)
            for (int x = cs[k].sx; x < cs[k].sx + R; x++) {
                int outer = corner_cov(static_cast<float>(x), static_cast<float>(y), cx, cy, static_cast<float>(R));
                int inner = R - t > 0 ? corner_cov(static_cast<float>(x), static_cast<float>(y), cx, cy, static_cast<float>(R - t)) : 0;
                int cov   = outer * (255 - inner) / 255;
                if (cov > 0) blend_px(x, y, c, cov);
            }
    }
}

void Framebuffer::blit_tiled(const Image& img, int ox, int oy, bool repeat_x, bool repeat_y, const IRect& area) {
    if (img.w <= 0 || img.h <= 0) return;
    IRect v = area.intersect(clip_);
    if (v.empty()) return;
    for (int y = v.y; y < v.bottom(); y++) {
        int sy = y - oy;
        if (repeat_y) {
            sy %= img.h;
            if (sy < 0) sy += img.h;
        } else if (sy < 0 || sy >= img.h) continue;
        const uint32_t* srow = &img.px[static_cast<size_t>(sy) * img.w];
        uint32_t*       drow = &px[static_cast<size_t>(y) * w];
        for (int x = v.x; x < v.right(); x++) {
            int sx = x - ox;
            if (repeat_x) {
                sx %= img.w;
                if (sx < 0) sx += img.w;
            } else if (sx < 0 || sx >= img.w) continue;
            uint32_t s = srow[sx];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 255) {
                drow[x] = s;
            } else {
                Color c(static_cast<uint8_t>((s >> 16) & 255), static_cast<uint8_t>((s >> 8) & 255), static_cast<uint8_t>(s & 255));
                drow[x] = blend(drow[x], c, static_cast<int>(a));
            }
        }
    }
}

} // namespace quill
