// Web font sanitizer: turns a font a web page sent (TrueType, WOFF, WOFF2) into a small, freshly written TrueType font
// that only contains tables Quill draws with, every byte of which has been checked. stb_truetype assumes well-formed
// fonts (its documentation says not to use it on untrusted data), so it never sees a downloaded font directly: like
// browsers' OpenType Sanitizer, the parsing that has to be careful happens here, once, with bounds checks everywhere.
//
// Kept: cmap (rewritten as one format 12 subtable), head, hhea, maxp, hmtx, loca (long) and glyf (each glyph validated;
// broken ones become empty, composite glyphs that refer to missing glyphs or loop are dropped). Everything else
// (hinting programs, kerning, layout tables, CFF outlines) is left out. CFF-only fonts (.otf with "OTTO") are refused.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace quill {

struct SanitizedFont {
    std::vector<unsigned char> data;     // a TrueType font stb_truetype can use
    bool                       variable = false; // the source had variation axes (only its default instance is drawn)
    size_t                     glyphs = 0, mapped = 0, dropped_glyphs = 0;
};

// `in` may be sfnt (TrueType), WOFF or WOFF2. Returns false with a reason in `err`.
bool sanitize_font(const unsigned char* in, size_t n, SanitizedFont& out, std::string* err = nullptr);

// True when this build can decode WOFF2 (needs Brotli).
bool woff2_supported();

} // namespace quill
