// Box generation. See box.h for the rules implemented.
#include "boxbuilder.h"
#include "grid.h"
#include "cssvars.h"
#include "image.h"

#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <map>

namespace quill::engine {

using namespace domutil;

const char* display_name(Display d) {
    switch (d) {
    case Display::None: return "none";
    case Display::Inline: return "inline";
    case Display::Block: return "block";
    case Display::ListItem: return "list-item";
    case Display::InlineBlock: return "inline-block";
    case Display::Table: return "table";
    case Display::InlineTable: return "inline-table";
    case Display::TableRowGroup: return "table-row-group";
    case Display::TableHeaderGroup: return "table-header-group";
    case Display::TableFooterGroup: return "table-footer-group";
    case Display::TableRow: return "table-row";
    case Display::TableColumnGroup: return "table-column-group";
    case Display::TableColumn: return "table-column";
    case Display::TableCell: return "table-cell";
    case Display::TableCaption: return "table-caption";
    case Display::Flex: return "flex";
    case Display::InlineFlex: return "inline-flex";
    case Display::Grid: return "grid";
    case Display::InlineGrid: return "inline-grid";
    }
    return "?";
}

namespace {

// ------------------------------------------------------------------ style extraction

struct Extract {
    const css_computed_style* s;
    const css_unit_ctx&       uc;
    float                     zoom = 1; // page zoom: CSS px -> layout px

