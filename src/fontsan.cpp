#include "fontsan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>

#ifdef QUILL_HAVE_BROTLI
#include <brotli/decode.h>
#endif

// stb_image's zlib inflater (compiled in stb_impl.cpp): WOFF 1.0 tables are zlib streams.
extern "C" int stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

namespace quill {

namespace {

constexpr size_t   kMaxInput     = 16u << 20; // a font file
constexpr size_t   kMaxTables    = 64u << 20; // all tables of one font, decompressed
constexpr uint32_t kMaxMappings  = 300000;    // cmap entries kept
constexpr int      kMaxCompDepth = 8;         // composite glyph nesting

constexpr uint32_t tag(const char* s) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}
constexpr uint32_t kGlyf = tag("glyf"), kLoca = tag("loca"), kHead = tag("head"), kHhea = tag("hhea"), kHmtx = tag("hmtx"),
                   kMaxp = tag("maxp"), kCmap = tag("cmap"), kFvar = tag("fvar");

using Tables = std::map<uint32_t, std::string>;

// Bounds-checked big-endian reader. Any read past the end sets ok = false and returns 0.
struct Rd {
    const unsigned char* p;
    size_t               n, pos = 0;
    bool                 ok = true;
    Rd(const unsigned char* d, size_t len, size_t at = 0) : p(d), n(len), pos(at) { if (at > len) ok = false; }
    explicit Rd(const std::string& s, size_t at = 0) : Rd(reinterpret_cast<const unsigned char*>(s.data()), s.size(), at) {}
    bool     has(size_t k) const { return ok && pos <= n && k <= n - pos; }
    bool     need(size_t k) { if (!has(k)) ok = false; return ok; }
    uint8_t  u8() { if (!need(1)) return 0; return p[pos++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = static_cast<uint16_t>((p[pos] << 8) | p[pos + 1]); pos += 2; return v; }
    int16_t  s16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (static_cast<uint32_t>(p[pos]) << 24) | (static_cast<uint32_t>(p[pos + 1]) << 16) | (static_cast<uint32_t>(p[pos + 2]) << 8) | p[pos + 3];
        pos += 4;
        return v;
    }
    bool skip(size_t k) { if (!need(k)) return false; pos += k; return true; }
    // WOFF2's UIntBase128 and 255UInt16
    uint32_t base128() {
        uint32_t v = 0;
        for (int i = 0; i < 5; i++) {
            uint8_t b = u8();
            if (!ok) return 0;
            if (i == 0 && b == 0x80) { ok = false; return 0; } // leading zero
            if (v & 0xFE000000u) { ok = false; return 0; }     // overflow
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    uint16_t u255() {
        uint8_t c = u8();
        if (c == 253) return u16();
        if (c == 255) return static_cast<uint16_t>(u8() + 253);
        if (c == 254) return static_cast<uint16_t>(u8() + 506);
        return c;
    }
};

uint16_t rd16(const std::string& t, size_t off) { Rd r(t, off); uint16_t v = r.u16(); return r.ok ? v : 0; }
int16_t  rs16(const std::string& t, size_t off) { return static_cast<int16_t>(rd16(t, off)); }
uint32_t rd32(const std::string& t, size_t off) { Rd r(t, off); uint32_t v = r.u32(); return r.ok ? v : 0; }

void w16(std::string& o, uint32_t v) { o += static_cast<char>((v >> 8) & 0xFF); o += static_cast<char>(v & 0xFF); }
void w32(std::string& o, uint32_t v) { w16(o, v >> 16); w16(o, v & 0xFFFF); }
void put16(std::string& o, size_t at, uint32_t v) { o[at] = static_cast<char>((v >> 8) & 0xFF); o[at + 1] = static_cast<char>(v & 0xFF); }

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

// ------------------------------------------------------------------ containers
bool read_sfnt(const unsigned char* p, size_t n, Tables& t, std::string* err) {
    Rd       r(p, n);
    uint32_t ver = r.u32();
    if (ver == tag("OTTO")) return fail(err, "CFF (PostScript-outline) fonts are not supported");
    if (ver == tag("ttcf")) return fail(err, "font collections are not supported");
    if (ver != 0x00010000u && ver != tag("true")) return fail(err, "not a font");
    uint16_t num = r.u16();
    r.skip(6);
    if (!r.ok || num == 0 || num > 256) return fail(err, "bad table directory");
    size_t total = 0;
    for (uint16_t i = 0; i < num; i++) {
        uint32_t tg = r.u32();
        r.u32(); // checksum
        uint32_t off = r.u32(), len = r.u32();
        if (!r.ok) return fail(err, "truncated table directory");
        if (off > n || len > n - off) return fail(err, "table outside the file");
        if (t.count(tg)) return fail(err, "duplicate table");
        total += len;
        if (total > kMaxTables) return fail(err, "font too large");
        t[tg] = std::string(reinterpret_cast<const char*>(p + off), len);
    }
    return true;
}

bool read_woff(const unsigned char* p, size_t n, Tables& t, std::string* err) {
    Rd       r(p, n);
    r.u32(); // 'wOFF'
    uint32_t flavor = r.u32();
    if (flavor == tag("OTTO")) return fail(err, "CFF (PostScript-outline) fonts are not supported");
    if (flavor != 0x00010000u && flavor != tag("true")) return fail(err, "unsupported WOFF flavor");
    r.u32(); // length
    uint16_t num = r.u16();
    r.skip(2 + 4 + 2 + 2 + 4 + 4 + 4 + 4 + 4); // reserved, totalSfntSize, version, meta, private
    if (!r.ok || num == 0 || num > 256) return fail(err, "bad WOFF header");
    size_t total = 0;
    for (uint16_t i = 0; i < num; i++) {
        uint32_t tg = r.u32(), off = r.u32(), comp = r.u32(), orig = r.u32();
        r.u32(); // checksum
        if (!r.ok) return fail(err, "truncated WOFF directory");
        if (off > n || comp > n - off) return fail(err, "WOFF table outside the file");
        if (comp > orig || orig > kMaxTables) return fail(err, "bad WOFF table size");
        total += orig;
        if (total > kMaxTables) return fail(err, "font too large");
        if (t.count(tg)) return fail(err, "duplicate table");
        if (comp == orig) {
            t[tg] = std::string(reinterpret_cast<const char*>(p + off), orig);
        } else {
            std::string out(orig, '\0');
            int         got = stbi_zlib_decode_buffer(&out[0], static_cast<int>(orig), reinterpret_cast<const char*>(p + off), static_cast<int>(comp));
            if (got != static_cast<int>(orig)) return fail(err, "WOFF table does not decompress");
            t[tg] = std::move(out);
        }
    }
    return true;
}

#ifdef QUILL_HAVE_BROTLI
const char* const kWoff2Tags[63] = {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep",
                                    "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE",
                                    "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt",
                                    "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar",
                                    "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

// Length of one composite glyph's component records starting at r.pos (flags.. up to the last component), or false.
bool composite_length(Rd& r, bool& has_instructions, std::vector<uint16_t>* refs) {
    has_instructions = false;
    for (int k = 0; k < 1024; k++) {
        uint16_t flags = r.u16(), gi = r.u16();
        if (refs) refs->push_back(gi);
        r.skip((flags & 0x0001) ? 4 : 2);
        if (flags & 0x0008) r.skip(2);
        else if (flags & 0x0040) r.skip(4);
        else if (flags & 0x0080) r.skip(8);
        if (!r.ok) return false;
        if (flags & 0x0100) has_instructions = true;
        if (!(flags & 0x0020)) return true;
    }
    return false;
}

// WOFF2 5.1: the transformed glyf table back to plain glyf + loca (long offsets).
bool woff2_glyf(const std::string& t, std::string& glyf, std::string& loca, std::vector<int16_t>& xmins, uint16_t& num_glyphs, std::string* err) {
    Rd r(t);
    r.u16(); // reserved
    uint16_t options = r.u16();
    num_glyphs       = r.u16();
    r.u16(); // indexFormat (we always write long loca)
    uint32_t sz[7];
    for (auto& s : sz) s = r.u32();
    if (!r.ok) return fail(err, "truncated WOFF2 glyf header");
    size_t pos = r.pos;
    Rd     st[7] = {Rd(nullptr, 0), Rd(nullptr, 0), Rd(nullptr, 0), Rd(nullptr, 0), Rd(nullptr, 0), Rd(nullptr, 0), Rd(nullptr, 0)};
    for (int i = 0; i < 7; i++) {
        if (pos > t.size() || sz[i] > t.size() - pos) return fail(err, "WOFF2 glyf stream outside the table");
        st[i] = Rd(reinterpret_cast<const unsigned char*>(t.data()) + pos, sz[i]);
        pos += sz[i];
    }
    (void)options; // bit 0: an overlapSimpleBitmap follows; it only sets a rendering hint we don't use
    Rd&    nc_s = st[0];
    Rd&    np_s = st[1];
    Rd&    fl_s = st[2];
    Rd&    gl_s = st[3];
    Rd&    co_s = st[4];
    Rd&    bb_s = st[5];
    Rd&    in_s = st[6];
    size_t bitmap_len = ((static_cast<size_t>(num_glyphs) + 31) / 32) * 4;
    if (!bb_s.has(bitmap_len)) return fail(err, "WOFF2 bbox bitmap missing");
    const unsigned char* bitmap = bb_s.p;
    bb_s.pos                    = bitmap_len;
    xmins.assign(num_glyphs, 0);
    loca.clear();
    glyf.clear();
    std::vector<int32_t> xs, ys;
    std::vector<uint8_t> on;
    std::vector<uint16_t> ends;
    for (uint32_t g = 0; g < num_glyphs; g++) {
        w32(loca, static_cast<uint32_t>(glyf.size()));
        int16_t nc       = nc_s.s16();
        bool    has_bbox = (bitmap[g >> 3] & (0x80 >> (g & 7))) != 0;
        if (!nc_s.ok) return fail(err, "WOFF2 nContour stream too short");
        if (nc == 0) continue; // empty glyph
        std::string out;
        if (nc == -1) { // composite
            if (!has_bbox) return fail(err, "WOFF2 composite glyph without bbox");
            int16_t bb[4];
            for (auto& v : bb) v = bb_s.s16();
            size_t start = co_s.pos;
            bool   instr = false;
            if (!composite_length(co_s, instr, nullptr)) return fail(err, "WOFF2 composite stream too short");
            w16(out, 0xFFFF);
            for (auto v : bb) w16(out, static_cast<uint16_t>(v));
            out.append(reinterpret_cast<const char*>(co_s.p + start), co_s.pos - start);
            if (instr) {
                uint16_t il = gl_s.u255();
                if (!in_s.has(il)) return fail(err, "WOFF2 instruction stream too short");
                w16(out, il);
                out.append(reinterpret_cast<const char*>(in_s.p + in_s.pos), il);
                in_s.pos += il;
            }
            xmins[g] = bb[0];
        } else if (nc > 0) {
            ends.clear();
            uint32_t total = 0;
            for (int c = 0; c < nc; c++) {
                total += np_s.u255();
                if (total > 0xFFFF) return fail(err, "WOFF2 glyph has too many points");
                ends.push_back(static_cast<uint16_t>(total ? total - 1 : 0));
            }
            if (!np_s.ok || !fl_s.has(total)) return fail(err, "WOFF2 point streams too short");
            xs.resize(total);
            ys.resize(total);
            on.resize(total);
            int32_t x = 0, y = 0;
            for (uint32_t i = 0; i < total; i++) {
                uint8_t flag = fl_s.p[fl_s.pos++];
                on[i]        = !(flag >> 7);
                flag &= 0x7F;
                size_t nb = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
                if (!gl_s.has(nb)) return fail(err, "WOFF2 glyph stream too short");
                const unsigned char* in = gl_s.p + gl_s.pos;
                gl_s.pos += nb;
                auto    sign = [](int f, int base) { return (f & 1) ? base : -base; };
                int32_t dx, dy;
                if (flag < 10) {
                    dx = 0;
                    dy = sign(flag, ((flag & 14) << 7) + in[0]);
                } else if (flag < 20) {
                    dx = sign(flag, (((flag - 10) & 14) << 7) + in[0]);
                    dy = 0;
                } else if (flag < 84) {
                    int b0 = flag - 20, b1 = in[0];
                    dx     = sign(flag, 1 + (b0 & 0x30) + (b1 >> 4));
                    dy     = sign(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
                } else if (flag < 120) {
                    int b0 = flag - 84;
                    dx     = sign(flag, 1 + ((b0 / 12) << 8) + in[0]);
                    dy     = sign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + in[1]);
                } else if (flag < 124) {
                    int b2 = in[1];
                    dx     = sign(flag, (in[0] << 4) + (b2 >> 4));
                    dy     = sign(flag >> 1, ((b2 & 0x0f) << 8) + in[2]);
                } else {
                    dx = sign(flag, (in[0] << 8) + in[1]);
                    dy = sign(flag >> 1, (in[2] << 8) + in[3]);
                }
                x += dx;
                y += dy;
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return fail(err, "WOFF2 coordinates out of range");
                xs[i] = x;
                ys[i] = y;
            }
            uint16_t il = gl_s.u255();
            if (!gl_s.ok || !in_s.has(il)) return fail(err, "WOFF2 instruction stream too short");
            int16_t bb[4];
            if (has_bbox) {
                for (auto& v : bb) v = bb_s.s16();
            } else {
                int32_t x0 = total ? xs[0] : 0, x1 = x0, y0 = total ? ys[0] : 0, y1 = y0;
                for (uint32_t i = 1; i < total; i++) {
                    x0 = std::min(x0, xs[i]); x1 = std::max(x1, xs[i]);
                    y0 = std::min(y0, ys[i]); y1 = std::max(y1, ys[i]);
                }
                bb[0] = static_cast<int16_t>(x0); bb[1] = static_cast<int16_t>(y0); bb[2] = static_cast<int16_t>(x1); bb[3] = static_cast<int16_t>(y1);
            }
            w16(out, static_cast<uint16_t>(nc));
            for (auto v : bb) w16(out, static_cast<uint16_t>(v));
            for (auto e : ends) w16(out, e);
            w16(out, il);
            out.append(reinterpret_cast<const char*>(in_s.p + in_s.pos), il);
            in_s.pos += il;
            for (uint32_t i = 0; i < total; i++) out += static_cast<char>(on[i] ? 1 : 0); // 16-bit deltas, no repeats
            int32_t px = 0, py = 0;
            for (uint32_t i = 0; i < total; i++) { w16(out, static_cast<uint16_t>(static_cast<int16_t>(xs[i] - px))); px = xs[i]; }
            for (uint32_t i = 0; i < total; i++) { w16(out, static_cast<uint16_t>(static_cast<int16_t>(ys[i] - py))); py = ys[i]; }
            for (uint32_t i = 1; i < total; i++)
                if (xs[i] - xs[i - 1] < -32768 || xs[i] - xs[i - 1] > 32767 || ys[i] - ys[i - 1] < -32768 || ys[i] - ys[i - 1] > 32767)
                    return fail(err, "WOFF2 coordinate delta out of range");
            xmins[g] = bb[0];
        } else {
            return fail(err, "WOFF2 bad contour count");
        }
        if (bb_s.ok == false) return fail(err, "WOFF2 bbox stream too short");
        if (out.size() & 1) out += '\0';
        glyf += out;
        if (glyf.size() > kMaxTables) return fail(err, "font too large");
    }
    w32(loca, static_cast<uint32_t>(glyf.size()));
    return true;
}

bool read_woff2(const unsigned char* p, size_t n, Tables& t, std::string* err) {
    Rd       r(p, n);
    r.u32(); // 'wOF2'
    uint32_t flavor = r.u32();
    if (flavor == tag("ttcf")) return fail(err, "font collections are not supported");
    if (flavor == tag("OTTO")) return fail(err, "CFF (PostScript-outline) fonts are not supported");
    r.u32(); // length
    uint16_t num = r.u16();
    r.u16();
    r.u32(); // totalSfntSize
    uint32_t compressed = r.u32();
    r.skip(2 + 2 + 4 + 4 + 4 + 4 + 4);
    if (!r.ok || num == 0 || num > 256) return fail(err, "bad WOFF2 header");
    struct Ent { uint32_t tag, orig, len; bool xform; };
    std::vector<Ent> ents;
    uint64_t         total = 0;
    for (uint16_t i = 0; i < num; i++) {
        uint8_t  fl = r.u8();
        uint32_t tg = (fl & 63) == 63 ? r.u32() : tag(kWoff2Tags[fl & 63]);
        int      xv = (fl >> 6) & 3;
        Ent      e{tg, r.base128(), 0, false};
        if (tg == kGlyf || tg == kLoca) e.xform = xv == 0;
        else if (tg == kHmtx) e.xform = xv == 1;
        if (xv != 0 && !(tg == kGlyf || tg == kLoca || (tg == kHmtx && xv == 1))) return fail(err, "unknown WOFF2 transform");
        if ((tg == kGlyf || tg == kLoca) && xv != 0 && xv != 3) return fail(err, "unknown WOFF2 transform");
        e.len = e.xform ? r.base128() : e.orig;
        if (!r.ok) return fail(err, "bad WOFF2 table directory");
        total += e.len;
        if (total > kMaxTables) return fail(err, "font too large");
        ents.push_back(e);
    }
    if (r.pos > n || compressed > n - r.pos) return fail(err, "WOFF2 data outside the file");
    std::string data(static_cast<size_t>(total), '\0');
    size_t      out_len = data.size();
    if (BrotliDecoderDecompress(compressed, p + r.pos, &out_len, reinterpret_cast<uint8_t*>(&data[0])) != BROTLI_DECODER_RESULT_SUCCESS ||
        out_len != data.size())
        return fail(err, "WOFF2 data does not decompress");
    size_t   off = 0;
    Tables   raw;
    std::map<uint32_t, bool> xformed;
    for (auto& e : ents) {
        if (raw.count(e.tag)) return fail(err, "duplicate table");
        raw[e.tag]     = data.substr(off, e.len);
        xformed[e.tag] = e.xform;
        off += e.len;
    }
    std::vector<int16_t> xmins;
    uint16_t             ng = 0;
    if (raw.count(kGlyf) && xformed[kGlyf]) {
        std::string glyf, loca;
        if (!woff2_glyf(raw[kGlyf], glyf, loca, xmins, ng, err)) return false;
        raw[kGlyf] = std::move(glyf);
        raw[kLoca] = std::move(loca);
        if (raw.count(kHead) && raw[kHead].size() >= 54) put16(raw[kHead], 50, 1); // we wrote long offsets
    }
    if (raw.count(kHmtx) && xformed[kHmtx]) { // 5.4: advance widths kept, side bearings = glyph xMin
        if (xmins.empty() || !raw.count(kHhea) || raw[kHhea].size() < 36) return fail(err, "WOFF2 hmtx transform without glyf");
        uint16_t    nh = rd16(raw[kHhea], 34);
        if (nh == 0 || nh > ng) return fail(err, "bad hhea");
        Rd          h(raw[kHmtx]);
        uint8_t     flags = h.u8();
        std::vector<uint16_t> adv(nh);
        for (auto& a : adv) a = h.u16();
        std::string out;
        std::vector<int16_t> lsb(ng);
        for (uint32_t i = 0; i < nh; i++) lsb[i] = (flags & 1) ? xmins[i] : h.s16();
        for (uint32_t i = nh; i < ng; i++) lsb[i] = (flags & 2) ? xmins[i] : h.s16();
        if (!h.ok) return fail(err, "WOFF2 hmtx too short");
        for (uint32_t i = 0; i < nh; i++) { w16(out, adv[i]); w16(out, static_cast<uint16_t>(lsb[i])); }
        for (uint32_t i = nh; i < ng; i++) w16(out, static_cast<uint16_t>(lsb[i]));
        raw[kHmtx] = std::move(out);
    }
    t = std::move(raw);
    return true;
}
#endif

// ------------------------------------------------------------------ glyphs
// Checks one glyf entry the way stb_truetype will read it. Fills `refs` with a composite's component glyphs.
bool glyph_ok(const std::string& g, uint32_t num_glyphs, std::vector<uint16_t>& refs) {
    refs.clear();
    if (g.empty()) return true;
    Rd      r(g);
    int16_t nc = r.s16();
    int16_t x0 = r.s16(), y0 = r.s16(), x1 = r.s16(), y1 = r.s16();
    if (!r.ok || x0 > x1 || y0 > y1) return false; // stb_truetype sizes bitmaps from this box
    if (nc > 0) {
        int32_t last = -1;
        std::vector<uint32_t> starts; // first point of each contour
        for (int i = 0; i < nc; i++) {
            int32_t e = r.u16();
            if (!r.ok || e <= last) return false; // stb walks the contours in order: they must increase
            starts.push_back(static_cast<uint32_t>(last + 1));
            last = e;
        }
        uint32_t n  = static_cast<uint32_t>(last) + 1;
        uint16_t il = r.u16();
        if (!r.skip(il)) return false;
        // flags, exactly as stb_truetype consumes them (a repeat may run past the last point: it just stops)
        std::vector<uint8_t> flags(n);
        uint8_t              cur = 0, repeat = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (repeat == 0) {
                cur = r.u8();
                if (cur & 8) repeat = r.u8();
            } else {
                --repeat;
            }
            flags[i] = cur;
        }
        if (!r.ok) return false;
        // stb_truetype looks one point ahead when a contour starts off the curve: the very last point can't do that
        // (it would read past its point array), and a one-point off-curve contour draws nothing anyway
        for (size_t k = 0; k < starts.size(); k++) {
            uint32_t s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] - 1 : n - 1;
            if (!(flags[s] & 1) && (s == n - 1 || s == e)) return false;
        }
        size_t need = 0;
        for (uint8_t f : flags) need += (f & 2) ? 1 : (f & 16) ? 0 : 2;
        for (uint8_t f : flags) need += (f & 4) ? 1 : (f & 32) ? 0 : 2;
        return r.has(need);
    }
    if (nc < 0) { // composite
        for (int k = 0; k < 256; k++) {
            uint16_t flags = r.u16(), gi = r.u16();
            if (!r.ok || gi >= num_glyphs) return false;
            if (!(flags & 0x0002)) return false; // point-matched placement: stb_truetype can't read it (and misparses)
            refs.push_back(gi);
            r.skip((flags & 0x0001) ? 4 : 2);
            if (flags & 0x0008) r.skip(2);
            else if (flags & 0x0040) r.skip(4);
            else if (flags & 0x0080) r.skip(8);
            if (!r.ok) return false;
            if (!(flags & 0x0020)) return true;
        }
        return false;
    }
    return true; // no contours
}

// ------------------------------------------------------------------ cmap
using Mapping = std::map<uint32_t, uint16_t>;

void add_map(Mapping& m, uint32_t cp, uint32_t g, uint32_t num_glyphs) {
    if (g == 0 || g >= num_glyphs || cp > 0x10FFFF || m.size() >= kMaxMappings) return;
    m.emplace(cp, static_cast<uint16_t>(g));
}

bool cmap_format4(const std::string& c, size_t off, Mapping& m, uint32_t ng, bool symbol) {
    Rd       r(c, off);
    r.u16(); // format
    uint32_t len = r.u16();
    r.u16(); // language
    uint32_t segx2 = r.u16();
    if (!r.ok || segx2 == 0 || (segx2 & 1)) return false;
    size_t end = std::min(c.size(), off + std::max<size_t>(len, 14));
    if (len < 16 + 4 * static_cast<size_t>(segx2)) end = c.size(); // some fonts get the length wrong: trust the table end
    uint32_t seg = segx2 / 2;
    size_t   ends = off + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
    if (ranges + segx2 > end) return false;
    for (uint32_t i = 0; i < seg; i++) {
        uint32_t e = rd16(c, ends + 2 * i), s = rd16(c, starts + 2 * i);
        uint16_t delta = rd16(c, deltas + 2 * i), ro = rd16(c, ranges + 2 * i);
        if (s > e) continue;
        for (uint32_t cp = s; cp <= e && cp < 0xFFFF; cp++) {
            uint32_t g;
            if (ro == 0) {
                g = (cp + delta) & 0xFFFF;
            } else {
                size_t at = ranges + 2 * i + ro + 2 * static_cast<size_t>(cp - s);
                if (at + 2 > end) break;
                g = rd16(c, at);
                if (g) g = (g + delta) & 0xFFFF;
            }
            add_map(m, cp, g, ng);
            if (symbol && cp >= 0xF000 && cp <= 0xF0FF) add_map(m, cp - 0xF000, g, ng); // symbol fonts: U+00xx too
        }
        if (m.size() >= kMaxMappings) break;
    }
    return true;
}

bool cmap_format12(const std::string& c, size_t off, Mapping& m, uint32_t ng) {
    Rd       r(c, off);
    r.u16();
    r.u16();
    uint32_t len = r.u32();
    r.u32();
    uint32_t groups = r.u32();
    if (!r.ok || len < 16 || off + len > c.size() || groups > (len - 16) / 12) return false;
    for (uint32_t i = 0; i < groups && m.size() < kMaxMappings; i++) {
        uint32_t s = r.u32(), e = r.u32(), g = r.u32();
        if (!r.ok) return false;
        if (s > e || e > 0x10FFFF) continue;
        for (uint32_t cp = s; cp <= e && m.size() < kMaxMappings; cp++) add_map(m, cp, g + (cp - s), ng);
    }
    return true;
}

bool cmap_format6(const std::string& c, size_t off, Mapping& m, uint32_t ng) {
    Rd r(c, off);
    r.u16();
    r.u16();
    r.u16();
    uint32_t first = r.u16(), count = r.u16();
    for (uint32_t i = 0; i < count && r.ok; i++) {
        uint16_t g = r.u16();
        if (r.ok) add_map(m, first + i, g, ng);
    }
    return r.ok;
}

bool read_cmap(const std::string& c, uint32_t ng, Mapping& m) {
    Rd       r(c);
    r.u16();
    uint16_t num = r.u16();
    if (!r.ok) return false;
    struct Cand { int rank; size_t off; int fmt; bool symbol; };
    std::vector<Cand> cands;
    for (uint16_t i = 0; i < num && i < 64; i++) {
        uint16_t plat = r.u16(), enc = r.u16();
        uint32_t off  = r.u32();
        if (!r.ok) break;
        if (off + 2 > c.size()) continue;
        int fmt = rd16(c, off), rank = -1;
        bool sym = false;
        if (plat == 3 && enc == 10 && fmt == 12) rank = 6;
        else if (plat == 0 && fmt == 12) rank = 5;
        else if (plat == 3 && enc == 1 && fmt == 4) rank = 4;
        else if (plat == 0 && fmt == 4) rank = 3;
        else if (plat == 3 && enc == 0 && fmt == 4) { rank = 2; sym = true; }
        else if (plat == 0 && fmt == 6) rank = 1;
        if (rank >= 0) cands.push_back({rank, off, fmt, sym});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.rank > b.rank; });
    for (auto& k : cands) {
        Mapping tmp;
        bool    ok = k.fmt == 12 ? cmap_format12(c, k.off, tmp, ng) : k.fmt == 4 ? cmap_format4(c, k.off, tmp, ng, k.symbol) : cmap_format6(c, k.off, tmp, ng);
        if (ok && !tmp.empty()) { m = std::move(tmp); return true; }
    }
    return false;
}

std::string write_cmap(const Mapping& m) {
    std::string groups;
    uint32_t    n = 0;
    auto        it = m.begin();
    while (it != m.end()) {
        uint32_t s = it->first, g = it->second, e = s;
        auto     nx = std::next(it);
        while (nx != m.end() && nx->first == e + 1 && nx->second == g + (e + 1 - s)) { e++; nx = std::next(nx); }
        w32(groups, s);
        w32(groups, e);
        w32(groups, g);
        n++;
        it = nx;
    }
    std::string c;
    w16(c, 0);  // version
    w16(c, 1);  // one subtable
    w16(c, 3);  // Windows
    w16(c, 10); // Unicode full repertoire
    w32(c, 12);
    w16(c, 12); // format 12
    w16(c, 0);
    w32(c, 16 + static_cast<uint32_t>(groups.size()));
    w32(c, 0);
    w32(c, n);
    c += groups;
    return c;
}

// ------------------------------------------------------------------ the rebuilt font
bool rebuild(Tables& t, SanitizedFont& out, std::string* err) {
    for (uint32_t need : {kHead, kHhea, kMaxp, kCmap, kHmtx}) if (!t.count(need)) return fail(err, "font lacks a required table");
    if (!t.count(kGlyf) || !t.count(kLoca)) return fail(err, "no TrueType outlines (glyf)");
    const std::string& head = t[kHead];
    const std::string& hhea = t[kHhea];
    const std::string& maxp = t[kMaxp];
    if (head.size() < 54 || hhea.size() < 36 || maxp.size() < 6) return fail(err, "truncated head / hhea / maxp");
    uint16_t upem = rd16(head, 18);
    if (upem < 16 || upem > 16384) return fail(err, "bad unitsPerEm");
    int16_t loc_fmt = rs16(head, 50);
    if (loc_fmt != 0 && loc_fmt != 1) return fail(err, "bad loca format");
    uint32_t ng = rd16(maxp, 4);
    if (ng == 0) return fail(err, "no glyphs");
    const std::string& loca = t[kLoca];
    const std::string& glyf = t[kGlyf];
    size_t             esz  = loc_fmt ? 4 : 2;
    if (loca.size() < (static_cast<size_t>(ng) + 1) * esz) return fail(err, "loca too short");
    std::vector<std::string> glyphs(ng);
    std::vector<std::vector<uint16_t>> refs(ng);
    size_t dropped = 0;
    for (uint32_t i = 0; i < ng; i++) {
        uint64_t a = loc_fmt ? rd32(loca, i * 4) : 2ull * rd16(loca, i * 2);
        uint64_t b = loc_fmt ? rd32(loca, (i + 1) * 4) : 2ull * rd16(loca, (i + 1) * 2);
        if (b < a || b > glyf.size()) { dropped++; continue; }
        if (b - a == 0) continue;
        if (b - a < 10) { dropped++; continue; }
        std::string g = glyf.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
        if (!glyph_ok(g, ng, refs[i])) { dropped++; refs[i].clear(); continue; }
        glyphs[i] = std::move(g);
    }
    // composites: no cycles, no deeper than kMaxCompDepth (stb_truetype recurses into components)
    std::vector<int> depth(ng, -1); // -1 unknown, -2 visiting / bad
    std::function<int(uint32_t, int)> visit = [&](uint32_t g, int lvl) -> int {
        if (depth[g] >= 0) return depth[g];
        if (depth[g] == -2 || lvl > kMaxCompDepth) return 1000;
        if (refs[g].empty()) return depth[g] = 0;
        depth[g] = -2;
        int d = 0;
        for (uint16_t c : refs[g]) d = std::max(d, visit(c, lvl + 1) + 1);
        depth[g] = std::min(d, 1000);
        return depth[g];
    };
    for (uint32_t i = 0; i < ng; i++)
        if (!refs[i].empty() && visit(i, 0) > kMaxCompDepth) {
            glyphs[i].clear();
            dropped++;
        }
    uint16_t nh = rd16(hhea, 34);
    if (nh == 0) return fail(err, "bad hhea");
    if (nh > ng) nh = static_cast<uint16_t>(ng);
    size_t hm_len = 4 * static_cast<size_t>(nh) + 2 * static_cast<size_t>(ng - nh);
    if (t[kHmtx].size() < hm_len) return fail(err, "hmtx too short");
    Mapping m;
    if (!read_cmap(t[kCmap], ng, m)) return fail(err, "no usable character map");

    // ---- write
    Tables o;
    std::string nglyf, nloca;
    for (uint32_t i = 0; i < ng; i++) {
        w32(nloca, static_cast<uint32_t>(nglyf.size()));
        nglyf += glyphs[i];
        if (nglyf.size() & 3) nglyf.append(4 - (nglyf.size() & 3), '\0');
    }
    w32(nloca, static_cast<uint32_t>(nglyf.size()));
    std::string nhead = head.substr(0, 54);
    std::memset(&nhead[8], 0, 4); // checkSumAdjustment
    put16(nhead, 50, 1);          // long loca
    put16(nhead, 52, 0);          // glyphDataFormat
    std::string nhhea = hhea.substr(0, 36);
    put16(nhhea, 34, nh);
    std::string nmaxp;
    w32(nmaxp, 0x00005000);
    w16(nmaxp, ng);
    o[kCmap] = write_cmap(m);
    o[kGlyf] = std::move(nglyf);
    o[kHead] = std::move(nhead);
    o[kHhea] = std::move(nhhea);
    o[kHmtx] = t[kHmtx].substr(0, hm_len);
    o[kLoca] = std::move(nloca);
    o[kMaxp] = std::move(nmaxp);

    std::string f;
    uint16_t    num = static_cast<uint16_t>(o.size());
    uint16_t    pow2 = 1, sel = 0;
    while (pow2 * 2 <= num) { pow2 *= 2; sel++; }
    w32(f, 0x00010000);
    w16(f, num);
    w16(f, pow2 * 16);
    w16(f, sel);
    w16(f, num * 16 - pow2 * 16);
    size_t off = 12 + 16 * static_cast<size_t>(num);
    std::string body;
    for (auto& kv : o) {
        std::string d = kv.second;
        uint32_t    sum = 0;
        std::string pad = d;
        if (pad.size() & 3) pad.append(4 - (pad.size() & 3), '\0');
        for (size_t i = 0; i < pad.size(); i += 4) sum += rd32(pad, i);
        w32(f, kv.first);
        w32(f, sum);
        w32(f, static_cast<uint32_t>(off + body.size()));
        w32(f, static_cast<uint32_t>(d.size()));
        body += pad;
    }
    f += body;
    out.data.assign(f.begin(), f.end());
    out.variable       = t.count(kFvar) != 0;
    out.glyphs         = ng;
    out.mapped         = m.size();
    out.dropped_glyphs = dropped;
    return true;
}

} // namespace

bool woff2_supported() {
#ifdef QUILL_HAVE_BROTLI
    return true;
#else
    return false;
#endif
}

bool sanitize_font(const unsigned char* in, size_t n, SanitizedFont& out, std::string* err) {
    out = SanitizedFont();
    if (!in || n < 12) return fail(err, "not a font");
    if (n > kMaxInput) return fail(err, "font file too large");
    Tables t;
    if (std::memcmp(in, "wOFF", 4) == 0) {
        if (!read_woff(in, n, t, err)) return false;
    } else if (std::memcmp(in, "wOF2", 4) == 0) {
#ifdef QUILL_HAVE_BROTLI
        if (!read_woff2(in, n, t, err)) return false;
#else
        return fail(err, "WOFF2 needs Quill built with Brotli");
#endif
    } else if (!read_sfnt(in, n, t, err)) {
        return false;
    }
    return rebuild(t, out, err);
}

} // namespace quill
