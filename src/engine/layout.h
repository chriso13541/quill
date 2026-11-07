// Layout: box tree -> positions and sizes (CSS 2.1 normal flow: block formatting with margin
// collapsing, inline formatting with line breaking, inline-block / replaced shrink-to-fit).
// Floats are laid out as atomic inline boxes and absolute/fixed boxes are skipped for now (E4);
// flex and tables stack as blocks until E5/E4. libdom/libcss free.
#pragma once

#include "box.h"

#include <functional>
#include <string>

namespace quill::engine {

struct FontMetrics {
    float ascent = 0, descent = 0, line_gap = 0;
};

class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    virtual float       width(const Style& style, const std::string& utf8) = 0;
    virtual FontMetrics metrics(const Style& style) = 0;
};

// Deterministic measurer for tests: every code point is 0.5em wide, ascent 0.8em, descent 0.2em.
TextMeasurer& fake_text_measurer();

// Intrinsic size of an image by its src attribute; return false if unknown.
using ImageSizer = std::function<bool(const std::string& src, int& w, int& h)>;
// True if the image is known to be unavailable (failed or blocked), as opposed to still loading.
using ImageBroken = std::function<bool(const std::string& src)>;

struct LayoutEnv {
    TextMeasurer* text = nullptr; // null = fake_text_measurer()
    ImageSizer    image_size;
    ImageBroken   image_broken;
    std::function<void(const std::string& src)> image_request; // start loading (layout doesn't need its size)
    float         viewport_w = 1024, viewport_h = 768;
    float         zoom = 1; // page zoom: pictures' and controls' intrinsic sizes are multiplied by it (styles already are)
};

// Lays out the tree rooted at `root` (the <html> box). Afterwards every Box's x/y/w/h is absolute.
void layout_document(Box& root, const LayoutEnv& env);
// Recomputes the ink extents (paint culling) of a laid-out tree whose paint-only properties changed (box-shadow...).
void update_ink(Box& root);

} // namespace quill::engine
