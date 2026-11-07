// Fonts via stb_truetype. System fonts are found by scanning well-known
// directories for well-known file names (no fontconfig, no FreeType).
//
// Families: CSS names are mapped to sans / serif / monospace. Bold and italic
// use the real face when installed, otherwise are synthesised. Missing glyphs
// fall back to broad-coverage fonts (DejaVu, Noto, CJK fonts) when present.
#pragma once
#include "framebuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quill {

class Font {
public:
    Font();
    ~Font();

    // Metrics in pixels, valid after creation.
    int   ascent      = 0;
    int   descent     = 0;
    int   line_height = 0; // ascent + descent + line gap
    int   x_height    = 0;
    int   ch_width    = 0; // advance of '0'
    float size        = 16;

    float text_width(const std::string& utf8);
    void  draw(Framebuffer& fb, const std::string& utf8, int x, int baseline, Color color);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

class FontManager {
public:
    static FontManager& instance();

    // Optional extra directory searched before the system ones. Call before the first get().
    void set_extra_dir(const std::string& dir);
    // True if at least one usable font was found.
    bool have_fonts();

    // `css_family` is a CSS font-family list ("Helvetica, Arial, sans-serif"). With a `scope`, names the scope has web
    // fonts for (@font-face) use those; everything else (and characters the web font lacks) uses the system fonts.
    Font* get(const std::string& css_family, float size_px, int weight, bool italic, uint64_t scope = 0);

    // ---- web fonts. A scope is one page's set of @font-face fonts: other pages never see them (a page can't restyle
    // another tab by defining "Arial"). Font data must come from sanitize_font() (fontsan.h).
    uint64_t new_scope();
    void     drop_scope(uint64_t scope); // frees the scope's fonts: only once nothing drawn with them is in use
    struct WebFace {
        std::string                               family;        // as in @font-face (matched case-insensitively)
        int                                       weight_lo = 400, weight_hi = 400;
        bool                                      italic   = false;
        bool                                      variable = false; // only its default instance is available
        std::vector<std::pair<uint32_t, uint32_t>> ranges;       // unicode-range (empty = everything)
        std::shared_ptr<const std::vector<unsigned char>> data;  // sanitized TrueType
    };
    // False if the scope is gone (the page was closed while the font downloaded) or the data won't load.
    bool add_web_face(uint64_t scope, const WebFace& face);
    // Bumped whenever a scope gains a face: fonts picked before then may be stale (callers re-ask get()).
    uint64_t generation() const { return gen_.load(); }

    struct Impl;

private:
    FontManager();
    ~FontManager();
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> gen_{1};
};

// UTF-8 helper shared with the container (decodes one code point, advancing i).
uint32_t utf8_next(const std::string& s, size_t& i);

} // namespace quill
