#include "selection.h"

#include "font.h"
#include "style.h"

#include <algorithm>
#include <cmath>

namespace quill::engine {

namespace {
bool is_word_byte(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) return true; // non-ASCII letters count as word characters
    return std::isalnum(c) || c == '_' || c == '\'';
}
size_t utf8_prev(const std::string& s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
    return i;
}
size_t utf8_after(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    i++;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
    return i;
}
bool preformatted(const Box* b) {
    return b && b->style && (b->style->white_space == WhiteSpace::Pre || b->style->white_space == WhiteSpace::PreWrap ||
                             b->style->white_space == WhiteSpace::PreLine);
}
} // namespace

void TextIndex::build(const Box& root, float scroll_y, const BoxScroll* bs) {
    entries_.clear();
    order_.clear();
    ordinal_.clear();
    running_.clear();
    std::unordered_set<const Box*> seen;
    walk(root, 0, 0, false, scroll_y, bs, seen, 0);
    running_.clear();
}

void TextIndex::walk(const Box& b, float dx, float dy, bool fixed, float scroll_y, const BoxScroll* bs, std::unordered_set<const Box*>& seen,
                     int depth) {
    if (depth > 400 || b.skipped || !b.style || !seen.insert(&b).second) return;
    if (b.style->display == Display::None) return;
    if (b.style->position == Position::Fixed && !fixed) {
        fixed = true;
        dy -= scroll_y; // drawn at the same place on the screen whatever the scroll
    }
    float cdx = dx, cdy = dy; // for what is inside a scroll container
    if (b.scrollable()) {
        float ox, oy;
        box_scroll_of(b, bs, ox, oy);
        cdx += ox;
        cdy += oy;
    }
    for (auto& L : b.lines) {
        for (auto& f : L.frags) {
            if (f.kind == Frag::Kind::Atomic) {
                if (f.box) walk(*f.box, cdx, cdy, fixed, scroll_y, bs, seen, depth + 1);
                continue;
            }
            if (!f.box || f.text.empty()) continue;
            if (f.box->style && f.box->style->visibility != Visibility::Visible) continue;
            size_t& run = running_[f.box];
            entries_.push_back({&f, &L, &b, f.box, run, cdx, cdy});
            if (ordinal_.emplace(f.box, order_.size()).second) order_.push_back(f.box);
            run += f.text.size();
        }
    }
    for (auto& c : b.children) walk(*c, cdx, cdy, fixed, scroll_y, bs, seen, depth + 1);
}

size_t TextIndex::byte_at(const Entry& e, float x) const {
    const Frag& f  = *e.frag;
    float       fx = f.x - e.dx;
    if (x <= fx) return 0;
    if (x >= fx + f.w) return f.text.size();
    quill::Font* fn = style_font(*e.tbox->style);
    if (!fn) return 0;
    float  full = fn->text_width(f.text), k = full > 0 ? f.w / full : 1.f;
    size_t best = 0;
    float  prev = 0;
    for (size_t i = 0; i < f.text.size();) {
        size_t n = utf8_after(f.text, i);
        float  w = fn->text_width(f.text.substr(0, n)) * k;
        if (x - fx < (prev + w) / 2) return i;
        prev = w;
        best = n;
        i    = n;
    }
    return best;
}

TextPoint TextIndex::at(float x, float y, bool* exact) const {
    if (exact) *exact = false;
    if (entries_.empty()) return {};
    // Best first: a line at that height in a block under the point (nearest sideways); then any text in a block
    // under the point (nearest line); then anything (between columns, in a margin).
    const Entry* best = nullptr;
    int          tier = 3;
    float        bd = 1e30f;
    for (const Entry& e : entries_) {
        float top = e.line->y - e.dy, bot = top + e.line->h;
        float fx0 = e.frag->x - e.dx, fx1 = fx0 + e.frag->w;
        float ox0 = e.owner->x - e.dx, ox1 = ox0 + e.owner->w;
        float vy  = y < top ? top - y : y >= bot ? y - bot + 0.01f : 0.f;
        float hx  = x < fx0 ? fx0 - x : x > fx1 ? x - fx1 : 0.f;
        bool  in_block = x >= ox0 && x < ox1;
        int   t = vy == 0 && in_block ? 0 : in_block ? 1 : 2;
        float d = t == 0 ? hx : t == 1 ? vy * 1000.f + hx : vy * 4.f + hx;
        if (t < tier || (t == tier && d < bd)) {
            tier = t;
            bd   = d;
            best = &e;
        }
    }
    if (!best) return {};
    if (exact) *exact = tier == 0 && bd == 0;
    return {best->tbox, best->base + byte_at(*best, x)};
}