    float px(css_fixed l, css_unit u) const { return FIXTOFLT(css_unit_len2css_px(s, &uc, l, u)) * zoom; }
    Length len(css_fixed l, css_unit u) const { return u == CSS_UNIT_PCT ? Length::pct(FIXTOFLT(l)) : Length::px(px(l, u)); }
};

Display map_display(uint8_t d) {
    switch (d) {
    case CSS_DISPLAY_INLINE: return Display::Inline;
    case CSS_DISPLAY_BLOCK: case CSS_DISPLAY_RUN_IN: return Display::Block;
    case CSS_DISPLAY_GRID: return Display::Grid;
    case CSS_DISPLAY_LIST_ITEM: return Display::ListItem;
    case CSS_DISPLAY_INLINE_BLOCK: return Display::InlineBlock;
    case CSS_DISPLAY_INLINE_GRID: return Display::InlineGrid;
    case CSS_DISPLAY_TABLE: return Display::Table;
    case CSS_DISPLAY_INLINE_TABLE: return Display::InlineTable;
    case CSS_DISPLAY_TABLE_ROW_GROUP: return Display::TableRowGroup;
    case CSS_DISPLAY_TABLE_HEADER_GROUP: return Display::TableHeaderGroup;
    case CSS_DISPLAY_TABLE_FOOTER_GROUP: return Display::TableFooterGroup;
    case CSS_DISPLAY_TABLE_ROW: return Display::TableRow;
    case CSS_DISPLAY_TABLE_COLUMN_GROUP: return Display::TableColumnGroup;
    case CSS_DISPLAY_TABLE_COLUMN: return Display::TableColumn;
    case CSS_DISPLAY_TABLE_CELL: return Display::TableCell;
    case CSS_DISPLAY_TABLE_CAPTION: return Display::TableCaption;
    case CSS_DISPLAY_NONE: return Display::None;
    case CSS_DISPLAY_FLEX: return Display::Flex;
    case CSS_DISPLAY_INLINE_FLEX: return Display::InlineFlex;
    }
    return Display::Inline;
}

BorderStyle map_border_style(uint8_t b) {
    switch (b) {
    case CSS_BORDER_STYLE_HIDDEN: return BorderStyle::Hidden;
    case CSS_BORDER_STYLE_DOTTED: return BorderStyle::Dotted;
    case CSS_BORDER_STYLE_DASHED: return BorderStyle::Dashed;
    case CSS_BORDER_STYLE_SOLID: return BorderStyle::Solid;
    case CSS_BORDER_STYLE_DOUBLE: return BorderStyle::Double;
    case CSS_BORDER_STYLE_GROOVE: return BorderStyle::Groove;
    case CSS_BORDER_STYLE_RIDGE: return BorderStyle::Ridge;
    case CSS_BORDER_STYLE_INSET: return BorderStyle::Inset;
    case CSS_BORDER_STYLE_OUTSET: return BorderStyle::Outset;
    }
    return BorderStyle::None;
}

Align map_align(uint8_t a) {
    switch (a) {
    case CSS_ALIGN_ITEMS_STRETCH: return Align::Stretch;
    case CSS_ALIGN_ITEMS_FLEX_START: return Align::FlexStart;
    case CSS_ALIGN_ITEMS_FLEX_END: return Align::FlexEnd;
    case CSS_ALIGN_ITEMS_CENTER: return Align::Center;
    case CSS_ALIGN_ITEMS_BASELINE: return Align::Baseline;
    }
    return Align::Auto;
}

static uint8_t legacy_align_of(const css_computed_style* s) {
    switch (css_computed_text_align(s)) {
    case CSS_TEXT_ALIGN_LIBCSS_LEFT: return 1;
    case CSS_TEXT_ALIGN_LIBCSS_CENTER: return 2;
    case CSS_TEXT_ALIGN_LIBCSS_RIGHT: return 3;
    default: return 0;
    }
}

Style extract_style(const css_computed_style* s, bool is_root, const css_unit_ctx& uc, float zoom) {
    Extract x{s, uc, zoom};
    Style   st;
    css_fixed l = 0, l2 = 0;
    css_unit  u = CSS_UNIT_PX, u2 = CSS_UNIT_PX;
    css_color c = 0;

    st.display    = map_display(css_computed_display(s, is_root));
    uint8_t pos   = css_computed_position(s);
    st.position   = pos == CSS_POSITION_RELATIVE ? Position::Relative : pos == CSS_POSITION_ABSOLUTE ? Position::Absolute
                  : pos == CSS_POSITION_FIXED ? Position::Fixed : pos == CSS_POSITION_STICKY ? Position::Sticky : Position::Static;
    uint8_t fl    = css_computed_float(s);
    st.float_     = fl == CSS_FLOAT_LEFT ? Float::Left : fl == CSS_FLOAT_RIGHT ? Float::Right : Float::None;
    uint8_t cl    = css_computed_clear(s);
    st.clear      = cl == CSS_CLEAR_LEFT ? Clear::Left : cl == CSS_CLEAR_RIGHT ? Clear::Right : cl == CSS_CLEAR_BOTH ? Clear::Both : Clear::None;
    {
        lwc_string** urls = nullptr;
        uint8_t cur = css_computed_cursor(s, &urls);
        st.cursor = cur == CSS_CURSOR_POINTER ? 1 : cur == CSS_CURSOR_TEXT ? 2 : (cur == CSS_CURSOR_DEFAULT || cur == CSS_CURSOR_WAIT || cur == CSS_CURSOR_PROGRESS) ? 3 : 0;
    }
    uint8_t vis   = css_computed_visibility(s);
    st.visibility = vis == CSS_VISIBILITY_HIDDEN ? Visibility::Hidden : vis == CSS_VISIBILITY_COLLAPSE ? Visibility::Collapse : Visibility::Visible;
    auto ov = [](uint8_t o) { return o == CSS_OVERFLOW_HIDDEN ? Overflow::Hidden : o == CSS_OVERFLOW_SCROLL ? Overflow::Scroll : o == CSS_OVERFLOW_AUTO ? Overflow::Auto : Overflow::Visible; };
    st.overflow_x = ov(css_computed_overflow_x(s));
    st.overflow_y = ov(css_computed_overflow_y(s));
    // CSS Overflow: if one axis isn't visible, a visible other axis computes to auto
    if (st.overflow_x == Overflow::Visible && st.overflow_y != Overflow::Visible) st.overflow_x = Overflow::Auto;
    if (st.overflow_y == Overflow::Visible && st.overflow_x != Overflow::Visible) st.overflow_y = Overflow::Auto;
    st.box_sizing = css_computed_box_sizing(s) == CSS_BOX_SIZING_BORDER_BOX ? BoxSizing::BorderBox : BoxSizing::ContentBox;

    // sizes
    if (css_computed_width(s, &l, &u) == CSS_WIDTH_SET) st.width = x.len(l, u);
    if (css_computed_height(s, &l, &u) == CSS_HEIGHT_SET) st.height = x.len(l, u);
    if (css_computed_min_width(s, &l, &u) == CSS_MIN_WIDTH_SET) st.min_width = x.len(l, u);
    if (css_computed_min_height(s, &l, &u) == CSS_MIN_HEIGHT_SET) { st.min_height = x.len(l, u); st.min_height_auto = false; }
    if (css_computed_max_width(s, &l, &u) == CSS_MAX_WIDTH_SET) st.max_width = x.len(l, u);
    if (css_computed_max_height(s, &l, &u) == CSS_MAX_HEIGHT_SET) st.max_height = x.len(l, u);

    typedef uint8_t (*LenGetter)(const css_computed_style*, css_fixed*, css_unit*);
    LenGetter margins[4] = {css_computed_margin_top, css_computed_margin_right, css_computed_margin_bottom, css_computed_margin_left};
    LenGetter paddings[4] = {css_computed_padding_top, css_computed_padding_right, css_computed_padding_bottom, css_computed_padding_left};
    LenGetter insets[4]   = {css_computed_top, css_computed_right, css_computed_bottom, css_computed_left};
    for (int i = 0; i < 4; ++i) {
        uint8_t r = margins[i](s, &l, &u);
        st.margin[i] = r == CSS_MARGIN_SET ? x.len(l, u) : r == CSS_MARGIN_AUTO ? Length{} : Length::px(0);
        r = paddings[i](s, &l, &u);
        st.padding[i] = r == CSS_PADDING_SET ? x.len(l, u) : Length::px(0);
        r = insets[i](s, &l, &u);
        st.inset[i] = r == 1 /* SET */ ? x.len(l, u) : Length{};
    }

    // borders
    typedef uint8_t (*StyleGetter)(const css_computed_style*);
    typedef uint8_t (*WidthGetter)(const css_computed_style*, css_fixed*, css_unit*);
    typedef uint8_t (*ColorGetter)(const css_computed_style*, css_color*);
    StyleGetter bs[4] = {css_computed_border_top_style, css_computed_border_right_style, css_computed_border_bottom_style, css_computed_border_left_style};
    WidthGetter bw[4] = {css_computed_border_top_width, css_computed_border_right_width, css_computed_border_bottom_width, css_computed_border_left_width};
    ColorGetter bc[4] = {css_computed_border_top_color, css_computed_border_right_color, css_computed_border_bottom_color, css_computed_border_left_color};
    css_computed_color(s, &c);
    st.color = c;
    for (int i = 0; i < 4; ++i) {
        st.border_style[i] = map_border_style(bs[i](s));
        uint8_t wr = bw[i](s, &l, &u);
        float   w  = wr == CSS_BORDER_WIDTH_THIN ? zoom : wr == CSS_BORDER_WIDTH_MEDIUM ? 3 * zoom : wr == CSS_BORDER_WIDTH_THICK ? 5 * zoom
                   : wr == CSS_BORDER_WIDTH_WIDTH ? x.px(l, u) : 0;
        if (st.border_style[i] == BorderStyle::None || st.border_style[i] == BorderStyle::Hidden) w = 0;
        if (zoom != 1.f && w > 0) w = std::max(1.f, std::floor(w + 0.05f)); // zoomed: whole pixels, at least one (as browsers)
        st.border_width[i] = w;
        css_color bcol = 0;
        uint8_t   cr   = bc[i](s, &bcol);
        st.border_color[i] = cr == CSS_BORDER_COLOR_CURRENT_COLOR ? st.color : bcol;
    }

    // background
    uint8_t bgr = css_computed_background_color(s, &c);
    st.background_color = bgr == CSS_BACKGROUND_COLOR_COLOR ? c : bgr == CSS_BACKGROUND_COLOR_CURRENT_COLOR ? st.color : 0;
    lwc_string* url = nullptr;
    if (css_computed_background_image(s, &url) == CSS_BACKGROUND_IMAGE_IMAGE && url) st.background_image = lwcs(url);
    switch (css_computed_background_repeat(s)) {
    case CSS_BACKGROUND_REPEAT_REPEAT_X: st.background_repeat = BgRepeat::RepeatX; break;
    case CSS_BACKGROUND_REPEAT_REPEAT_Y: st.background_repeat = BgRepeat::RepeatY; break;
    case CSS_BACKGROUND_REPEAT_NO_REPEAT: st.background_repeat = BgRepeat::NoRepeat; break;
    default: break;
    }
    if (css_computed_background_position(s, &l, &u, &l2, &u2) == CSS_BACKGROUND_POSITION_SET) {
        st.background_pos_x = x.len(l, u);
        st.background_pos_y = x.len(l2, u2);
    }

    // text / font
    css_computed_font_size(s, &l, &u);
    st.font_size = x.px(l, u);
    switch (css_computed_font_weight(s)) {
    case CSS_FONT_WEIGHT_BOLD: case CSS_FONT_WEIGHT_700: st.font_weight = 700; break;
    case CSS_FONT_WEIGHT_BOLDER: case CSS_FONT_WEIGHT_800: st.font_weight = 800; break;
    case CSS_FONT_WEIGHT_900: st.font_weight = 900; break;
    case CSS_FONT_WEIGHT_LIGHTER: case CSS_FONT_WEIGHT_300: st.font_weight = 300; break;
    case CSS_FONT_WEIGHT_100: st.font_weight = 100; break;
    case CSS_FONT_WEIGHT_200: st.font_weight = 200; break;
    case CSS_FONT_WEIGHT_500: st.font_weight = 500; break;
    case CSS_FONT_WEIGHT_600: st.font_weight = 600; break;
    default: st.font_weight = 400; break;
    }
    st.italic = css_computed_font_style(s) != CSS_FONT_STYLE_NORMAL;
    lwc_string** names = nullptr;
    uint8_t      fam   = css_computed_font_family(s, &names);
    if (names)
        for (lwc_string** n = names; *n; ++n) st.font_family.push_back(lwcs(*n));
    switch (fam) {
    case CSS_FONT_FAMILY_SERIF: st.font_family.push_back("serif"); break;
    case CSS_FONT_FAMILY_SANS_SERIF: st.font_family.push_back("sans-serif"); break;
    case CSS_FONT_FAMILY_MONOSPACE: st.font_family.push_back("monospace"); break;
    case CSS_FONT_FAMILY_CURSIVE: st.font_family.push_back("cursive"); break;
    case CSS_FONT_FAMILY_FANTASY: st.font_family.push_back("fantasy"); break;
    default: break;
    }
    uint8_t lhr = css_computed_line_height(s, &l, &u);
    st.line_height = lhr == CSS_LINE_HEIGHT_NUMBER ? FIXTOFLT(l) * st.font_size : lhr == CSS_LINE_HEIGHT_DIMENSION
                         ? (u == CSS_UNIT_PCT ? FIXTOFLT(l) * st.font_size / 100.f : x.px(l, u)) : -1.f;
    if (css_computed_letter_spacing(s, &l, &u) == CSS_LETTER_SPACING_SET) st.letter_spacing = x.px(l, u);
    if (css_computed_word_spacing(s, &l, &u) == CSS_WORD_SPACING_SET) st.word_spacing = x.px(l, u);
    if (css_computed_text_indent(s, &l, &u) == CSS_TEXT_INDENT_SET) st.text_indent = x.len(l, u);
    switch (css_computed_text_align(s)) {
    case CSS_TEXT_ALIGN_LEFT: case CSS_TEXT_ALIGN_LIBCSS_LEFT: st.text_align = TextAlign::Left; break;
    case CSS_TEXT_ALIGN_RIGHT: case CSS_TEXT_ALIGN_LIBCSS_RIGHT: st.text_align = TextAlign::Right; break;
    case CSS_TEXT_ALIGN_CENTER: case CSS_TEXT_ALIGN_LIBCSS_CENTER: st.text_align = TextAlign::Center; break;
    case CSS_TEXT_ALIGN_JUSTIFY: st.text_align = TextAlign::Justify; break;
    default: st.text_align = TextAlign::Start; break;
    }
    st.legacy_align = legacy_align_of(s);
    uint8_t td = css_computed_text_decoration(s);
    st.text_decoration = static_cast<uint8_t>(((td & CSS_TEXT_DECORATION_UNDERLINE) ? 1 : 0) | ((td & CSS_TEXT_DECORATION_OVERLINE) ? 2 : 0) |
                                              ((td & CSS_TEXT_DECORATION_LINE_THROUGH) ? 4 : 0) | ((td & CSS_TEXT_DECORATION_BLINK) ? 8 : 0));
    switch (css_computed_white_space(s)) {
    case CSS_WHITE_SPACE_PRE: st.white_space = WhiteSpace::Pre; break;
    case CSS_WHITE_SPACE_NOWRAP: st.white_space = WhiteSpace::NoWrap; break;
    case CSS_WHITE_SPACE_PRE_WRAP: st.white_space = WhiteSpace::PreWrap; break;
    case CSS_WHITE_SPACE_PRE_LINE: st.white_space = WhiteSpace::PreLine; break;
    default: break;
    }
    switch (css_computed_text_transform(s)) {
    case CSS_TEXT_TRANSFORM_CAPITALIZE: st.text_transform = TextTransform::Capitalize; break;
    case CSS_TEXT_TRANSFORM_UPPERCASE: st.text_transform = TextTransform::Uppercase; break;
    case CSS_TEXT_TRANSFORM_LOWERCASE: st.text_transform = TextTransform::Lowercase; break;
    default: break;
    }
    switch (css_computed_vertical_align(s, &l, &u)) {
    case CSS_VERTICAL_ALIGN_SUB: st.vertical_align = VAlign::Sub; break;
    case CSS_VERTICAL_ALIGN_SUPER: st.vertical_align = VAlign::Super; break;
    case CSS_VERTICAL_ALIGN_TOP: st.vertical_align = VAlign::Top; break;
    case CSS_VERTICAL_ALIGN_TEXT_TOP: st.vertical_align = VAlign::TextTop; break;
    case CSS_VERTICAL_ALIGN_MIDDLE: st.vertical_align = VAlign::Middle; break;
    case CSS_VERTICAL_ALIGN_BOTTOM: st.vertical_align = VAlign::Bottom; break;
    case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: st.vertical_align = VAlign::TextBottom; break;
    case CSS_VERTICAL_ALIGN_SET:
        st.vertical_align     = VAlign::Length;
        st.vertical_align_len = u == CSS_UNIT_PCT ? FIXTOFLT(l) * (st.line_height > 0 ? st.line_height : st.font_size * 1.2f) / 100.f : x.px(l, u);
        break;
    default: break;
    }
    st.rtl = css_computed_direction(s) == CSS_DIRECTION_RTL;

    switch (css_computed_list_style_type(s)) {
    case CSS_LIST_STYLE_TYPE_CIRCLE: st.list_type = ListType::Circle; break;
    case CSS_LIST_STYLE_TYPE_SQUARE: st.list_type = ListType::Square; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL: st.list_type = ListType::Decimal; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO: st.list_type = ListType::DecimalLeadingZero; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ROMAN: st.list_type = ListType::LowerRoman; break;
    case CSS_LIST_STYLE_TYPE_UPPER_ROMAN: st.list_type = ListType::UpperRoman; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ALPHA: case CSS_LIST_STYLE_TYPE_LOWER_LATIN: st.list_type = ListType::LowerAlpha; break;
    case CSS_LIST_STYLE_TYPE_UPPER_ALPHA: case CSS_LIST_STYLE_TYPE_UPPER_LATIN: st.list_type = ListType::UpperAlpha; break;
    case CSS_LIST_STYLE_TYPE_LOWER_GREEK: st.list_type = ListType::LowerGreek; break;
    case CSS_LIST_STYLE_TYPE_NONE: st.list_type = ListType::None; break;
    case CSS_LIST_STYLE_TYPE_DISC: break;
    default: st.list_type = ListType::Decimal; break;
    }
    st.list_inside = css_computed_list_style_position(s) == CSS_LIST_STYLE_POSITION_INSIDE;

    css_computed_opacity(s, &l);
    st.opacity = FIXTOFLT(l);
    int32_t z = 0;
    if (css_computed_z_index(s, &z) == CSS_Z_INDEX_SET) { st.z_auto = false; st.z_index = z; }

    // flex
    switch (css_computed_flex_direction(s)) {
    case CSS_FLEX_DIRECTION_ROW_REVERSE: st.flex_direction = FlexDirection::RowReverse; break;
    case CSS_FLEX_DIRECTION_COLUMN: st.flex_direction = FlexDirection::Column; break;
    case CSS_FLEX_DIRECTION_COLUMN_REVERSE: st.flex_direction = FlexDirection::ColumnReverse; break;
    default: break;
    }
    uint8_t fw = css_computed_flex_wrap(s);
    st.flex_wrap = fw == CSS_FLEX_WRAP_WRAP ? FlexWrap::Wrap : fw == CSS_FLEX_WRAP_WRAP_REVERSE ? FlexWrap::WrapReverse : FlexWrap::NoWrap;
    css_computed_flex_grow(s, &l);
    st.flex_grow = FIXTOFLT(l);
    css_computed_flex_shrink(s, &l);
    st.flex_shrink = FIXTOFLT(l);
    if (css_computed_flex_basis(s, &l, &u) == CSS_FLEX_BASIS_SET) st.flex_basis = x.len(l, u);
    switch (css_computed_justify_content(s)) {
    case CSS_JUSTIFY_CONTENT_FLEX_END: st.justify_content = Justify::FlexEnd; break;
    case CSS_JUSTIFY_CONTENT_CENTER: st.justify_content = Justify::Center; break;
    case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: st.justify_content = Justify::SpaceBetween; break;
    case CSS_JUSTIFY_CONTENT_SPACE_AROUND: st.justify_content = Justify::SpaceAround; break;
    case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: st.justify_content = Justify::SpaceEvenly; break;
    default: break;
    }
    st.align_items   = map_align(css_computed_align_items(s));
    st.align_self    = map_align(css_computed_align_self(s));
    st.align_content = map_align(css_computed_align_content(s));
    int32_t ord = 0;
    css_computed_order(s, &ord);
    st.order = ord;
    if (css_computed_column_gap(s, &l, &u) == CSS_COLUMN_GAP_SET) st.column_gap = x.len(l, u);
    if (css_computed_row_gap(s, &l, &u) == CSS_ROW_GAP_SET) st.row_gap = x.len(l, u);

    // tables
    st.border_collapse = css_computed_border_collapse(s) == CSS_BORDER_COLLAPSE_COLLAPSE;
    if (css_computed_border_spacing(s, &l, &u, &l2, &u2) == CSS_BORDER_SPACING_SET) {
        st.border_spacing_h = x.px(l, u);
        st.border_spacing_v = x.px(l2, u2);
    }
    st.table_fixed    = css_computed_table_layout(s) == CSS_TABLE_LAYOUT_FIXED;
    st.caption_bottom = css_computed_caption_side(s) == CSS_CAPTION_SIDE_BOTTOM;
    return st;
}

// Style for a generated (anonymous) box: inherited properties from `p`, initial values for the rest.
std::shared_ptr<const Style> anonymous_style(const Style& p, Display d) {
    auto st              = std::make_shared<Style>();
    st->display          = d;
    st->color            = p.color;
    st->font_size        = p.font_size;
    st->font_weight      = p.font_weight;
    st->italic           = p.italic;
    st->font_family      = p.font_family;
    st->font_scope       = p.font_scope;
    st->line_height      = p.line_height;
    st->letter_spacing   = p.letter_spacing;
    st->word_spacing     = p.word_spacing;
    st->text_indent      = p.text_indent;
    st->text_align       = p.text_align;
    st->legacy_align     = p.legacy_align;
    st->parent_align     = p.legacy_align;
    st->white_space      = p.white_space;
    st->text_transform   = p.text_transform;
    st->list_type        = p.list_type;
    st->list_inside      = p.list_inside;
    st->rtl              = p.rtl;
    st->visibility       = p.visibility;
    st->border_collapse  = p.border_collapse;
    st->border_spacing_h = p.border_spacing_h;
    st->border_spacing_v = p.border_spacing_v;
    return st;
}

// ------------------------------------------------------------------ helpers

bool all_space(const std::string& s) {
    for (char c : s)
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')) return false;
    return true;
}

bool collapsible_blank(const Box& b) { return b.kind == Box::Kind::Text && b.style && b.style->collapses_whitespace() && all_space(b.text); }

// In normal flow and block-level: the boxes that force anonymous block wrapping / split inlines.
bool flow_block(const Box& b) {
    if (!b.is_block_level()) return false;
    const Style& s = *b.style;
    return s.float_ == Float::None && s.position != Position::Absolute && s.position != Position::Fixed;
}
bool out_of_flow(const Box& b) {
    if (b.kind == Box::Kind::Text || !b.style) return false;
    const Style& s = *b.style;
    return s.float_ != Float::None || s.position == Position::Absolute || s.position == Position::Fixed;
}

std::string apply_transform(const std::string& in, TextTransform t) {
    if (t == TextTransform::None) return in;
    std::string o = in;
    bool start = true;
    for (char& c : o) {
        unsigned char u = static_cast<unsigned char>(c);
        if (t == TextTransform::Uppercase) c = static_cast<char>(std::toupper(u));
        else if (t == TextTransform::Lowercase) c = static_cast<char>(std::tolower(u));
        else if (start && std::isalpha(u)) c = static_cast<char>(std::toupper(u));
        start = std::isspace(u) != 0;
    }
    return o;
}

std::string roman(int n, bool upper) {
    if (n <= 0 || n >= 4000) return std::to_string(n);
    static const int  v[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* s[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    std::string o;
    for (int i = 0; i < 13; ++i)
        while (n >= v[i]) { o += s[i]; n -= v[i]; }
    if (upper) for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}
std::string alpha(int n, bool upper) {
    if (n <= 0) return std::to_string(n);
    std::string o;
    while (n > 0) { --n; o.insert(o.begin(), static_cast<char>((upper ? 'A' : 'a') + n % 26)); n /= 26; }
    return o;
}

std::string marker_text(ListType t, int n) {
    switch (t) {
    case ListType::Disc: return "\xE2\x80\xA2";   // •
    case ListType::Circle: return "\xE2\x97\xA6"; // ◦
    case ListType::Square: return "\xE2\x96\xAA"; // ▪
    case ListType::Decimal: return std::to_string(n) + ".";
    case ListType::DecimalLeadingZero: return (n >= 0 && n < 10 ? "0" : "") + std::to_string(n) + ".";
    case ListType::LowerRoman: return roman(n, false) + ".";
    case ListType::UpperRoman: return roman(n, true) + ".";
    case ListType::LowerAlpha: return alpha(n, false) + ".";
    case ListType::UpperAlpha: return alpha(n, true) + ".";
    case ListType::LowerGreek: {
        static const char* g[] = {"\xCE\xB1", "\xCE\xB2", "\xCE\xB3", "\xCE\xB4", "\xCE\xB5", "\xCE\xB6", "\xCE\xB7", "\xCE\xB8", "\xCE\xB9", "\xCE\xBA", "\xCE\xBB", "\xCE\xBC",
                                  "\xCE\xBD", "\xCE\xBE", "\xCE\xBF", "\xCF\x80", "\xCF\x81", "\xCF\x83", "\xCF\x84", "\xCF\x85", "\xCF\x86", "\xCF\x87", "\xCF\x88", "\xCF\x89"};
        return (n >= 1 && n <= 24 ? std::string(g[n - 1]) : std::to_string(n)) + ".";
    }
    case ListType::None: return "";
    }
    return "";
}

ReplacedKind replaced_kind(const std::string& tag) {
    if (tag == "img") return ReplacedKind::Image;
    if (tag == "input") return ReplacedKind::Input;
    if (tag == "textarea") return ReplacedKind::Textarea;
    if (tag == "select") return ReplacedKind::Select;
    if (tag == "video") return ReplacedKind::Video;
    if (tag == "canvas") return ReplacedKind::Canvas;
    if (tag == "svg") return ReplacedKind::Svg;
    if (tag == "iframe") return ReplacedKind::Iframe;
    if (tag == "object" || tag == "embed") return ReplacedKind::Object;
    if (tag == "meter") return ReplacedKind::Meter;
    if (tag == "progress") return ReplacedKind::Progress;
    return ReplacedKind::None;
}


// Serialises an inline <svg> subtree back to SVG text for the rasteriser (HTML parsing lower-cases the names
// of SVG camelCase attributes and elements; the few that matter are restored).
std::string svg_attr_name(const std::string& n) {
    static const char* fix[] = {"viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "spreadMethod", "patternUnits",
                                "patternTransform", "clipPathUnits", "markerWidth", "markerHeight", "refX", "refY", "startOffset", "textLength"};
    for (const char* f : fix) if (lower(f) == n) return f;
    return n;
}
std::string svg_tag_name(const std::string& n) {
    static const char* fix[] = {"linearGradient", "radialGradient", "clipPath", "textPath", "foreignObject", "feGaussianBlur"};
    for (const char* f : fix) if (lower(f) == n) return f;
    return n;
}
void svg_escape(std::string& out, const std::string& v) {
    for (char c : v) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
}
// Writes an inline <svg> subtree as standalone SVG text: attribute/element case restored, the cascade's
// presentation properties (fill, stroke, ...) folded into style="", <use> expanded in place.
class SvgWriter {
public:
    SvgWriter(dom_node* origin, const ExtraLookup& ex) : origin_(origin), ex_(ex) {}
    std::string symbol_viewbox;

    void write(dom_node* el, bool root, std::string& out, int depth) {
        if (depth > 48) return;
        std::string tag = tag_of(el);
        if (tag == "script" || tag == "style" || tag == "foreignobject" || tag == "symbol" || tag == "title" || tag == "desc") return;
        auto attrs = all_attrs(el);
        if (tag == "use") { write_use(el, attrs, out, depth); return; }
        out += "<" + svg_tag_name(tag);
        if (root) out += " xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
        emit_attrs(el, attrs, out, "", "");
        out += ">";
        for (dom_node* c = first_child(el); c; c = next_sibling(c))
            if (is_element(c)) write(c, false, out, depth + 1);
        out += "</" + svg_tag_name(tag) + ">";
    }

private:
    dom_node*                                    origin_;
    const ExtraLookup&                           ex_;
    std::unordered_map<std::string, dom_node*>   ids_;
    bool                                         ids_built_ = false;
    int                                          use_depth_ = 0;

    static std::string find_attr(const std::vector<std::pair<std::string, std::string>>& a, const char* n) {
        for (auto& kv : a) if (kv.first == n) return kv.second;
        return {};
    }
    void emit_attrs(dom_node* el, const std::vector<std::pair<std::string, std::string>>& attrs, std::string& out, const std::string& only_prefix, const std::string& extra_transform) {
        (void)only_prefix;
        std::string css;
        if (ex_) if (const Extras* e = ex_(el, 0)) css = e->svg_style;
        std::string inline_style;
        for (auto& a : attrs) {
            if (a.first == "xmlns" || a.first.rfind("xmlns:", 0) == 0 || a.first == "class" || a.first.rfind("on", 0) == 0) continue;
            if (a.first == "style") { inline_style = a.second; continue; }
            if (a.first == "transform" && !extra_transform.empty()) continue;
            out += " " + svg_attr_name(a.first) + "=\"";
            svg_escape(out, a.second);
            out += "\"";
        }
        if (!extra_transform.empty()) { out += " transform=\""; svg_escape(out, extra_transform); out += "\""; }
        std::string st = css + inline_style;
        if (!st.empty()) { out += " style=\""; svg_escape(out, st); out += "\""; }
    }
    void build_ids() {
        ids_built_ = true;
        dom_node* top = origin_;
        while (dom_node* p = parent_of(top)) top = p;
        std::function<void(dom_node*)> walk = [&](dom_node* n) {
            for (dom_node* c = first_child(n); c; c = next_sibling(c)) {
                if (!is_element(c)) continue;
                std::string id;
                if (get_attr(c, "id", id) && !id.empty() && !ids_.count(id)) ids_[id] = c;
                walk(c);
            }
        };
        walk(top);
    }
    static bool parse_viewbox(const std::string& vb, float a[4]) {
        const char* p = vb.c_str();
        for (int n = 0; n < 4; ++n) {
            char* e = nullptr;
            a[n] = std::strtof(p, &e);
            if (e == p) return false;
            p = e;
            while (*p == ',' || *p == ' ') ++p;
        }
        return a[2] > 0 && a[3] > 0;
    }
    void write_use(dom_node* el, const std::vector<std::pair<std::string, std::string>>& attrs, std::string& out, int depth) {
        std::string href = find_attr(attrs, "href");
        if (href.empty()) href = find_attr(attrs, "xlink:href");
        if (href.size() < 2 || href[0] != '#' || use_depth_ > 6) return;
        if (!ids_built_) build_ids();
        auto it = ids_.find(href.substr(1));
        if (it == ids_.end()) return;
        dom_node* target = it->second;
        float x = std::strtof(find_attr(attrs, "x").c_str(), nullptr), y = std::strtof(find_attr(attrs, "y").c_str(), nullptr);
        std::string tr = find_attr(attrs, "transform");
        char buf[160];
        bool sym = tag_of(target) == "symbol";
        std::snprintf(buf, sizeof buf, " translate(%g %g)", x, y);
        tr += buf;
        if (sym) {
            std::string vb;
            get_attr(target, "viewBox", vb);
            if (vb.empty()) get_attr(target, "viewbox", vb);
            float a[4];
            if (parse_viewbox(vb, a)) {
                if (symbol_viewbox.empty()) symbol_viewbox = vb;
                std::string uw = find_attr(attrs, "width"), uh = find_attr(attrs, "height");
                if (!uw.empty() && !uh.empty() && uw.find('%') == std::string::npos && uh.find('%') == std::string::npos) {
                    float w = std::strtof(uw.c_str(), nullptr), h = std::strtof(uh.c_str(), nullptr);
                    if (w > 0 && h > 0) {
                        std::snprintf(buf, sizeof buf, " scale(%g %g) translate(%g %g)", w / a[2], h / a[3], -a[0], -a[1]);
                        tr += buf;
                    }
                }
            }
        }
        std::vector<std::pair<std::string, std::string>> gattrs;
        for (const char* n : {"fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity", "style", "stroke-linecap", "stroke-linejoin", "fill-rule"}) {
            std::string v = find_attr(attrs, n);
            if (!v.empty()) gattrs.push_back({n, v});
        }
        out += "<g";
        emit_attrs(el, gattrs, out, "", tr);
        out += ">";
        ++use_depth_;
        if (sym) {
            for (dom_node* c = first_child(target); c; c = next_sibling(c))
                if (is_element(c)) write(c, false, out, depth + 1);
        } else {
            write(target, false, out, depth + 1);
        }
        --use_depth_;
        out += "</g>";
    }
};

// ------------------------------------------------------------------ responsive images (srcset / sizes / <picture>)
std::string trim_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

struct SrcCand {
    std::string url;
    double      w = 0, x = 0; // "480w" / "2x" descriptor (0 = none)
};

// HTML's srcset parsing: URLs may contain commas (data: URIs), so a candidate ends at whitespace, not at a comma.
std::vector<SrcCand> parse_srcset(const std::string& s) {
    std::vector<SrcCand> out;
    size_t i = 0, n = s.size();
    auto   ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (i < n && out.size() < 64) {
        while (i < n && (ws(s[i]) || s[i] == ',')) ++i;
        if (i >= n) break;
        size_t st = i;
        while (i < n && !ws(s[i])) ++i;
        std::string url = s.substr(st, i - st);
        bool        ended = false;
        while (!url.empty() && url.back() == ',') { url.pop_back(); ended = true; }
        SrcCand c;
        c.url = url;
        if (!ended) { // descriptors up to the next comma outside parentheses
            std::string d;
            int         depth = 0;
            while (i < n) {
                char ch = s[i];
                if (ch == '(') depth++;
                else if (ch == ')') depth = std::max(0, depth - 1);
                else if (ch == ',' && depth == 0) { ++i; break; }
                d += ch;
                ++i;
            }
            std::istringstream in(d);
            std::string        tok;
            while (in >> tok) {
                char   u = static_cast<char>(std::tolower(static_cast<unsigned char>(tok.back())));
                double v = std::strtod(tok.c_str(), nullptr);
                if (u == 'w' && v > 0) c.w = v;
                else if (u == 'x' && v > 0) c.x = v;
            }
        }
        if (!c.url.empty()) out.push_back(c);
    }
    return out;
}

// One <length> of a sizes attribute in px (vw, vh, em, rem, px, and simple calc() sums). -1 if unusable.
double sizes_length(std::string t, double vw, double vh, double em) {
    t = trim_ws(t);
    std::string l = lower(t);
    if (l.compare(0, 5, "calc(") == 0 && l.back() == ')') {
        std::string e = l.substr(5, l.size() - 6);
        double      sum = 0;
        int         sign = 1;
        size_t      i = 0;
        while (i < e.size()) {
            while (i < e.size() && e[i] == ' ') ++i;
            if (i < e.size() && (e[i] == '+' || e[i] == '-') && (i + 1 >= e.size() || e[i + 1] == ' ')) { sign = e[i] == '-' ? -1 : 1; ++i; continue; }
            size_t j = i;
            while (j < e.size() && e[j] != ' ') ++j;
            double v = sizes_length(e.substr(i, j - i), vw, vh, em);
            if (v < -0.5) return -1;
            sum += sign * v;
            sign = 1;
            i    = j;
        }
        return sum;
    }
    char*  end = nullptr;
    double v   = std::strtod(l.c_str(), &end);
    if (end == l.c_str()) return -1;
    std::string u = end;
    if (u == "px" || (u.empty() && v == 0)) return v;
    if (u == "vw") return v * vw / 100;
    if (u == "vh") return v * vh / 100;
    if (u == "em" || u == "rem") return v * em;
    if (u == "vmin") return v * std::min(vw, vh) / 100;
    if (u == "vmax") return v * std::max(vw, vh) / 100;
    return -1;
}

// The slot width a sizes attribute picks ("(max-width: 600px) 100vw, 50vw"); 100vw without one.
double sizes_px(const std::string& sizes, double vw, double vh, const CssVars& mq) {
    std::vector<std::string> parts;
    std::string              cur;
    int                      depth = 0;
    for (char c : sizes) {
        if (c == '(') depth++;
        else if (c == ')') depth = std::max(0, depth - 1);
        if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    for (auto& p : parts) {
        std::string e = trim_ws(p);
        if (e.empty()) continue;
        std::string media, len = e;
        size_t      close = e.rfind(')');
        if (!e.empty() && e[0] != 'c' && close != std::string::npos && close + 1 < e.size() && lower(e).compare(0, 5, "calc(") != 0) {
            media = e.substr(0, close + 1);
            len   = e.substr(close + 1);
        } else if (e.find(' ') != std::string::npos && lower(e).compare(0, 5, "calc(") != 0) {
            continue; // something we can't read
        }
        if (!media.empty() && !mq.media_matches(media)) continue;
        double v = sizes_length(len, vw, vh, 16);
        if (v >= 0) return v;
    }
    return vw;
}

// The best candidate for a screen of `dpr` device pixels per CSS pixel (the page zoom): the smallest picture that is
// still at least that dense. `density` gets how many image pixels go to one CSS pixel (to size the image).
bool pick_candidate(const std::vector<SrcCand>& cands, const std::string& fallback_src, double slot, std::string& url, double& density,
                    double dpr = 1.0) {
    struct Opt { const std::string* url; double d; };
    std::vector<Opt> opts;
    bool             any_w = false;
    for (auto& c : cands) any_w |= c.w > 0;
    for (auto& c : cands) {
        if (any_w) { if (c.w > 0 && slot > 0) opts.push_back({&c.url, c.w / slot}); }
        else opts.push_back({&c.url, c.x > 0 ? c.x : 1.0});
    }
    if (!any_w && !fallback_src.empty()) {
        bool have1 = false;
        for (auto& o : opts) have1 |= std::fabs(o.d - 1.0) < 1e-6;
        if (!have1) opts.push_back({&fallback_src, 1.0});
    }
    if (opts.empty()) return false;
    const Opt* best = nullptr;
    for (auto& o : opts)
        if (o.d >= dpr - 0.001 && (!best || o.d < best->d)) best = &o;
    if (!best)
        for (auto& o : opts)
            if (!best || o.d > best->d) best = &o;
    url     = *best->url;
    density = best->d;
    return true;
}

bool image_type_supported(const std::string& type_in) {
    std::string t = lower(trim_ws(type_in));
    size_t      sc = t.find(';');
    if (sc != std::string::npos) t = trim_ws(t.substr(0, sc));
    if (t.empty()) return true;
    static const char* ok[] = {"image/png", "image/jpeg", "image/jpg", "image/pjpeg", "image/gif", "image/bmp", "image/svg+xml", "image/x-icon",
                               "image/vnd.microsoft.icon", nullptr};
    for (const char** p = ok; *p; ++p)
        if (t == *p) return true;
    return t == "image/webp" && quill::webp_supported();
}

const char* kKeepAttrs[] = {"id", "class", "href", "src", "alt", "title", "type", "value", "placeholder", "name", "checked", "disabled", "readonly",
                            "selected", "width", "height", "colspan", "rowspan", "span", "target", "rel", "for", "size", "rows", "cols", "maxlength",
                            "label", "start", "reversed", "role", "action", "method", "min", "max", "open", "viewbox", "srcdoc", "scrolling",
                            "srcset", "sizes", "usemap", "ismap", "shape", "coords", "nohref", "media", "multiple"};

// ------------------------------------------------------------------ builder

class Builder {
public:
    Builder(const StyleLookup& l, const css_unit_ctx& uc, const ExtraLookup& x, uint64_t font_scope, float zoom = 1.f)
        : lookup_(l), uc_(uc), extras_(x), font_scope_(font_scope), zoom_(zoom) {}

    std::unique_ptr<Box> build_root(dom_node* root) {
        auto b = element_box(root, true, 0);
        if (!b) return nullptr;
        normalize(b.get());
        return b;
    }
    // (restyle_in_place uses these two)
    std::shared_ptr<const Style> style_of(dom_node* n, int pseudo, bool root) { return style_for(n, pseudo, root); }
    bool                         generated_box(dom_node* el, int pseudo, std::unique_ptr<Box>& out) { return generated(el, pseudo, out); }

private:
    const StyleLookup& lookup_;
    const css_unit_ctx& uc_;
    const ExtraLookup&  extras_;
    uint64_t            font_scope_ = 0;
    float               zoom_ = 1.f;

    std::shared_ptr<const Style> style_for(dom_node* n, int pseudo, bool root, const css_computed_style** raw = nullptr) {
        const css_computed_style* cs = lookup_(n, pseudo);
        if (raw) *raw = cs;
        if (!cs) return nullptr;
        auto st = std::make_shared<Style>(extract_style(cs, root, uc_, zoom_));
        st->font_scope = font_scope_;
        if (pseudo == 0 && !root)
            if (dom_node* par = parent_element(n))
                if (const css_computed_style* ps = lookup_(par, 0)) st->parent_align = legacy_align_of(ps);
        if (extras_) {
            // generated boxes are asked for with libcss's pseudo-element ids
            int which = pseudo == 0 ? 0 : pseudo == 3 ? 1 : 2;
            if (const Extras* ex = extras_(n, which)) {
                // (lengths from the element cascade are CSS px: zoomed here, as extract_style zooms libcss's)
                const float z  = zoom_;
                auto        zl = [z](Length& l) { if (l.is_px()) l.v *= z; };
                if (ex->has_radius)
                    for (int i = 0; i < 4; ++i) {
                        st->radius[i] = ex->radius[i];
                        zl(st->radius[i].x);
                        zl(st->radius[i].y);
                    }
                if (ex->has_shadow) {
                    st->shadows = ex->shadows;
                    for (auto& sh : st->shadows) { sh.ox *= z; sh.oy *= z; sh.blur *= z; sh.spread *= z; }
                }
                if (ex->has_mask) {
                    st->mask = ex->mask;
                    zl(st->mask.w); zl(st->mask.h); zl(st->mask.pos_x); zl(st->mask.pos_y);
                }
                if (ex->has_bgsize) {
                    st->bg_size = ex->bg_size;
                    zl(st->bg_size.w); zl(st->bg_size.h);
                }
                if (ex->has_appearance) st->appearance_none = ex->appearance_none;
                if (ex->has_transform) {
                    st->tr_x_px = ex->tx_px * z + ex->tx_em * st->font_size; st->tr_x_pct = ex->tx_pct;
                    st->tr_y_px = ex->ty_px * z + ex->ty_em * st->font_size; st->tr_y_pct = ex->ty_pct;
                }
                // the linear part: rotate and scale properties first, then transform (CSS Transforms 2 order)
                float m[4] = {1, 0, 0, 1};
                auto  mulm = [&m](const float f[4]) {
                    float a = m[0] * f[0] + m[2] * f[1], b = m[1] * f[0] + m[3] * f[1];
                    float c = m[0] * f[2] + m[2] * f[3], d = m[1] * f[2] + m[3] * f[3];
                    m[0] = a; m[1] = b; m[2] = c; m[3] = d;
                };
                if (ex->has_rotate && ex->rotate_deg != 0) {
                    float r = ex->rotate_deg * 0.0174532925f, f[4] = {std::cos(r), std::sin(r), -std::sin(r), std::cos(r)};
                    mulm(f);
                }
                if (ex->has_scale) { float f[4] = {ex->scale_x, 0, 0, ex->scale_y}; mulm(f); }
                if (ex->has_xform) mulm(ex->xf);
                if (std::fabs(m[0] - 1) > 1e-4f || std::fabs(m[1]) > 1e-4f || std::fabs(m[2]) > 1e-4f || std::fabs(m[3] - 1) > 1e-4f) {
                    st->has_xform = true;
                    for (int k = 0; k < 4; k++) st->xf[k] = m[k];
                }
                if (ex->has_origin) {
                    st->xf_origin_x = ex->origin_x; st->xf_origin_y = ex->origin_y;
                    zl(st->xf_origin_x); zl(st->xf_origin_y);
                }
                if (ex->has_gradients) {
                    st->gradients = ex->gradients;
                    for (auto& g : st->gradients) {
                        zl(g.rx); zl(g.ry); zl(g.cx); zl(g.cy);
                        for (auto& sp : g.stops) {
                            if (sp.current) sp.color = st->color;
                            zl(sp.pos);
                        }
                    }
                    st->gradient_over_image = ex->gradient_over_image;
                    if (ex->gradient_hides_url) st->background_image.clear();
                }
                if (ex->has_bg_color) st->background_color = ex->bg_color_current ? st->color : ex->bg_color;
                if (!ex->grid.empty()) {
                    apply_grid_props(*st, ex->grid);
                    if (z != 1.f) zoom_grid(*st, z);
                }
            }
        }
        if (pseudo == 0 && !root && tag_of(n) == "marquee") marquee_attrs(n, *st);
        return st;
    }

    static void zoom_grid(Style& st, float z) {
        auto tr = [z](GTrack& t) {
            if (t.min.k == 1) t.min.v *= z;
            if (t.max.k == 1) t.max.v *= z;
            if (t.fit >= 0) t.fit *= z;
        };
        if (st.grid) {
            auto g = std::make_shared<GridContainer>(*st.grid);
            for (GTrackList* l : {&g->cols, &g->rows}) {
                for (auto& t : l->tracks) tr(t);
                for (auto& t : l->rep) tr(t);
            }
            for (auto& t : g->auto_cols) tr(t);
            for (auto& t : g->auto_rows) tr(t);
            st.grid = g;
        }
    }

    static void marquee_attrs(dom_node* n, Style& st) {
        std::string v;
        auto        num = [&](const char* a, int def, int lo, int hi) {
            std::string t;
            if (!get_attr(n, a, t)) return def;
            char* e = nullptr;
            long  r = std::strtol(t.c_str(), &e, 10);
            return e == t.c_str() ? def : static_cast<int>(std::max<long>(lo, std::min<long>(hi, r)));
        };
        st.marquee = 1;
        if (get_attr(n, "behavior", v)) {
            v = lower(trim_ws(v));
            st.marquee = v == "slide" ? 2 : v == "alternate" ? 3 : 1;
        }
        st.marquee_dir = 0;
        if (get_attr(n, "direction", v)) {
            v = lower(trim_ws(v));
            st.marquee_dir = v == "right" ? 1 : v == "up" ? 2 : v == "down" ? 3 : 0;
        }
        st.marquee_amount = static_cast<int16_t>(num("scrollamount", 6, 0, 2000));
        int delay = num("scrolldelay", 85, 1, 30000);
        std::string tm;
        if (delay < 60 && !get_attr(n, "truespeed", tm)) delay = 60; // as browsers: faster only with truespeed
        st.marquee_delay = static_cast<int16_t>(delay);
        int loop = num("loop", -1, -1, 30000);
        st.marquee_loop = static_cast<int16_t>(loop == 0 ? -1 : loop);
    }

    std::unique_ptr<Box> text_box(const std::string& t, const std::shared_ptr<const Style>& st) {
        auto b   = std::make_unique<Box>();
        b->kind  = Box::Kind::Text;
        b->text  = apply_transform(t, st->text_transform);
        b->style = st;
        return b;
    }

    bool generated(dom_node* el, int pseudo, std::unique_ptr<Box>& out) {
        const css_computed_style* cs = nullptr;
        auto                      st = style_for(el, pseudo, false, &cs);
        if (!st || st->display == Display::None) return false;
        const css_computed_content_item* items = nullptr;
        if (css_computed_content(cs, &items) != CSS_CONTENT_SET || !items) return false;
        std::string text;
        for (const css_computed_content_item* it = items; it->type != CSS_COMPUTED_CONTENT_NONE; ++it) {
            switch (it->type) {
            case CSS_COMPUTED_CONTENT_STRING: text += lwcs(it->data.string); break;
            case CSS_COMPUTED_CONTENT_ATTR: {
                std::string v;
                if (get_attr(el, lwcs(it->data.attr).c_str(), v)) text += v;
                break;
            }
            case CSS_COMPUTED_CONTENT_OPEN_QUOTE: text += "\xE2\x80\x9C"; break;
            case CSS_COMPUTED_CONTENT_CLOSE_QUOTE: text += "\xE2\x80\x9D"; break;
            default: break;
            }
        }
        auto b     = std::make_unique<Box>();
        b->kind    = Box::Kind::Element;
        b->pseudo  = pseudo == 3 ? Box::Pseudo::Before : Box::Pseudo::After;
        b->tag     = pseudo == 3 ? "::before" : "::after";
        b->style   = st;
        if (!text.empty()) b->add(text_box(text, st));
        out = std::move(b);
        return true;
    }

    std::unique_ptr<Box> element_box(dom_node* el, bool root, int ordinal) {
        auto st = style_for(el, 0, root);
        if (!st || st->display == Display::None) return nullptr;
        std::string tag = tag_of(el);
        auto        b   = std::make_unique<Box>();
        b->node         = el;
        b->tag          = tag;
        b->style        = st;
        // one pass over the attributes the element actually has (a DOM lookup per whitelisted name was ~10% of a load)
        for (auto& kv : all_attrs(el))
            for (const char* k : kKeepAttrs)
                if (kv.first == k) {
                    b->attrs.emplace_back(k, std::move(kv.second));
                    break;
                }
        if (tag == "br") { b->kind = Box::Kind::LineBreak; return b; }

        b->replaced = replaced_kind(tag);
        if (b->replaced == ReplacedKind::Object) embed_kind(el, tag, *b);
        if (b->replaced == ReplacedKind::Image) choose_source(el, *b);
        if (b->replaced != ReplacedKind::None) {
            if (b->replaced == ReplacedKind::Textarea) b->text = text_content(el);
            if (b->replaced == ReplacedKind::Select) {
                std::string longest;
                b->text = select_label(el, longest);
                b->attrs.emplace_back("maxlabel", longest);
            }
            if (b->replaced == ReplacedKind::Svg) {
                SvgWriter w(el, extras_);
                w.write(el, true, b->text, 0);
                std::string have;
                std::string aw, ah;
                if (!get_attr(el, "viewBox", have) && !get_attr(el, "viewbox", have) && !w.symbol_viewbox.empty() && !(get_attr(el, "width", aw) && get_attr(el, "height", ah))) {
                    // an icon made only of <use href="#symbol">: borrow the symbol's viewBox
                    size_t pos = b->text.find("<svg");
                    if (pos != std::string::npos) b->text.insert(pos + 4, " viewBox=\"" + w.symbol_viewbox + "\"");
                    b->attrs.emplace_back("viewbox", w.symbol_viewbox);
                }
            }
            return b;
        }

        if (st->display == Display::ListItem && st->list_type != ListType::None) {
            auto m          = std::make_unique<Box>();
            m->kind         = Box::Kind::Marker;
            m->style        = st;
            m->text         = marker_text(st->list_type, ordinal);
            m->list_ordinal = ordinal;
            b->add(std::move(m));
        }
        std::unique_ptr<Box> gen;
        if (generated(el, 3, gen)) b->add(std::move(gen));
        build_children(el, b.get(), tag);
        if (generated(el, 4, gen)) b->add(std::move(gen));
        return b;
    }

    // <object data type> / <embed src type>: Quill has no plugins, so what it can show is a picture (shown as an <img>)
    // or a page (shown as an <iframe>). Anything else (Flash, Java, PDF, ...) makes an <object> show its fallback
    // content (its children, as an ordinary element) and an <embed> an empty box of its size.
    void embed_kind(dom_node* el, const std::string& tag, Box& b) {
        std::string url, type;
        get_attr(el, tag == "object" ? "data" : "src", url);
        url = trim_ws(url);
        get_attr(el, "type", type);
        type = lower(trim_ws(type));
        size_t semi = type.find(';');
        if (semi != std::string::npos) type = trim_ws(type.substr(0, semi));
        std::string path = lower(url.substr(0, url.find_first_of("?#")));
        size_t      sl = path.find_last_of('/'), dot = path.find_last_of('.');
        std::string ext = dot != std::string::npos && (sl == std::string::npos || dot > sl) ? path.substr(dot + 1) : std::string();
        static const char* kImg[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "avif"};
        static const char* kDoc[] = {"html", "htm", "xhtml", "shtml", "php", "asp", "aspx", "jsp", "txt"};
        bool image = false, page = false;
        if (!type.empty()) {
            image = type.compare(0, 6, "image/") == 0 && image_type_supported(type);
            page  = type == "text/html" || type == "application/xhtml+xml" || type == "text/plain";
        } else if (!url.empty()) {
            for (const char* e : kImg) image = image || ext == e;
            for (const char* e : kDoc) page = page || ext == e;
            if (ext.empty()) page = true; // "/widget" or "page?id=3": most likely a page
        }
        if (lower(url).compare(0, 11, "data:image/") == 0) image = true, page = false;
        if (url.empty()) image = page = false;
        auto set_src = [&](const std::string& v) {
            for (auto& kv : b.attrs)
                if (kv.first == "src") { kv.second = v; return; }
            b.attrs.emplace_back("src", v);
        };
        if (image) {
            b.replaced = ReplacedKind::Image;
            set_src(url);
        } else if (page) {
            b.replaced = ReplacedKind::Iframe;
            set_src(url);
        } else if (tag == "object") {
            b.replaced = ReplacedKind::None; // the fallback: its children render instead
        }
    }

    // <img srcset sizes> and <picture><source media type srcset>: which picture this screen gets. The choice replaces
    // the box's src (everything downstream, from layout to the loader, reads src); `density` scales its size.
    void choose_source(dom_node* img, Box& b) {
        double vw = FIXTOFLT(uc_.viewport_width), vh = FIXTOFLT(uc_.viewport_height);
        CssVars mq(MediaEnv{static_cast<int>(vw), static_cast<int>(vh)}, ElementView{}, ElementView{});
        std::string srcset, sizes, src;
        get_attr(img, "src", src);
        bool from_source = false;
        if (dom_node* par = parent_element(img); par && tag_of(par) == "picture") {
            for (dom_node* c = first_child(par); c && c != img; c = next_sibling(c)) {
                if (!is_element(c) || tag_of(c) != "source") continue;
                std::string ss, media, type;
                if (!get_attr(c, "srcset", ss) || trim_ws(ss).empty()) continue;
                if (get_attr(c, "media", media) && !trim_ws(media).empty() && !mq.media_matches(media)) continue;
                if (get_attr(c, "type", type) && !image_type_supported(type)) continue;
                srcset = ss;
                sizes.clear();
                get_attr(c, "sizes", sizes);
                from_source = true;
                break;
            }
        }
        if (!from_source) {
            if (!get_attr(img, "srcset", srcset) || trim_ws(srcset).empty()) return;
            get_attr(img, "sizes", sizes);
        }
        std::vector<SrcCand> cands = parse_srcset(srcset);
        double slot = sizes.empty() || lower(trim_ws(sizes)) == "auto" ? vw : sizes_px(sizes, vw, vh, mq);
        // an explicit CSS / attribute width is the slot when sizes says nothing more useful
        if (sizes.empty() && b.style && b.style->width.is_px()) slot = b.style->width.v;
        std::string url;
        double      density = 1;
        if (!pick_candidate(cands, from_source ? std::string() : src, slot, url, density, zoom_)) return;
        bool set = false;
        for (auto& kv : b.attrs)
            if (kv.first == "src") { kv.second = url; set = true; }
        if (!set) b.attrs.emplace_back("src", url);
        if (std::fabs(density - 1.0) > 1e-3) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4f", density);
            b.attrs.emplace_back("density", buf);
        }
    }

    std::string select_label(dom_node* sel, std::string& longest) {
        std::string first, chosen;
        bool        have_first = false;
        std::function<void(dom_node*)> walk = [&](dom_node* n) {
            for (dom_node* c = first_child(n); c; c = next_sibling(c)) {
                if (!is_element(c)) continue;
                std::string t = tag_of(c);
                if (t == "option") {
                    std::string v;
                    std::string label = text_content(c);
                    if (label.size() > longest.size()) longest = label;
                    if (!have_first) { first = label; have_first = true; }
                    if (get_attr(c, "selected", v) && chosen.empty()) chosen = label;
                } else if (t == "optgroup") {
                    walk(c);
                }
            }
        };
        walk(sel);
        return chosen.empty() ? first : chosen;
    }

    void build_children(dom_node* el, Box* parent, const std::string& tag) {
        // list numbering for <ol>/<ul>/<menu> children
        int next = 1, step = 1;
        if (tag == "ol") {
            std::string v;
            bool        rev = get_attr(el, "reversed", v);
            int         count = 0;
            for (dom_node* c = first_child(el); c; c = next_sibling(c))
                if (is_element(c) && tag_of(c) == "li") ++count;
            if (get_attr(el, "start", v)) next = std::atoi(v.c_str());
            else if (rev) next = count;
            if (rev) step = -1;
        }
        // <details>: only its first <summary> shows until it is open
        dom_node* summary = nullptr;
        bool      closed  = false;
        if (tag == "details") {
            std::string v;
            closed = !get_attr(el, "open", v);
            for (dom_node* c = first_child(el); c; c = next_sibling(c))
                if (is_element(c) && tag_of(c) == "summary") { summary = c; break; }
        }
        for (dom_node* c = first_child(el); c; c = next_sibling(c)) {
            if (closed && c != summary) continue;
            dom_node_type t = type_of(c);
            if (t == DOM_TEXT_NODE || t == DOM_CDATA_SECTION_NODE) {
                std::string s = text_content(c);
                if (!s.empty()) parent->add(text_box(s, parent->style));
            } else if (t == DOM_ELEMENT_NODE) {
                int ord = 0;
                if (tag_of(c) == "li") {
                    std::string v;
                    if (get_attr(c, "value", v)) next = std::atoi(v.c_str());
                    ord = next;
                    next += step;
                }
                auto b = element_box(c, false, ord);
                if (b && c == summary) disclosure_marker(*b, closed);
                if (b) parent->add(std::move(b));
            }
        }
    }

    // The triangle in front of a <summary> (pointing right while closed, down when open). Pages that style the summary
    // as something else (display:flex, list-style:none, ...) draw their own.
    void disclosure_marker(Box& b, bool closed) {
        const Style& st = *b.style;
        if (st.list_type == ListType::None || (st.display != Display::Block && st.display != Display::ListItem)) return;
        auto m = text_box(closed ? "\xE2\x96\xB8\xC2\xA0" : "\xE2\x96\xBE\xC2\xA0", b.style); // ▸ / ▾ + no-break space
        b.children.insert(b.children.begin(), std::move(m));
        b.children.front()->parent = &b;
    }

    // ---- normalization passes (bottom-up)

    std::unique_ptr<Box> make_anon(Box* parent, Display d) {
        auto a   = std::make_unique<Box>();
        a->kind  = Box::Kind::Anonymous;
        a->style = anonymous_style(*parent->style, d);
        return a;
    }

    // Moves children [from, to) of `p` into a new anonymous box of display `d` and returns its index.
    size_t wrap_run(Box* p, size_t from, size_t to, Display d) {
        auto a = make_anon(p, d);
        for (size_t i = from; i < to; ++i) a->add(std::move(p->children[i]));
        p->children.erase(p->children.begin() + static_cast<long>(from) + 1, p->children.begin() + static_cast<long>(to));
        a->parent        = p;
        p->children[from] = std::move(a);
        return from;
    }

    static std::shared_ptr<const Style> without_edges(const Style& s, bool left, bool right) {
        auto n = std::make_shared<Style>(s);
        auto clear = [&](int i) { n->margin[i] = Length::px(0); n->padding[i] = Length::px(0); n->border_width[i] = 0; };
        if (left) clear(3);
        if (right) clear(1);
        return n;
    }

    // Inline boxes that contain in-flow blocks are split around them (CSS 2.1 9.2.1.1).
    void split_inlines(Box* p) {
        for (size_t i = 0; i < p->children.size(); ++i) {
            Box* c = p->children[i].get();
            if (c->kind != Box::Kind::Element || c->replaced != ReplacedKind::None || c->style->display != Display::Inline) continue;
            bool has_block = false;
            for (auto& g : c->children)
                if (flow_block(*g)) { has_block = true; break; }
            if (!has_block) continue;

            std::unique_ptr<Box> orig = std::move(p->children[i]);
            std::vector<std::unique_ptr<Box>> parts;
            auto clone = [&](bool cont) {
                auto n          = std::make_unique<Box>();
                n->kind         = orig->kind;
                n->pseudo       = orig->pseudo;
                n->style        = orig->style;
                n->node         = orig->node;
                n->tag          = orig->tag;
                n->attrs        = orig->attrs;
                n->continuation = cont;
                return n;
            };
            auto cur = clone(false);
            std::vector<size_t> clone_idx;
            for (auto& g : orig->children) {
                if (flow_block(*g)) {
                    if (!cur->children.empty()) { clone_idx.push_back(parts.size()); parts.push_back(std::move(cur)); }
                    // remember the inline it came out of (after any inlines it was already lifted out of)
                    Box* t = g.get();
                    while (t->split_parent) t = t->split_parent.get();
                    std::shared_ptr<Box> sh = clone(false);
                    sh->parent      = p;
                    t->split_parent = sh;
                    parts.push_back(std::move(g));
                    cur = clone(true);
                } else {
                    cur->add(std::move(g));
                }
            }
            if (!cur->children.empty()) { clone_idx.push_back(parts.size()); parts.push_back(std::move(cur)); }
            // keep the left edge only on the very first fragment and the right edge only on the last
            for (size_t k = 0; k < clone_idx.size(); ++k) {
                Box* part      = parts[clone_idx[k]].get();
                bool first_frag = !part->continuation;
                bool last_frag  = k + 1 == clone_idx.size() && clone_idx[k] + 1 == parts.size();
                if (!first_frag || !last_frag) part->style = without_edges(*part->style, !first_frag, !last_frag);
            }
            for (auto& part : parts) part->parent = p;
            size_t n = parts.size();
            p->children.erase(p->children.begin() + static_cast<long>(i));
            p->children.insert(p->children.begin() + static_cast<long>(i), std::make_move_iterator(parts.begin()), std::make_move_iterator(parts.end()));
            i += n ? n - 1 : 0;
        }
    }

    void drop_blank_text(Box* p) {
        auto& ch = p->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [](const std::unique_ptr<Box>& c) { return collapsible_blank(*c); }), ch.end());
    }

    static bool is_row_group(Display d) { return d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup; }

    void fix_tables(Box* p) {
        Display pd = p->style ? p->style->display : Display::Block;
        if (p->kind == Box::Kind::Text || p->replaced != ReplacedKind::None) return;
        bool in_table_ctx = pd == Display::Table || pd == Display::InlineTable || is_row_group(pd) || pd == Display::TableRow;
        if (in_table_ctx) drop_blank_text(p);

        // 1. table-internal boxes whose parent is not a table context get an anonymous table around them
        if (!in_table_ctx && pd != Display::TableColumnGroup) {
            for (size_t i = 0; i < p->children.size();) {
                if (!p->children[i]->is_table_internal()) { ++i; continue; }
                size_t j = i + 1;
                while (j < p->children.size()) {
                    if (p->children[j]->is_table_internal() || collapsible_blank(*p->children[j])) ++j;
                    else break;
                }
                while (j > i + 1 && collapsible_blank(*p->children[j - 1])) --j; // do not swallow trailing blanks
                wrap_run(p, i, j, Display::Table);
                fix_tables(p->children[i].get());
                ++i;
            }
        }
        if (!in_table_ctx) return;

        // 2. children of table / row-group / row must be of the right type
        for (size_t i = 0; i < p->children.size();) {
            Box*    c  = p->children[i].get();
            Display cd = c->style->display;
            bool    ok;
            if (pd == Display::TableRow) ok = c->kind != Box::Kind::Text && cd == Display::TableCell;
            else if (is_row_group(pd)) ok = cd == Display::TableRow;
            else ok = cd == Display::TableRow || is_row_group(cd) || cd == Display::TableCaption || cd == Display::TableColumn || cd == Display::TableColumnGroup;
            if (c->kind == Box::Kind::Text) ok = false;
            if (ok) { ++i; continue; }
            // gather the run of misplaced children
            auto misplaced = [&](Box* b) {
                Display d = b->style->display;
                if (b->kind == Box::Kind::Text) return true;
                if (pd == Display::TableRow) return d != Display::TableCell;
                if (is_row_group(pd)) return d != Display::TableRow;
                return !(d == Display::TableRow || is_row_group(d) || d == Display::TableCaption || d == Display::TableColumn || d == Display::TableColumnGroup);
            };
            size_t j = i + 1;
            while (j < p->children.size() && misplaced(p->children[j].get())) ++j;
            if (pd == Display::TableRow) {
                wrap_run(p, i, j, Display::TableCell);
            } else {
                bool only_cells = true;
                for (size_t k = i; k < j; ++k)
                    if (p->children[k]->style->display != Display::TableCell) only_cells = false;
                if (only_cells) {
                    wrap_run(p, i, j, Display::TableRow);
                } else {
                    // cells directly inside become a row; everything else goes row > cell
                    size_t k = i;
                    while (k < j) {
                        size_t m = k;
                        bool   cell = p->children[k]->kind != Box::Kind::Text && p->children[k]->style->display == Display::TableCell;
                        while (m < j && (p->children[m]->kind != Box::Kind::Text && p->children[m]->style->display == Display::TableCell) == cell) ++m;
                        size_t idx = wrap_run(p, k, m, Display::TableRow);
                        if (!cell) {
                            Box* row = p->children[idx].get();
                            wrap_run(row, 0, row->children.size(), Display::TableCell);
                        }
                        j -= (m - k) - 1;
                        k = idx + 1;
                    }
                }
            }
            ++i;
        }
        // rows made of blank cells etc. are fine; recurse into the (possibly new) children happens in normalize()
    }

    // Flex containers: every in-flow child is a flex item; text runs become anonymous items.
    void fix_flex(Box* p) {
        Display d = p->style->display;
        if (d != Display::Flex && d != Display::InlineFlex && d != Display::Grid && d != Display::InlineGrid) return;
        if (p->replaced != ReplacedKind::None) return;
        for (size_t i = 0; i < p->children.size();) {
            Box* c = p->children[i].get();
            if (c->kind == Box::Kind::Marker) { ++i; continue; }
            if (c->kind == Box::Kind::Text || c->kind == Box::Kind::LineBreak) {
                size_t j = i + 1;
                while (j < p->children.size() && (p->children[j]->kind == Box::Kind::Text || p->children[j]->kind == Box::Kind::LineBreak)) ++j;
                bool blank = true;
                for (size_t k = i; k < j; ++k)
                    if (!collapsible_blank(*p->children[k]) && p->children[k]->kind != Box::Kind::LineBreak) blank = false;
                if (blank) {
                    p->children.erase(p->children.begin() + static_cast<long>(i), p->children.begin() + static_cast<long>(j));
                } else {
                    wrap_run(p, i, j, Display::Block);
                    ++i;
                }
                continue;
            }
            // blockify inline-level items
            Display cd = c->style->display;
            Display nd = cd == Display::Inline || cd == Display::InlineBlock ? Display::Block : cd == Display::InlineTable ? Display::Table : cd == Display::InlineFlex ? Display::Flex : cd == Display::InlineGrid ? Display::Grid : cd;
            if (nd != cd) {
                auto s     = std::make_shared<Style>(*c->style);
                s->display = nd;
                c->style   = s;
            }
            ++i;
        }
    }

    bool is_block_container(const Box& b) const {
        if (b.kind == Box::Kind::Text || b.kind == Box::Kind::LineBreak || b.kind == Box::Kind::Marker) return false;
        if (b.replaced != ReplacedKind::None || !b.style) return false;
        switch (b.style->display) {
        case Display::Block: case Display::ListItem: case Display::InlineBlock: case Display::TableCell: case Display::TableCaption: return true;
        default: return false;
        }
    }

    // A block container with in-flow block children must not also have bare inline children.
    void wrap_inline_runs(Box* p) {
        if (!is_block_container(*p)) return;
        bool has_block = false, has_inline = false;
        for (auto& c : p->children) {
            if (flow_block(*c)) has_block = true;
            else if (c->kind != Box::Kind::Marker && !out_of_flow(*c)) has_inline = true;
        }
        if (!has_block || !has_inline) return;
        for (size_t i = 0; i < p->children.size();) {
            Box* c = p->children[i].get();
            if (c->kind == Box::Kind::Marker || flow_block(*c) || out_of_flow(*c)) { ++i; continue; }
            // run of inline-level boxes; out-of-flow boxes inside the run stay with it
            size_t j = i, last_inline = i;
            while (j < p->children.size()) {
                Box* d = p->children[j].get();
                if (flow_block(*d) || d->kind == Box::Kind::Marker) break;
                if (!out_of_flow(*d)) last_inline = j + 1;
                ++j;
            }
            j = last_inline;
            bool blank = true;
            for (size_t k = i; k < j; ++k)
                if (!collapsible_blank(*p->children[k])) blank = false;
            if (blank) {
                p->children.erase(p->children.begin() + static_cast<long>(i), p->children.begin() + static_cast<long>(j));
            } else {
                wrap_run(p, i, j, Display::Block);
                ++i;
            }
        }
    }

    void normalize(Box* b) {
        for (size_t i = 0; i < b->children.size(); ++i) normalize(b->children[i].get());
        if (b->kind == Box::Kind::Text || b->kind == Box::Kind::LineBreak || b->replaced != ReplacedKind::None) return;
        split_inlines(b);
        fix_tables(b);
        fix_flex(b);
        wrap_inline_runs(b);
        // fix-ups can create new boxes (anonymous tables/rows/cells) that need their own pass
        for (auto& c : b->children)
            if (c->kind == Box::Kind::Anonymous && c->children.size() && c->style->display != Display::Block) normalize_new(c.get());
    }

    // Anonymous table parts: children are already normalized; only their table structure may be missing.
    void normalize_new(Box* b) {
        fix_tables(b);
        wrap_inline_runs(b);
        for (auto& c : b->children)
            if (c->kind == Box::Kind::Anonymous && c->style->display != Display::Block) normalize_new(c.get());
    }
};

} // namespace

std::unique_ptr<Box> build_box_tree(dom_node* root, const StyleLookup& styles, const css_unit_ctx& uc, const ExtraLookup& extras, uint64_t font_scope,
                                    float zoom) {
    Builder b(styles, uc, extras, font_scope, zoom);
    return b.build_root(root);
}

namespace {
struct InPlace {
    Builder&                                         b;
    const std::unordered_set<const void*>&           nodes;
    const std::function<bool(const Style&, const Style&)>& same;
    dom_node*                                        root_node;
    std::unordered_map<const Style*, std::shared_ptr<const Style>> map; // old style -> new
    std::unordered_map<const void*, std::shared_ptr<const Style>>  by_node;
    std::vector<std::pair<Box*, std::shared_ptr<const Style>>>     plan;

