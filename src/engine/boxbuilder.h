// DOM + computed styles -> box tree. Internal header (pulls libdom/libcss C headers).
#pragma once

#include "box.h"
#include "domutil.h"
#include "elemcss.h"

#include <functional>
#include <functional>
#include <memory>
#include <unordered_set>

namespace quill::engine {

// pseudo: 0 = the element itself, 3 = ::before, 4 = ::after (libcss's CSS_PSEUDO_ELEMENT_* values).
using StyleLookup = std::function<const css_computed_style*(dom_node*, int pseudo)>;

// Optional: per-element extras (border-radius, box-shadow) from the element cascade.
using ExtraLookup = std::function<const Extras*(dom_node*, int pseudo)>; // pseudo: 0 element, 1 ::before, 2 ::after

// Restyles a laid-out tree in place after the computed styles of `nodes` changed: every box of those elements (and the
// text / anonymous / generated boxes that share or derive their style) gets its new style, but only if
// `layout_equal(old, new)` holds for all of them; otherwise nothing is touched and false is returned.
bool restyle_in_place(Box& root, dom_node* root_node, const std::unordered_set<const void*>& nodes, const StyleLookup& styles,
                      const css_unit_ctx& uc, const ExtraLookup& extras, uint64_t font_scope,
                      const std::function<bool(const Style&, const Style&)>& layout_equal, float zoom = 1.f);
// font_scope: the page's web fonts (FontManager scope), written into every Style.
// zoom: page zoom, the layout px per CSS px (every length is multiplied by it; `uc` describes the CSS viewport).
std::unique_ptr<Box> build_box_tree(dom_node* root, const StyleLookup& styles, const css_unit_ctx& uc, const ExtraLookup& extras = nullptr,
                                    uint64_t font_scope = 0, float zoom = 1.f);

} // namespace quill::engine
