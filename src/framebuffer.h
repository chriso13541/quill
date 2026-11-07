// A tiny software renderer: ARGB8888 pixels in memory, clipping, rect fills,
// rounded rects and image blits. No dependencies, so it behaves the same on
// Linux and Windows and is trivially testable without a window.
#pragma once
#include "image.h"

#include <cstdint>
#include <vector>

namespace quill {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    Color() = default;
    Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) : r(r_), g(g_), b(b_), a(a_) {}
};

struct IRect {
    int x = 0, y = 0, w = 0, h = 0;
    IRect() = default;
    IRect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
    int  right() const { return x + w; }
    int  bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    IRect intersect(const IRect& o) const;
};

class Framebuffer {
public:
    int                   w = 0, h = 0;
    std::vector<uint32_t> px;

    void resize(int nw, int nh);
    void clear(Color c);

    void  push_clip(const IRect& r); // intersected with the current clip
    void  pop_clip();
    IRect clip() const { return clip_; }

    void fill_rect(const IRect& r, Color c);
    // Radii are per corner: top-left, top-right, bottom-right, bottom-left (circular).
    void fill_round_rect(const IRect& r, int rtl, int rtr, int rbr, int rbl, Color c);
    // Outline of a rounded rect, `t` pixels thick, drawn inside r (used for uniform CSS borders with border-radius).
    void stroke_round_rect(const IRect& r, int rtl, int rtr, int rbr, int rbl, int t, Color c);
    void blend_px(int x, int y, Color c, int coverage255);
    // Draws `img` (already the right size) anchored at (ox,oy), optionally tiled, inside `area`.
    void blit_tiled(const Image& img, int ox, int oy, bool repeat_x, bool repeat_y, const IRect& area);

private:
    IRect              clip_;
    std::vector<IRect> stack_;
};

} // namespace quill
