// Decoded images: PNG, JPEG, GIF (animated), BMP via stb_image; WebP via libwebp
// (optional); SVG via nanosvg (re-rendered crisply at the size it is drawn at).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quill {

class SvgDoc;

struct Image {
    int                   w = 0, h = 0;
    std::vector<uint32_t> px; // 0xAARRGGBB, straight (non-premultiplied) alpha
    std::shared_ptr<const SvgDoc> svg; // set for vector images: scaled() re-renders from this

    // Animated GIF: this image is frame 0, `frames` holds the rest (same size); delays_ms has one entry per frame
    // (frame 0 included). Empty for still images.
    std::vector<std::shared_ptr<Image>> frames;
    std::vector<int>                    delays_ms;
    int                                 total_ms = 0;
    bool animated() const { return !frames.empty() && total_ms > 0; }
    // The frame showing `t_ms` after the animation started (it loops forever). `next_ms`, if given, receives the
    // milliseconds until the next frame is due.
    const Image& frame_at(long long t_ms, int* next_ms = nullptr) const;

    // Returns this image resampled to tw x th (area average when shrinking,
    // bilinear when enlarging; SVGs are re-rendered). Results are cached; main thread only.
    const Image& scaled(int tw, int th) const;

private:
    mutable std::vector<std::unique_ptr<Image>> cache_;
};

// True if this build can decode WebP.
bool webp_supported();

// Returns nullptr if the data is not a supported image or is absurdly large.
std::shared_ptr<Image> decode_image(const unsigned char* data, size_t len, size_t max_pixels = 40u * 1000 * 1000);

// Parses "data:[<mime>][;base64],<payload>". Returns false if malformed.
bool decode_data_uri(const std::string& uri, std::string& mime, std::string& bytes);

} // namespace quill
