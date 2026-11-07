// The engine's own computed-style record. libcss types stop at the style extraction boundary
// (boxbuilder.cpp); layout and paint only ever see this plain struct. Lengths are already resolved
// to px except percentages, which stay relative until layout knows the containing block.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quill {
struct Image;
}

namespace quill::engine {

enum class Display : uint8_t {
    None, Inline, Block, ListItem, InlineBlock, Table, InlineTable, TableRowGroup, TableHeaderGroup,
    TableFooterGroup, TableRow, TableColumnGroup, TableColumn, TableCell, TableCaption, Flex, InlineFlex, Grid, InlineGrid,
};
enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
enum class Float : uint8_t { None, Left, Right };
enum class Clear : uint8_t { None, Left, Right, Both };
enum class Visibility : uint8_t { Visible, Hidden, Collapse };
enum class Overflow : uint8_t { Visible, Hidden, Scroll, Auto };
enum class BoxSizing : uint8_t { ContentBox, BorderBox };
enum class BorderStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };
enum class TextAlign : uint8_t { Start, Left, Right, Center, Justify };
enum class WhiteSpace : uint8_t { Normal, Pre, NoWrap, PreWrap, PreLine };
enum class TextTransform : uint8_t { None, Capitalize, Uppercase, Lowercase };
enum class VAlign : uint8_t { Baseline, Sub, Super, Top, TextTop, Middle, Bottom, TextBottom, Length };
enum class ListType : uint8_t { Disc, Circle, Square, Decimal, DecimalLeadingZero, LowerRoman, UpperRoman, LowerAlpha, UpperAlpha, LowerGreek, None };
enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap : uint8_t { NoWrap, Wrap, WrapReverse };
enum class Justify : uint8_t { FlexStart, FlexEnd, Center, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align : uint8_t { Auto, Stretch, FlexStart, FlexEnd, Center, Baseline };
enum class BgRepeat : uint8_t { Repeat, RepeatX, RepeatY, NoRepeat };

struct Length {
    enum Kind : uint8_t { Auto, Px, Percent, None } kind = Auto;
    float v = 0;
    static Length px(float f) { return {Px, f}; }
    static Length pct(float f) { return {Percent, f}; }
    static Length none() { return {None, 0}; }
    bool is_auto() const { return kind == Auto; }
    bool is_none() const { return kind == None; }
    bool is_px() const { return kind == Px; }
    bool is_pct() const { return kind == Percent; }
    // Resolves against `base` (the percentage reference); auto/none give `fallback`.
    float resolve(float base, float fallback = 0) const { return kind == Px ? v : kind == Percent ? v * base / 100.f : fallback; }
};

using Color = uint32_t; // 0xAARRGGBB

struct Radius {
    Length x = Length::px(0), y = Length::px(0); // one corner's elliptical radii (percent = of the border box)
};
struct BoxShadow {
    bool  inset = false;
    float ox = 0, oy = 0, blur = 0, spread = 0;
    Color color = 0xff000000;
    bool  current = false; // currentcolor
};

// CSS gradients (background-image: linear-/radial-/conic-gradient, repeating-*), drawn by paint.cpp.
struct GradStop {
    Color  color   = 0;
    bool   current = false; // currentcolor
    bool   has_pos = false;
    Length pos;             // px or % of the gradient line (conic: % of a turn)
};
struct Gradient {
    uint8_t kind      = 0; // 0 linear, 1 radial, 2 conic
    bool    repeating = false;
    float   angle     = 180; // linear: CSS degrees (0 = to top, clockwise); conic: the "from" angle
    int8_t  corner_x = 0, corner_y = 0; // linear "to <corner>": -1/+1 for left/right, top/bottom (angle then depends on the box)
    bool    circle  = false;
    uint8_t size_kw = 3; // radial: 0 closest-side, 1 closest-corner, 2 farthest-side, 3 farthest-corner, 4 explicit (rx, ry)
    Length  rx, ry;
    Length  cx = Length::pct(50), cy = Length::pct(50); // radial / conic centre
    std::vector<GradStop> stops;
};

struct SizeSpec { // background-size / mask-size
    uint8_t kind = 0; // 0 auto, 1 contain, 2 cover, 3 explicit
    Length  w, h;     // explicit (auto = keep the image's ratio)
};

struct Mask { // mask-image: only the first layer; drawn as an alpha mask over the element's background colour
    std::string url;
    BgRepeat    repeat = BgRepeat::Repeat;
    uint8_t     size_kind = 0; // 0 auto, 1 contain, 2 cover, 3 explicit
    Length      w, h;          // explicit size (auto = keep ratio)
    Length      pos_x = Length::pct(0), pos_y = Length::pct(0);
};

// ---- CSS grid (parsed by grid.cpp from the raw declarations elemcss cascades; laid out in layout.cpp)
struct GBreadth { uint8_t k = 0; float v = 0; }; // 0 auto, 1 px, 2 pct, 3 fr, 4 min-content, 5 max-content
struct GTrack { GBreadth min, max; float fit = -1; }; // fit = fit-content() limit in px
struct GLine { uint8_t k = 0; int n = 0; std::string name; }; // 0 auto, 1 line n, 2 span n, 3 named line (n = occurrence)
struct GTrackList {
    std::vector<GTrack> tracks;                      // explicit tracks before / after an auto-repeat
    std::vector<std::vector<std::string>> names;     // line names (size tracks+1, or empty)
    bool   auto_repeat = false, auto_fit = false;
    size_t auto_at = 0;                              // tracks[0..auto_at) come before the repeated tracks
    std::vector<GTrack> rep;
};
// alignment values: 0 normal/auto, 1 start, 2 end, 3 center, 4 stretch, 5 baseline; content: 5 space-between, 6 space-around, 7 space-evenly
struct GridContainer {
    GTrackList cols, rows;
    std::vector<GTrack> auto_cols, auto_rows;
    std::vector<std::vector<std::string>> areas;
    bool    flow_col = false, dense = false;
    uint8_t justify_items = 0, align_items = 0, justify_content = 0, align_content = 0;
};
struct GridItem {
    GLine       cs, ce, rs, re; // column-start/end, row-start/end
    std::string area;           // grid-area: <ident>
    uint8_t     justify_self = 0, align_self = 0;
};

struct Style {
    Display    display  = Display::Inline;
    Position   position = Position::Static;
    Float      float_   = Float::None;
    Clear      clear    = Clear::None;
    Visibility visibility = Visibility::Visible;
    Overflow   overflow_x = Overflow::Visible, overflow_y = Overflow::Visible;
    BoxSizing  box_sizing = BoxSizing::ContentBox;

