// The box tree: what layout consumes. Built from the styled DOM by boxbuilder.cpp (CSS 2.1 ch. 9/17
// box generation: display:none dropped, anonymous blocks around inline runs, block-in-inline splitting,
// anonymous table objects, list markers, ::before/::after, flex item blockification).
// Free of libdom/libcss types.
#pragma once

#include "style.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quill::engine {

enum class ReplacedKind : uint8_t { None, Image, Input, Textarea, Select, Video, Canvas, Svg, Iframe, Object, Meter, Progress };

struct Box;

struct Edges {
    float top = 0, right = 0, bottom = 0, left = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// One piece of laid-out inline content on a line. Text: `text` drawn with its box's style, `y` is the
// baseline. Atomic: an inline-block / replaced box positioned by the line (see Box::x/y).
struct Frag {
    enum class Kind : uint8_t { Text, Atomic } kind = Kind::Text;
    Box*        box = nullptr;
    std::string text;
    float       x = 0, y = 0, w = 0;
};

// Decoration extent of an inline element on one line (background / border / underline propagation).
struct Span {
    Box*  box = nullptr;
    float x = 0, w = 0;
    float baseline = 0, ascent = 0, descent = 0; // content area (font ascent/descent) around the baseline
    bool  first = false, last = false;
};

struct Line {
    float             x = 0, y = 0, w = 0, h = 0, baseline = 0; // baseline is relative to y
    float             ink_y0 = 0, ink_y1 = 0; // vertical extent of what painting this line touches (incl. inline-blocks)
    std::vector<Frag> frags;
    std::vector<Span> spans;
};

struct Box {
    enum class Kind : uint8_t { Element, Anonymous, Text, LineBreak, Marker };
    enum class Pseudo : uint8_t { None, Before, After };

    Kind                         kind = Kind::Element;
    Pseudo                       pseudo = Pseudo::None;
    std::shared_ptr<const Style> style;
    void*                        node = nullptr; // the DOM element (opaque); null for generated boxes
    std::string                  tag;            // element name, "" for anonymous/text
    std::string                  text;           // Text: the characters; Marker: marker text; controls: value/label text
    std::vector<std::pair<std::string, std::string>> attrs; // whitelisted attributes
    ReplacedKind                 replaced = ReplacedKind::None;
    bool                         continuation = false; // later fragment of an inline split around a block
    int                          list_ordinal = 0;

    Box*                              parent = nullptr;
    std::vector<std::unique_ptr<Box>> children;
    // A block that was inside an inline (<a href><div>...</div></a>) is lifted out of it when the inline is split
    // (CSS 2.1 9.2.1.1). This childless stand-in for that inline keeps the element (node, tag, attributes) on the block's
    // ancestor chain, so links and labels still work. Chains through nested inlines; layout and paint ignore it.
    std::shared_ptr<Box>              split_parent;
    const Box* logical_parent() const { return split_parent ? split_parent.get() : parent; }

    // ---- layout results. x/y/w/h are the border box; relative to the parent's content origin while
    // layout runs, absolute (document coordinates) once layout_document() returns.
    float x = 0, y = 0, w = 0, h = 0;
    Edges margin, border, padding;
    std::vector<Line> lines; // inline formatting context owned by this box (line-relative to its content origin)
    float last_baseline = -1;  // from the border-box top, for inline-block baseline alignment
    bool  border_fixed = false; // border widths set by layout (collapsed table borders), not from the style
    bool  skipped = false;     // out of flow and not laid out (absolute/fixed for now)
    bool  has_marker = false;  // list-item marker position (outside marker), see marker_x/marker_y
    float marker_x = 0, marker_y = 0, marker_w = 0;
    float min_content = -1, max_content = -1; // intrinsic width cache (content box)
    float content_dy = 0;                      // vertical shift of the content (table cell vertical-align)
    std::vector<Box*> inline_floats;           // floats laid out inside this box's inline content
    // Vertical extent (document coordinates) of everything painting this box's own flow touches: the box, its in-flow
    // descendants, lines, floats and shadows. Computed at the end of layout; paint skips subtrees outside the screen.
    float ink_y0 = 0, ink_y1 = 0;
    bool  ink_valid = false;
    // A scroll container (overflow auto / scroll) whose content is bigger than its padding box: how far it can scroll.
    // Computed at the end of layout; 0 = nothing to scroll that way.
    float scroll_max_x = 0, scroll_max_y = 0;
    bool  scrollable() const { return scroll_max_x > 0.5f || scroll_max_y > 0.5f; }

    const std::string* attr(const char* name) const {
        for (auto& a : attrs)
            if (a.first == name) return &a.second;
        return nullptr;
    }
    bool is_text() const { return kind == Kind::Text; }
    bool is_block_level() const { return kind != Kind::Text && kind != Kind::LineBreak && kind != Kind::Marker && style && style->block_level(); }
    bool is_inline_level() const {
        if (kind == Kind::Text || kind == Kind::LineBreak) return true;
        return kind != Kind::Marker && style && style->inline_level();
    }
    bool is_table_internal() const {
        if (!style || kind == Kind::Text) return false;
        switch (style->display) {
        case Display::TableRowGroup: case Display::TableHeaderGroup: case Display::TableFooterGroup: case Display::TableRow:
        case Display::TableColumnGroup: case Display::TableColumn: case Display::TableCell: case Display::TableCaption: return true;
        default: return false;
        }
    }
    Box* add(std::unique_ptr<Box> b) {
        b->parent = this;
        children.push_back(std::move(b));
        return children.back().get();
    }
};

// Indented text dump of a box subtree, for tests and tools.
std::string dump_box(const Box& b, int depth = 0);

} // namespace quill::engine
