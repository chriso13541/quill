// Paint: laid-out box tree -> pixels in a Framebuffer, using Quill's stb_truetype fonts.
// Tree-order painting (backgrounds, borders, replaced content, text, markers) with overflow clipping;
// stacking contexts / z-index / opacity layers arrive with positioning (E4).
#pragma once

#include "box.h"
#include "layout.h"

#include "framebuffer.h"
#include "image.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace quill {
class Font;
}

namespace quill::engine {

// Measures text with Quill's FontManager (the same fonts the painter draws with).
class FontTextMeasurer : public TextMeasurer {
public:
    float       width(const Style& style, const std::string& utf8) override;
    FontMetrics metrics(const Style& style) override;
};

// Selected text, by fragment: the byte range [first, second) of the fragment's text that is selected.
using TextSelection = std::unordered_map<const Frag*, std::pair<size_t, size_t>>;
// The font the painter draws `s` with (for measuring text the way it is drawn).
quill::Font* style_font(const Style& s);

// Scroll positions of scroll containers (overflow auto / scroll), by DOM element: (x, y), clamped to the box's range.
using BoxScroll = std::unordered_map<const void*, std::pair<float, float>>;
// Where `b`'s content is scrolled to (0, 0 if it isn't a scroll container or isn't in the map).
void box_scroll_of(const Box& b, const BoxScroll* map, float& x, float& y);

struct PaintEnv {
    // Decoded image for an <img src> / background-image url, or null if unavailable.
    std::function<const quill::Image*(const std::string& src)> image;
    // True if an image will never arrive (failed / blocked): its alt text is drawn instead of a loading placeholder.
    std::function<bool(const std::string& src)> image_broken;
    // Paints a live form control (input / textarea / select) in place of the static rendering. (ox, oy) is
    // where document (0,0) lands on the framebuffer. Return false to fall back to the static rendering.
    std::function<bool(const Box&, quill::Framebuffer&, int ox, int oy)> control;
    // Paints an <iframe>'s page into its content box (`r`, framebuffer coordinates). Absent: an empty frame.
    std::function<void(const Box&, quill::Framebuffer&, const quill::IRect& r)> frame;
    // Live checked state of a checkbox / radio: 1, 0, or -1 to use the markup.
    std::function<int(const Box&)> checked;
    // Animated images: milliseconds since the page's animations started (< 0 = always draw the first frame).
    long long anim_ms = -1;
    // If set, receives the fewest milliseconds until some animated image that was painted shows its next frame
    // (left alone when nothing animated was painted; the caller initialises it to -1).
    int* anim_next_ms = nullptr;
    // Skip boxes and lines outside the clip (normal). Off only to check culling against a full paint.
    bool cull = true;
    // Leave position:fixed boxes out (for cached page tiles, which scroll; paint_fixed_layer() adds them per frame).
    bool skip_fixed = false; // (and position:sticky ones, which also move with the scroll position)
    // Height of the viewport the document is shown in (sticky boxes stick to it); 0 = the painted area's height.
    float view_h = 0;
    // Scroll containers' positions (null: all at the top-left).
    const BoxScroll* box_scroll = nullptr;
    // Selected text, highlighted (null: none).
    const TextSelection* selection = nullptr;
    // Page zoom: background and mask pictures' natural sizes are multiplied by it (as layout does for <img>).
    float zoom = 1;
};

// Paints `root` into fb. (scroll_x, scroll_y) is the document coordinate shown at the framebuffer's origin.
void paint_document(const Box& root, quill::Framebuffer& fb, float scroll_x, float scroll_y, const PaintEnv& env);

// Same, but only inside `area` (framebuffer coordinates): the document's (scroll_x, scroll_y) lands on
// area's top-left, the canvas colour fills the area and nothing is drawn outside it.
// Does the page have position:fixed boxes (which stay put while the rest scrolls)?
bool has_fixed_boxes(const Box& root);
// Only the position:fixed boxes, on top of what `fb` already shows (cached tiles painted with skip_fixed).
void paint_fixed_layer(const Box& root, quill::Framebuffer& fb, const quill::IRect& area, float scroll_x, float scroll_y,
                       const PaintEnv& env);

void paint_document_area(const Box& root, quill::Framebuffer& fb, const quill::IRect& area, float scroll_x, float scroll_y, const PaintEnv& env);

// Deepest box under a document-coordinate point (reverse paint order, respects overflow clips,
// skips hidden boxes). Text hits return the text box; walk ->parent for the element. Null if none.
// (x, y) in document coordinates. With the scroll position and viewport height, position:fixed and sticky boxes are
// found where they are shown.
const Box* hit_test(const Box& root, float x, float y, float scroll_y = 0, float view_h = 0, long long anim_ms = -1,
                    const BoxScroll* box_scroll = nullptr);
// How far a position:sticky box is shifted from its laid-out place with the viewport at document y `view_y`.
float sticky_offset(const Box& b, float view_y, float view_h);
// <marquee>: how far its content is moved `anim_ms` after the page's animations started. False (no offset) when the
// box isn't a marquee or there is no clock (anim_ms < 0, a still render). `next_ms` (if set) gets the ms until it moves.
bool marquee_offset(const Box& b, long long anim_ms, float& dx, float& dy, int* next_ms = nullptr);
// Bottom edge of everything the page can scroll to: the root box and any descendant that overflows it (boxes that clip
// their content only count with their own border box; fixed boxes do not scroll).
float scroll_extent(const Box& root);

} // namespace quill::engine
