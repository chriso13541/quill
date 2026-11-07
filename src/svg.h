// SVG rendering through the vendored nanosvg (parser + rasterizer).
// Supports shapes, paths, fills/strokes, gradients, transforms and presentation
// attributes. Does NOT support <style>/CSS classes, text, filters, masks or
// external references (nothing is ever fetched on behalf of an SVG).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace quill {

class SvgDoc {
public:
    ~SvgDoc();
    float width() const;  // intrinsic size in CSS px
    float height() const;
    // Renders the drawing fitted (aspect preserved, centred) into a tw x th box.
    // `out` receives 0xAARRGGBB straight-alpha pixels.
    bool render(int tw, int th, std::vector<uint32_t>& out) const;

    // Returns nullptr for anything that is not a usable SVG.
    static std::shared_ptr<SvgDoc> parse(const unsigned char* data, size_t len);
    // Cheap check on the first bytes: does this look like an SVG document?
    static bool sniff(const unsigned char* data, size_t len);

private:
    SvgDoc() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quill
