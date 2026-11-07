// Text selection over a laid-out page: which text is where, the text at a point, the text between two points.
//
// A position is a text box and a byte offset into the text that box shows (all its line fragments in order), so it
// stays meaningful when the page is laid out again (the fragments change; the boxes don't).
#pragma once

#include "box.h"
#include "paint.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace quill::engine {

struct TextPoint {
    const Box* box = nullptr; // a Kind::Text box
    size_t     off = 0;       // bytes into the concatenation of its fragments' text
    bool       valid() const { return box != nullptr; }
    bool       operator==(const TextPoint& o) const { return box == o.box && off == o.off; }
    bool       operator!=(const TextPoint& o) const { return !(*this == o); }
};

// Every text fragment of a page in reading (document) order, with where it is drawn.
class TextIndex {
public:
    // scroll_y: the page's scroll (fixed boxes are drawn at a fixed place on the screen); box_scroll: scroll
    // containers' positions. Coordinates below are document coordinates (screen y + scroll_y).
    void build(const Box& root, float scroll_y, const BoxScroll* box_scroll);
    bool empty() const { return entries_.empty(); }

    // The position nearest (x, y). `exact` (optional) says whether the point is on the text itself.
    TextPoint at(float x, float y, bool* exact = nullptr) const;
    // The selection between two positions (either order), for painting.
    TextSelection ranges(const TextPoint& a, const TextPoint& b) const;
    // The text between two positions: line wraps become spaces, other block boundaries line breaks.
    std::string text(const TextPoint& a, const TextPoint& b) const;
    // The word / the whole block of text around a position.
    void word_at(const TextPoint& p, TextPoint& a, TextPoint& b) const;
    void block_at(const TextPoint& p, TextPoint& a, TextPoint& b) const;
    TextPoint first() const;
    TextPoint last() const;
    bool      contains(const Box* text_box) const { return ordinal_.count(text_box) != 0; }
    // Text boxes by their place in reading order: lets a selection outlive a rebuilt box tree of the same page.
    size_t     text_boxes() const { return order_.size(); }
    size_t     ordinal(const Box* text_box) const { auto it = ordinal_.find(text_box); return it == ordinal_.end() ? size_t(-1) : it->second; }
    const Box* text_box(size_t ordinal) const { return ordinal < order_.size() ? order_[ordinal] : nullptr; }

private:
    struct Entry {
        const Frag* frag;
        const Line* line;
        const Box*  owner; // the box whose lines these are
        const Box*  tbox;  // the text box
        size_t      base;  // where this fragment starts in its text box's text
        float       dx, dy; // drawn at (frag.x - dx, ...) in document coordinates (scroll containers, fixed boxes)
    };
    void   walk(const Box& b, float dx, float dy, bool fixed, float scroll_y, const BoxScroll* bs, std::unordered_set<const Box*>& seen,
                int depth);
    bool   locate(const TextPoint& p, size_t& idx, size_t& off) const; // entry and byte offset within it
    size_t byte_at(const Entry& e, float x) const;                    // nearest boundary to document x
    std::vector<Entry>              entries_;
    std::vector<const Box*>                order_;
    std::unordered_map<const Box*, size_t> ordinal_;
    std::unordered_map<const Box*, size_t> running_; // build-time: bytes seen so far per text box
};

} // namespace quill::engine