bool TextIndex::locate(const TextPoint& p, size_t& idx, size_t& off) const {
    if (!p.box) return false;
    const Entry* last = nullptr;
    size_t       li   = 0;
    for (size_t i = 0; i < entries_.size(); i++) {
        const Entry& e = entries_[i];
        if (e.tbox != p.box) continue;
        if (p.off >= e.base && p.off <= e.base + e.frag->text.size()) {
            idx = i;
            off = p.off - e.base;
            return true;
        }
        last = &e;
        li   = i;
    }
    if (last) { // past the end (the text got shorter): its end
        idx = li;
        off = last->frag->text.size();
        return true;
    }
    return false;
}

TextSelection TextIndex::ranges(const TextPoint& a, const TextPoint& b) const {
    TextSelection out;
    size_t ia, oa, ib, ob;
    if (!locate(a, ia, oa) || !locate(b, ib, ob)) return out;
    if (ib < ia || (ib == ia && ob < oa)) { std::swap(ia, ib); std::swap(oa, ob); }
    for (size_t i = ia; i <= ib; i++) {
        size_t s = i == ia ? oa : 0, e = i == ib ? ob : entries_[i].frag->text.size();
        if (e > s) out[entries_[i].frag] = {s, e};
    }
    return out;
}

std::string TextIndex::text(const TextPoint& a, const TextPoint& b) const {
    std::string out;
    size_t ia, oa, ib, ob;
    if (!locate(a, ia, oa) || !locate(b, ib, ob)) return out;
    if (ib < ia || (ib == ia && ob < oa)) { std::swap(ia, ib); std::swap(oa, ob); }
    const Entry* prev = nullptr;
    for (size_t i = ia; i <= ib; i++) {
        const Entry& e = entries_[i];
        size_t       s = i == ia ? oa : 0, en = i == ib ? ob : e.frag->text.size();
        if (prev && e.line != prev->line) {
            bool same_block = e.owner == prev->owner;
            if (same_block && !preformatted(e.owner)) { // a wrapped line: one space
                if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
            } else {
                while (!out.empty() && out.back() == ' ') out.pop_back();
                out += '\n';
            }
        }
        if (en > s) out += e.frag->text.substr(s, en - s);
        prev = &e;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    size_t lead = 0;
    while (lead < out.size() && (out[lead] == ' ' || out[lead] == '\n')) lead++;
    return out.substr(lead);
}

void TextIndex::word_at(const TextPoint& p, TextPoint& a, TextPoint& b) const {
    a = b = p;
    size_t i, off;
    if (!locate(p, i, off)) return;
    const std::string& t = entries_[i].frag->text;
    size_t             base = entries_[i].base;
    size_t             s = off, e = off;
    if (s < t.size() && is_word_byte(t, s)) {
        while (s > 0 && is_word_byte(t, utf8_prev(t, s))) s = utf8_prev(t, s);
        while (e < t.size() && is_word_byte(t, e)) e = utf8_after(t, e);
    } else if (s > 0 && is_word_byte(t, utf8_prev(t, s))) { // just after a word
        while (s > 0 && is_word_byte(t, utf8_prev(t, s))) s = utf8_prev(t, s);
    } else if (s < t.size()) { // on a space or punctuation: that character
        e = utf8_after(t, s);
    }
    a = {entries_[i].tbox, base + s};
    b = {entries_[i].tbox, base + e};
}

void TextIndex::block_at(const TextPoint& p, TextPoint& a, TextPoint& b) const {
    a = b = p;
    size_t i, off;
    if (!locate(p, i, off)) return;
    const Box* owner = entries_[i].owner;
    size_t     s = i, e = i;
    while (s > 0 && entries_[s - 1].owner == owner) s--;
    while (e + 1 < entries_.size() && entries_[e + 1].owner == owner) e++;
    a = {entries_[s].tbox, entries_[s].base};
    b = {entries_[e].tbox, entries_[e].base + entries_[e].frag->text.size()};
}

TextPoint TextIndex::first() const { return entries_.empty() ? TextPoint{} : TextPoint{entries_.front().tbox, entries_.front().base}; }
TextPoint TextIndex::last() const {
    if (entries_.empty()) return {};
    const Entry& e = entries_.back();
    return {e.tbox, e.base + e.frag->text.size()};
}

} // namespace quill::engine