    // Plans the new style of `bx` (and below). `pnew`: the parent's new style if it changed, else null.
    bool visit(Box& bx, const Style* pold, const std::shared_ptr<const Style>& pnew, dom_node* el_ctx) {
        std::shared_ptr<const Style> mine;
        dom_node* ctx = bx.node ? static_cast<dom_node*>(bx.node) : el_ctx;
        if (bx.style) {
            auto m = map.find(bx.style.get());
            if (m != map.end()) mine = m->second;
            else if (bx.kind == Box::Kind::Element && bx.pseudo == Box::Pseudo::None && bx.node && nodes.count(bx.node)) {
                auto it = by_node.find(bx.node);
                if (it == by_node.end()) {
                    auto st = b.style_of(static_cast<dom_node*>(bx.node), 0, bx.node == root_node);
                    if (!st) return false;
                    it = by_node.emplace(bx.node, st).first;
                }
                if (!same(*bx.style, *it->second)) return false;
                mine = it->second;
            } else if (bx.pseudo != Box::Pseudo::None && el_ctx && nodes.count(el_ctx)) {
                // ::before / ::after: same style rules, and the same generated text
                std::unique_ptr<Box> fresh;
                if (!b.generated_box(el_ctx, bx.pseudo == Box::Pseudo::Before ? 3 : 4, fresh) || !fresh) return false;
                if (!same(*bx.style, *fresh->style) || fresh->children.size() != bx.children.size()) return false;
                for (size_t i = 0; i < bx.children.size(); i++)
                    if (bx.children[i]->kind != fresh->children[i]->kind || bx.children[i]->text != fresh->children[i]->text) return false;
                mine = fresh->style;
            } else if (pnew && bx.kind == Box::Kind::Anonymous) {
                auto st = anonymous_style(*pnew, bx.style->display);
                if (!same(*bx.style, *st)) return false;
                mine = st;
            } else if (pnew && (bx.kind == Box::Kind::Text || bx.kind == Box::Kind::LineBreak || bx.kind == Box::Kind::Marker)) {
                return false; // shares no style we replaced: made some other way
            }
            if (mine) {
                map[bx.style.get()] = mine;
                plan.push_back({&bx, mine});
            }
        }
        (void)pold;
        for (auto& c : bx.children)
            if (!visit(*c, bx.style.get(), mine, ctx)) return false;
        return true;
    }
};
} // namespace

bool restyle_in_place(Box& root, dom_node* root_node, const std::unordered_set<const void*>& nodes, const StyleLookup& styles,
                      const css_unit_ctx& uc, const ExtraLookup& extras, uint64_t font_scope,
                      const std::function<bool(const Style&, const Style&)>& layout_equal, float zoom) {
    Builder b(styles, uc, extras, font_scope, zoom);
    InPlace ip{b, nodes, layout_equal, root_node, {}, {}, {}};
    if (!ip.visit(root, nullptr, nullptr, nullptr)) return false;
    for (auto& p : ip.plan) p.first->style = p.second;
    return true;
}

// ------------------------------------------------------------------ dump

std::string dump_box(const Box& b, int depth) {
    std::string out(static_cast<size_t>(depth) * 2, ' ');
    switch (b.kind) {
    case Box::Kind::Text: {
        std::string t, s = b.text;
        bool        space = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
            if (space && !t.empty()) t += ' ';
            space = false;
            t += c;
        }
        if (t.empty()) t = " ";
        else if (space) t += ' ';
        if (!s.empty() && !t.empty() && t[0] != ' ' && (s[0] == ' ' || s[0] == '\n' || s[0] == '\t')) t = " " + t;
        if (t.size() > 40) {
            size_t cut = 37;
            while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut; // do not split a UTF-8 sequence
            t = t.substr(0, cut) + "...";
        }
        out += "\"" + t + "\"\n";
        return out;
    }
    case Box::Kind::LineBreak: out += "br\n"; return out;
    case Box::Kind::Marker: out += "marker \"" + b.text + "\"\n"; return out;
    case Box::Kind::Anonymous: out += std::string("anon ") + display_name(b.style->display); break;
    case Box::Kind::Element: {
        out += b.tag;
        if (const std::string* id = b.attr("id")) if (!id->empty()) out += "#" + *id;
        if (const std::string* cl = b.attr("class")) {
            std::string c = *cl;
            for (char& ch : c) if (ch == ' ') ch = '.';
            if (!c.empty()) out += "." + c;
        }
        out += std::string(" ") + display_name(b.style->display);
        if (b.replaced != ReplacedKind::None) out += " replaced";
        if (b.continuation) out += " (cont)";
        break;
    }
    }
    if (b.style) {
        if (b.style->float_ != Float::None) out += b.style->float_ == Float::Left ? " float:left" : " float:right";
        if (b.style->position == Position::Absolute) out += " pos:absolute";
        else if (b.style->position == Position::Fixed) out += " pos:fixed";
        else if (b.style->position == Position::Relative) out += " pos:relative";
    }
    if (std::getenv("QUILL_DUMP_GEOM") && b.kind != Box::Kind::Text) {
        char buf[96];
        std::snprintf(buf, sizeof buf, " @%g,%g %gx%g", b.x, b.y, b.w, b.h);
        out += buf;
    }
    out += "\n";
    for (auto& c : b.children) out += dump_box(*c, depth + 1);
    return out;
}

} // namespace quill::engine
