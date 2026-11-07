// The engine's document: HTML parsed into a libdom tree, styled by libcss.
//
// This header is deliberately free of libdom/libcss types (they are C headers with their own
// quirks); everything that touches them lives in document.cpp behind a pimpl. Later stages
// (box tree, layout, paint) read the tree through the small accessors declared here.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "box.h"
#include "layout.h"

namespace quill::engine {

// What the custom-property (var()) pre-pass did on the last resolve_styles().
struct VarStats {
    size_t defined = 0, substituted = 0, unresolved = 0, skipped_scoped = 0;
};

// Form data model read straight from the DOM (hidden and display:none controls have no boxes but are still submitted).
struct FormInfo {
    const void* node = nullptr; // the <form> element (matches ControlInfo::form)
    std::string action, method, enctype, target;
};

// A <frameset> page (instead of a <body>): the tree of framesets and frames, read from the DOM.
struct FrameNode {
    bool                   is_set = false;   // <frameset> (children laid out by rows/cols) or <frame>
    std::string            rows, cols;       // frameset: raw "100,*,20%" lists
    std::vector<FrameNode> children;         // frameset: frames and nested framesets, in document order
    int                    border = -1;      // frameset: border / framespacing in px (-1 = default)
    bool                   frameborder = true; // frameset / frame: frameborder="0" or "no" turns the borders off
    std::string            src, name;        // frame
    int                    scrolling = 0;    // frame: 0 auto, 1 yes, 2 no
    int                    marginwidth = -1, marginheight = -1; // frame: body margins override (-1 = page decides)
};
// One @font-face rule.
struct FontFaceRule {
    std::string                                family;
    int                                        weight_lo = 400, weight_hi = 400;
    bool                                       italic = false;
    std::vector<std::pair<std::string, std::string>> src; // (absolute url, format() hint lower-cased or ""), in order
    std::vector<std::pair<uint32_t, uint32_t>> ranges;    // unicode-range; empty = all
    uint8_t                                    display = 0;   // font-display: 0 auto, 1 swap, 2 fallback, 3 optional, 4 block
};

// One <area> of a client-side image map (<img usemap="#name"> + <map name="name">).
struct MapArea {
    enum class Shape { Rect, Circle, Poly, Default } shape = Shape::Rect;
    std::vector<float> coords; // CSS px from the image's top-left
    std::string        href, target, alt;
    bool               nohref = false;
    bool               contains(float x, float y) const;
};
struct ControlInfo {
    struct Opt { std::string label, value; bool selected = false; };
    const void* node = nullptr; // the element; equals Box::node of its box
    const void* form = nullptr; // owning <form> or null
    std::string tag, type, name, id, value, placeholder, text; // text: <textarea> content
    bool        has_value = false, checked = false, disabled = false, readonly = false;
    int         size = 20, rows = 2, cols = 20, maxlength = 0;
    bool        multiple = false; // <select multiple> / <input type=file multiple>
    bool        autofocus = false;
    int         list_size = 0;    // <select size=N> as written (0 = absent)
    std::string accept;           // <input type=file accept=...>
    std::vector<Opt> options;
};

class Document {
public:
    Document();
    ~Document();
    Document(const Document&)            = delete;
    Document& operator=(const Document&) = delete;

    // Parses HTML (scripting disabled, so <noscript> content is real content). `charset` may be empty.
    bool parse_html(const std::string& html, const std::string& charset = "");

    // Finds <style> elements and <link rel=stylesheet> in document order and adds them as author sheets.
    // `loader` returns the text of a stylesheet for an href (relative to `base_url`); returning false
    // skips it. @import is followed the same way (depth limit 8).
    using CssLoader = std::function<bool(const std::string& href, std::string& css)>;
    // `prefetch`, if given, first receives every <link rel=stylesheet> URL in document order, so the caller can start
    // downloading them all in parallel; `loader` is still asked for each one, in order (the cascade order is kept).
    void load_stylesheets(const CssLoader& loader, const std::string& base_url = "",
                          const std::function<void(const std::vector<std::string>& urls)>& prefetch = nullptr);

    // Adds one stylesheet by text.
    bool add_author_css(const std::string& css, const std::string& base_url = "");

    // Cascades and computes styles for every element (UA sheet + author sheets + style attributes).
    bool resolve_styles(int viewport_w, int viewport_h);
    // Page zoom (1 = 100%): every CSS length is multiplied by it, and the page sees a viewport of viewport / zoom CSS
    // px. Takes effect at the next resolve_styles(); layout() and painting keep working in screen px.
    void  set_zoom(float z);
    // Changes whenever the box tree is rebuilt (boxes from before are gone, even if new ones reuse their addresses).
    uint64_t box_generation() const;
    float zoom() const;

