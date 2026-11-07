// HTML presentational hints: the legacy attributes (bgcolor, text, link, <font color/size/face>, table
// border/cellpadding/cellspacing/width/align, td valign/nowrap, hr size/noshade, img border/hspace/align, ...)
// turned into CSS declarations, as the HTML standard's rendering section describes. libcss applies them at
// author level with specificity zero, so any author rule overrides them and they override the UA sheet.
//
// Include only from .cpp files (pulls in the libdom / libcss C headers through domutil.h).
#pragma once

#include "domutil.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace quill::engine {

// Appends the hints for element `n` to `out`. `quirks`: the document is in quirks mode (no / old doctype),
// which adds the table font reset every browser does for such pages. Strings in the hints are referenced
// (and the font-family array malloc'd) for libcss, which takes ownership when it applies them.
// `visited` tells whether a link element counts as visited (for <body link=... vlink=...>).
void presentational_hints(dom_node* n, bool quirks, const std::function<bool(dom_node*)>& visited, std::vector<css_hint>& out);

// HTML "rules for parsing a legacy colour value": named colours, #rgb, #rrggbb, and the forgiving fallback
// that turns "ffcc00", "#0f0f0f0f" or even "chucknorris" into a colour like every browser does.
// Returns false for empty strings and "transparent". argb is 0xAARRGGBB (alpha always 0xff).
bool parse_legacy_color(const std::string& value, uint32_t& argb);

// HTML "rules for parsing dimension values": "120", " 50% ", "12.5px" (trailing junk ignored).
// Returns false if there is no leading number.
bool parse_html_dimension(const std::string& value, float& out, bool& percent);

} // namespace quill::engine