    Length width, height, min_width, min_height = Length::px(0), max_width = Length::none(), max_height = Length::none();
    bool   min_height_auto = true; // min-height: auto (min_height then reads 0): flex items get their automatic minimum size
    Length margin[4]  = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)}; // top right bottom left
    Length padding[4] = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)};
    Length inset[4];                                                                  // top right bottom left (auto = unset)
    float       border_width[4] = {0, 0, 0, 0};
    BorderStyle border_style[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
    Color       border_color[4] = {0xff000000, 0xff000000, 0xff000000, 0xff000000};

    Color       color = 0xff000000;
    Color       background_color = 0; // transparent
    std::string background_image;     // absolute-ish url as written; empty = none
    BgRepeat    background_repeat = BgRepeat::Repeat;
    Length      background_pos_x = Length::pct(0), background_pos_y = Length::pct(0);
    std::vector<Gradient> gradients;      // background-image gradients, first listed = top layer
    bool        gradient_over_image = false; // a gradient is listed before the url() layer (drawn above it)
    mutable std::vector<std::shared_ptr<quill::Image>> grad_cache; // rendered tiles, one per gradient (paint.cpp)
    mutable int grad_cw = 0, grad_ch = 0;

    float                    font_size = 16;
    int                      font_weight = 400;
    bool                     italic = false;
    std::vector<std::string> font_family;
    uint64_t                 font_scope = 0; // the page's web fonts (FontManager scope), 0 = system fonts only
    float                    line_height = -1; // px; < 0 = "normal"
    float                    letter_spacing = 0, word_spacing = 0;
    Length                   text_indent = Length::px(0);
    TextAlign                text_align = TextAlign::Start;
    // HTML align=left/center/right and <center> (libcss's -libcss-left/center/right text-align values) also move child
    // blocks: legacy_align is this element's own value, parent_align its parent's (0 none, 1 left, 2 center, 3 right).
    uint8_t                  legacy_align = 0, parent_align = 0;
    uint8_t                  text_decoration = 0; // bit0 underline, bit1 overline, bit2 line-through, bit3 blink
    WhiteSpace               white_space = WhiteSpace::Normal;
    TextTransform            text_transform = TextTransform::None;
    VAlign                   vertical_align = VAlign::Baseline;
    float                    vertical_align_len = 0;
    bool                     rtl = false;

    ListType list_type = ListType::Disc;
    bool     list_inside = false;

    Radius                 radius[4]; // top-left, top-right, bottom-right, bottom-left
    std::vector<BoxShadow> shadows;
    float                  tr_x_px = 0, tr_x_pct = 0, tr_y_px = 0, tr_y_pct = 0; // CSS translate() as a visual offset
    // the rest of transform (rotate / scale / skew / matrix): its linear part, applied around transform-origin when
    // painting (x' = a x + c y, y' = b x + d y). Makes a stacking context, like opacity.
    bool                   has_xform = false;
    float                  xf[4] = {1, 0, 0, 1};
    Length                 xf_origin_x = Length::pct(50), xf_origin_y = Length::pct(50);
    Mask                   mask;
    SizeSpec               bg_size;

    uint8_t cursor = 0; // 0 auto, 1 pointer, 2 text, 3 default
    // <marquee>: 0 not one, 1 scroll, 2 slide, 3 alternate; direction 0 left, 1 right, 2 up, 3 down. The content
    // moves `amount` px every `delay` ms, `loop` times (-1 forever). Applied at paint time (and in hit testing).
    uint8_t marquee = 0, marquee_dir = 0;
    int16_t marquee_amount = 6, marquee_delay = 85, marquee_loop = -1;
    bool    appearance_none = false;
    bool    overflow_to_viewport = false; // root / body whose overflow went to the viewport (now visible here) // appearance: none: form controls drop their built-in decorations (select arrow)

    // font_for() memo: the font picked for this style, valid while the font properties it was picked for are unchanged
    // (a copied-then-edited Style has a different family buffer or size, so it never reuses a wrong font).
    mutable void*       font_memo      = nullptr;
    mutable const void* font_memo_fam  = nullptr;
    mutable size_t      font_memo_n    = 0;
    mutable float       font_memo_size = -1;
    mutable int         font_memo_wt   = -1;
    mutable bool        font_memo_it   = false;
    mutable uint64_t    font_memo_gen  = 0; // FontManager::generation() when picked (a web font arrived since: pick again)
    float opacity = 1;
    int   z_index = 0;
    bool  z_auto = true;

    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap      flex_wrap = FlexWrap::NoWrap;
    float         flex_grow = 0, flex_shrink = 1;
    Length        flex_basis; // auto
    Justify       justify_content = Justify::FlexStart;
    Align         align_items = Align::Stretch, align_self = Align::Auto, align_content = Align::Stretch;
    int           order = 0;
    Length        column_gap = Length::px(0), row_gap = Length::px(0);
    std::shared_ptr<const GridContainer> grid; // grid container properties (null = all initial)
    std::shared_ptr<const GridItem>      gitem; // grid item placement / alignment (null = all initial)

    bool  border_collapse = false;
    float border_spacing_h = 0, border_spacing_v = 0;
    bool  table_fixed = false;
    bool  caption_bottom = false;

    // Returns true for display types that make a block-level box in normal flow.
    bool block_level() const {
        switch (display) {
        case Display::Block: case Display::ListItem: case Display::Table: case Display::Flex: case Display::Grid: return true;
        default: return false;
        }
    }
    bool inline_level() const { return display == Display::Inline || display == Display::InlineBlock || display == Display::InlineTable || display == Display::InlineFlex || display == Display::InlineGrid; }
    bool collapses_whitespace() const { return white_space == WhiteSpace::Normal || white_space == WhiteSpace::NoWrap || white_space == WhiteSpace::PreLine; }
};

const char* display_name(Display d);

} // namespace quill::engine