    // Text dumps used by tests and --dump tools.
    std::string dump_tree(bool with_styles) const;

    // Computed value of one property on the first element matching a simple selector ([tag][#id][.class]).
    // Lengths come back in px. Returns false (with a reason in `out`) if it can't answer.
    bool computed_value(const std::string& selector, const std::string& prop, std::string& out) const;

    VarStats var_stats() const;

    // Page metadata and form model (valid after parse_html).
    std::string              title() const;      // whitespace-collapsed <title>
    std::string              base_href() const;  // href of the first <base>, or ""
    // Interaction state for :hover :active :focus :focus-within. `node` is a Box::node (or null). Hover and active mark
    // the element and its ancestors, focus the element itself (focus-within its ancestors too). Each returns true if
    // some selector now matches a different set of elements, i.e. the caller must resolve_styles + layout again.
    // has_state_rules() is false when no author stylesheet uses any of them, so such pages never need a restyle.
    bool                     set_hover(const void* node);
    bool                     set_active(const void* node);
    bool                     set_focus(const void* node);
    bool                     has_state_rules() const;
    std::vector<FormInfo>    forms() const;
    std::string              base_target() const; // target of the first <base>, or ""
    std::string              meta_named(const std::string& name) const; // content of the last <meta name=...> ("" if none)
    // The best site icon the page names (<link rel="icon" ...>, preferring 32x32 / 16x16 / SVG), raw href; "" if none.
    std::string              icon_href() const;
    // <meta http-equiv="refresh" content="5; url=next.html">: true with the delay (seconds) and the raw URL ("" = this
    // page again) if the page asks to go somewhere else after a while.
    bool                     meta_refresh(double& delay, std::string& url) const;
    // True (filling `out`) if the page is a frameset document: <html> holds a <frameset> instead of a <body>.
    bool                     frameset(FrameNode& out) const;
    // :visited / :link. `visited(href)` gets the raw href attribute of an <a>/<area>; it must be thread-safe
    // (styles may be resolved on a worker thread). Unset = nothing is visited.
    void                     set_link_visited(std::function<bool(const std::string& href)> visited);
    // The next resolve_styles() recomputes every element (e.g. after the visited set changed).
    void                     restyle_all();
    // A click on `node` (a Box::node): if it is in the <summary> of a <details>, opens / closes that <details> (its open
    // attribute) and returns true; the caller then resolves styles and lays out again.
    bool                     toggle_details(const void* node);
    // True if the last resolve_styles() only changed paint properties (a :hover colour, say): the boxes kept their
    // layout, so the caller can repaint without calling layout().
    bool                     restyle_was_paint_only() const;
    size_t                   paint_only_restyles() const; // how many (tests / profiling)
    std::vector<ControlInfo> controls() const;   // input / textarea / select / button, in document order
    // Web fonts. The scope (a FontManager scope) is written into every Style the next resolve_styles() makes, so text
    // is drawn with this page's @font-face fonts. font_faces_in_use() (after resolve_styles) lists the @font-face
    // rules whose family some text asks for and whose unicode-range covers some of that text: the ones to download.
    void                      set_font_scope(uint64_t scope);
    std::vector<FontFaceRule> font_faces_in_use() const;
    // Every <map> on the page by name (and id), with its <area>s in document order.
    std::vector<std::pair<std::string, std::vector<MapArea>>> image_maps() const;

    // Layout (after resolve_styles). The measurer must outlive the document; null = a deterministic fake
    // font (0.5em per character). Afterwards every box has absolute document coordinates.
    void set_text_measurer(TextMeasurer* m);
    void set_image_sizer(ImageSizer f);
    void set_image_broken(ImageBroken f); // lets broken images be laid out as their alt text
    void set_image_request(std::function<void(const std::string&)> f); // images layout found but needs no size of
    bool layout(int viewport_w, int viewport_h);
    // Border-box rect of the first element box matching a simple selector; and a line box of it.
    bool box_rect(const std::string& selector, Rect& out) const;
    bool line_rect(const std::string& selector, size_t index, Rect& out) const;

    // The box tree built by the last resolve_styles() (null before). Owned by the document.
    const Box* box_root() const;
    // Indented dump of the box subtree rooted at the first element box matching a simple selector
    // ([tag][#id][.class]); empty selector = whole tree.
    std::string dump_boxes(const std::string& selector = "") const;
    size_t element_count() const;
    size_t stylesheet_count() const;
    const std::string& last_error() const;

    struct Impl; // opaque; defined in document.cpp

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace quill::engine
