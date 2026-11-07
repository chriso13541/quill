#include "image.h"

#include "svg.h"
#include "url.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef QUILL_HAVE_WEBP
#include <webp/decode.h>
#endif

// Declarations only; the implementation lives in stb_impl.cpp.
extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file,
                                     int desired_channels);
int            stbi_info_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp);
unsigned char* stbi_load_gif_from_memory(const unsigned char* buffer, int len, int** delays, int* x, int* y, int* z, int* comp,
                                         int req_comp);
void           stbi_image_free(void* retval_from_stbi_load);
}

namespace quill {

bool webp_supported() {
#ifdef QUILL_HAVE_WEBP
    return true;
#else
    return false;
#endif
}

namespace {

std::shared_ptr<Image> decode_svg(const unsigned char* data, size_t len, size_t max_pixels) {
    auto doc = SvgDoc::parse(data, len);
    if (!doc) return nullptr;
    // Base raster at the intrinsic size (what layout uses), clamped to something sane.
    double w = doc->width(), h = doc->height();
    double k = 1.0;
    if (w > 4096 || h > 4096) k = std::min(4096.0 / w, 4096.0 / h);
    if (w * h * k * k > 8e6) k = std::sqrt(8e6 / (w * h));
    int iw = std::max(1, static_cast<int>(std::lround(w * k)));
    int ih = std::max(1, static_cast<int>(std::lround(h * k)));
    if (static_cast<uint64_t>(iw) * ih > max_pixels) return nullptr;
    auto img = std::make_shared<Image>();
    img->w   = iw;
    img->h   = ih;
    if (!doc->render(iw, ih, img->px)) return nullptr;
    img->svg = doc;
    return img;
}

#ifdef QUILL_HAVE_WEBP
std::shared_ptr<Image> decode_webp(const unsigned char* data, size_t len, size_t max_pixels) {
    int w = 0, h = 0;
    if (!WebPGetInfo(data, len, &w, &h)) return nullptr;
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > max_pixels) return nullptr;
    uint8_t* p = WebPDecodeRGBA(data, len, &w, &h);
    if (!p) return nullptr;
    auto img = std::make_shared<Image>();
    img->w   = w;
    img->h   = h;
    img->px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < img->px.size(); i++) {
        const unsigned char* s = p + i * 4;
        img->px[i]             = (static_cast<uint32_t>(s[3]) << 24) | (static_cast<uint32_t>(s[0]) << 16) |
                     (static_cast<uint32_t>(s[1]) << 8) | s[2];
    }
    WebPFree(p);
    return img;
}
#endif

// Counts the image frames of a GIF by walking its block structure (no decoding), so a huge animation can be
// refused before stb_image allocates every frame. Returns 0 if the stream is malformed before the first frame.
int gif_frame_count(const unsigned char* d, size_t n) {
    if (n < 13 || std::memcmp(d, "GIF8", 4) != 0) return 0;
    size_t p = 13;
    if (d[10] & 0x80) p += 3u * (1u << ((d[10] & 7) + 1)); // global colour table
    auto skip_sub_blocks = [&]() {
        while (p < n) {
            size_t len = d[p++];
            if (len == 0) return true;
            p += len;
        }
        return false;
    };
    int frames = 0;
    while (p < n && frames < 100000) {
        unsigned char b = d[p++];
        if (b == 0x3B) break; // trailer
        if (b == 0x21) {      // extension
            if (p >= n) break;
            ++p;
            if (!skip_sub_blocks()) break;
        } else if (b == 0x2C) { // image descriptor
            if (p + 9 > n) break;
            unsigned char flags = d[p + 8];
            p += 9;
            if (flags & 0x80) p += 3u * (1u << ((flags & 7) + 1)); // local colour table
            if (p >= n) break;
            ++p; // LZW minimum code size
            ++frames;
            if (!skip_sub_blocks()) break;
        } else {
            break; // junk: stop counting, decode what we have
        }
    }
    return frames;
}

std::shared_ptr<Image> argb_image(const unsigned char* rgba, int w, int h) {
    auto img = std::make_shared<Image>();
    img->w   = w;
    img->h   = h;
    img->px.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < img->px.size(); i++) {
        const unsigned char* s = rgba + i * 4;
        img->px[i]             = (static_cast<uint32_t>(s[3]) << 24) | (static_cast<uint32_t>(s[0]) << 16) |
                     (static_cast<uint32_t>(s[1]) << 8) | s[2];
    }
    return img;
}

// All frames of an animated GIF, or null to fall back to the first frame only (still GIF, too big, decode error).
std::shared_ptr<Image> decode_gif_animation(const unsigned char* data, size_t len, int w, int h) {
    constexpr uint64_t kMaxAnimPixels = 8u * 1000 * 1000; // all frames together (32 MB): plenty for the old web's GIFs
    constexpr int      kMaxFrames     = 1000;
    int nframes = gif_frame_count(data, len);
    if (nframes < 2 || nframes > kMaxFrames) return nullptr;
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * static_cast<uint64_t>(nframes) > kMaxAnimPixels) return nullptr;
    int*           delays = nullptr;
    int            x = 0, y = 0, z = 0, comp = 0;
    unsigned char* p = stbi_load_gif_from_memory(data, static_cast<int>(len), &delays, &x, &y, &z, &comp, 4);
    if (!p || z < 2 || x != w || y != h) {
        if (p) stbi_image_free(p);
        if (delays) stbi_image_free(delays);
        return nullptr;
    }
    size_t frame_bytes = static_cast<size_t>(w) * h * 4;
    auto   img         = argb_image(p, w, h);
    for (int f = 0; f < z; f++) {
        if (f > 0) img->frames.push_back(argb_image(p + frame_bytes * static_cast<size_t>(f), w, h));
        // Like every browser: delays of 0-10 ms (meant as "as fast as possible" in 1996) play at 100 ms.
        int d = delays ? delays[f] : 100;
        if (d <= 10) d = 100;
        img->delays_ms.push_back(d);
        img->total_ms += d;
    }
    stbi_image_free(p);
    if (delays) stbi_image_free(delays);
    return img;
}

bool is_webp(const unsigned char* d, size_t n) { return n >= 16 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0; }

} // namespace

namespace {
uint32_t le16(const unsigned char* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }
uint32_t le32(const unsigned char* p) { return le16(p) | (le16(p + 2) << 16); }
void     put_le32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) s[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF); }

bool is_ico(const unsigned char* d, size_t n) { return n >= 22 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || d[2] == 2) && d[3] == 0 && le16(d + 4) > 0; }

// .ico / .cur: the entry nearest to 32 px (PNG inside, or a Windows DIB with its AND transparency mask).
std::shared_ptr<Image> decode_ico(const unsigned char* d, size_t n) {
    uint32_t count = le16(d + 4);
    if (count > 256 || 6 + 16ull * count > n) return nullptr;
    int    best = -1;
    double best_score = 1e18;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = d + 6 + 16 * i;
        int      w = e[0] ? e[0] : 256;
        uint32_t bpp = le16(e + 6), size = le32(e + 8), off = le32(e + 12);
        if (off > n || size > n - off || size < 40) continue;
        double score = w >= 32 ? (w - 32) : 1000 + (32 - w); // smallest at least 32 px, else the biggest
        score -= bpp / 64.0;
        if (score < best_score) { best_score = score; best = static_cast<int>(i); }
    }
    if (best < 0) return nullptr;
    const unsigned char* e = d + 6 + 16 * best;
    const unsigned char* p = d + le32(e + 12);
    size_t               len = le32(e + 8);
    if (len >= 8 && std::memcmp(p, "\x89PNG", 4) == 0) {
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load_from_memory(p, static_cast<int>(len), &w, &h, &c, 4);
        if (!px) return nullptr;
        std::shared_ptr<Image> img = (w > 0 && h > 0 && w <= 1024 && h <= 1024) ? argb_image(px, w, h) : nullptr;
        stbi_image_free(px);
        return img;
    }
    uint32_t hdr = le32(p);
    if (hdr < 40 || hdr > len) return nullptr;
    int32_t  w   = static_cast<int32_t>(le32(p + 4)), h2 = static_cast<int32_t>(le32(p + 8));
    uint32_t bpp = le16(p + 14), comp = le32(p + 16), used = le32(p + 32);
    if (w <= 0 || w > 256 || h2 <= 0 || h2 > 512 || (comp != 0 && comp != 3)) return nullptr;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return nullptr;
    int32_t  h = h2 / 2;
    if (h <= 0) return nullptr;
    uint32_t pal = bpp <= 8 ? (used ? std::min<uint32_t>(used, 256) : (1u << bpp)) * 4 : 0;
    uint32_t masks = comp == 3 && hdr == 40 ? 12 : 0;
    size_t   xor_stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4, and_stride = ((static_cast<size_t>(w) + 31) / 32) * 4;
    size_t   pix = hdr + masks + pal;
    if (pix + xor_stride * h > len) return nullptr;
    // a .bmp file around the DIB, with the real height (an icon's header counts the mask too)
    std::string bmp(14, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    put_le32(bmp, 2, static_cast<uint32_t>(14 + len));
    put_le32(bmp, 10, static_cast<uint32_t>(14 + pix));
    bmp.append(reinterpret_cast<const char*>(p), len);
    put_le32(bmp, 14 + 8, static_cast<uint32_t>(h));
    int  ow = 0, oh = 0, oc = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bmp.data()), static_cast<int>(bmp.size()), &ow, &oh, &oc, 4);
    if (!px) return nullptr;
    if (ow != w || oh != h) { stbi_image_free(px); return nullptr; }
    bool has_alpha = false; // from the file: stb_image turns an all-zero alpha channel opaque
    if (bpp == 32)
        for (size_t i = 0; i < static_cast<size_t>(w) * h && !has_alpha; i++) has_alpha = p[pix + i * 4 + 3] != 0;
    size_t mask_at = pix + xor_stride * h;
    if (!has_alpha && mask_at + and_stride * h <= len) { // the 1-bit AND mask: set = transparent (rows bottom-up)
        for (int y = 0; y < h; y++) {
            const unsigned char* row = p + mask_at + and_stride * static_cast<size_t>(h - 1 - y);
            for (int x = 0; x < w; x++) {
                bool clear = (row[x >> 3] >> (7 - (x & 7))) & 1;
                px[(static_cast<size_t>(y) * w + x) * 4 + 3] = clear ? 0 : 255;
            }
        }
    }
    auto img = argb_image(px, w, h);
    stbi_image_free(px);
    return img;
}
} // namespace

std::shared_ptr<Image> decode_image(const unsigned char* data, size_t len, size_t max_pixels) {
    if (!data || len == 0 || len > 0x7fffffff) return nullptr;
    if (is_ico(data, len)) return decode_ico(data, len);
    if (is_webp(data, len)) {
#ifdef QUILL_HAVE_WEBP
        return decode_webp(data, len, max_pixels);
#else
        return nullptr;
#endif
    }
    if (SvgDoc::sniff(data, len)) return decode_svg(data, len, max_pixels);
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(len), &w, &h, &n)) return nullptr;
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > max_pixels) return nullptr;
    if (len >= 6 && std::memcmp(data, "GIF8", 4) == 0)
        if (auto anim = decode_gif_animation(data, len, w, h)) return anim;
    unsigned char* p = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &n, 4);
    if (!p) return nullptr;
    auto img = argb_image(p, w, h);
    stbi_image_free(p);
    return img;
}

const Image& Image::frame_at(long long t_ms, int* next_ms) const {
    if (!animated() || delays_ms.size() != frames.size() + 1) {
        if (next_ms) *next_ms = -1;
        return *this;
    }
    long long t = t_ms < 0 ? 0 : t_ms % total_ms;
    for (size_t i = 0; i < delays_ms.size(); i++) {
        if (t < delays_ms[i]) {
            if (next_ms) *next_ms = static_cast<int>(delays_ms[i] - t);
            return i == 0 ? *this : *frames[i - 1];
        }
        t -= delays_ms[i];
    }
    if (next_ms) *next_ms = delays_ms[0];
    return *this;
}

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool decode_data_uri(const std::string& uri, std::string& mime, std::string& bytes) {
    if (to_lower(uri.substr(0, 5)) != "data:") return false;
    size_t comma = uri.find(',');
    if (comma == std::string::npos) return false;
    std::string meta    = uri.substr(5, comma - 5);
    std::string payload = uri.substr(comma + 1);
    bool        b64     = false;
    size_t      semi    = meta.find(';');
    mime                = to_lower(meta.substr(0, semi));
    if (semi != std::string::npos && to_lower(meta.substr(semi)).find(";base64") != std::string::npos) b64 = true;
    bytes.clear();
    if (!b64) {
        bytes = percent_decode(payload, false);
        return true;
    }
    payload = percent_decode(payload, false);
    uint32_t acc = 0;
    int      bits = 0;
    for (unsigned char c : payload) {
        if (c == '=') break;
        int v = b64val(c);
        if (v < 0) continue; // skip whitespace / junk
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

namespace {

inline uint32_t pack(float a, float r, float g, float b) {
    auto cl = [](float v) { return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, v + 0.5f))); };
    return (cl(a) << 24) | (cl(r) << 16) | (cl(g) << 8) | cl(b);
}

void scale_area(const Image& s, Image& d) {
    for (int dy = 0; dy < d.h; dy++) {
        int y0 = static_cast<int>(static_cast<int64_t>(dy) * s.h / d.h);
        int y1 = static_cast<int>(static_cast<int64_t>(dy + 1) * s.h / d.h);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > s.h) y1 = s.h;
        for (int dx = 0; dx < d.w; dx++) {
            int x0 = static_cast<int>(static_cast<int64_t>(dx) * s.w / d.w);
            int x1 = static_cast<int>(static_cast<int64_t>(dx + 1) * s.w / d.w);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > s.w) x1 = s.w;
            uint64_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int y = y0; y < y1; y++) {
                const uint32_t* row = &s.px[static_cast<size_t>(y) * s.w];
                for (int x = x0; x < x1; x++) {
                    uint32_t p = row[x];
                    uint32_t a = p >> 24;
                    sa += a;
                    sr += ((p >> 16) & 255) * a;
                    sg += ((p >> 8) & 255) * a;
                    sb += (p & 255) * a;
                    cnt++;
                }
            }
            if (sa == 0) {
                d.px[static_cast<size_t>(dy) * d.w + dx] = 0;
            } else {
                d.px[static_cast<size_t>(dy) * d.w + dx] =
                    pack(static_cast<float>(sa) / cnt, static_cast<float>(sr) / sa, static_cast<float>(sg) / sa,
                         static_cast<float>(sb) / sa);
            }
        }
    }
}

void scale_bilinear(const Image& s, Image& d) {
    for (int dy = 0; dy < d.h; dy++) {
        float fy = (dy + 0.5f) * s.h / d.h - 0.5f;
        int   y0 = static_cast<int>(std::floor(fy));
        float wy = fy - y0;
        int   y1 = std::min(std::max(y0 + 1, 0), s.h - 1);
        y0       = std::min(std::max(y0, 0), s.h - 1);
        for (int dx = 0; dx < d.w; dx++) {
            float fx = (dx + 0.5f) * s.w / d.w - 0.5f;
            int   x0 = static_cast<int>(std::floor(fx));
            float wx = fx - x0;
            int   x1 = std::min(std::max(x0 + 1, 0), s.w - 1);
            x0       = std::min(std::max(x0, 0), s.w - 1);
            uint32_t p00 = s.px[static_cast<size_t>(y0) * s.w + x0], p10 = s.px[static_cast<size_t>(y0) * s.w + x1];
            uint32_t p01 = s.px[static_cast<size_t>(y1) * s.w + x0], p11 = s.px[static_cast<size_t>(y1) * s.w + x1];
            float    w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
            auto     ch = [&](int shift) {
                return w00 * ((p00 >> shift) & 255) + w10 * ((p10 >> shift) & 255) + w01 * ((p01 >> shift) & 255) +
                       w11 * ((p11 >> shift) & 255);
            };
            d.px[static_cast<size_t>(dy) * d.w + dx] = pack(ch(24), ch(16), ch(8), ch(0));
        }
    }
}

} // namespace

const Image& Image::scaled(int tw, int th) const {
    if (tw == w && th == h) return *this;
    for (auto& c : cache_)
        if (c->w == tw && c->h == th) return *c;
    auto out = std::make_unique<Image>();
    out->w   = std::max(1, tw);
    out->h   = std::max(1, th);
    out->px.assign(static_cast<size_t>(out->w) * out->h, 0);
    if (svg && out->w <= 4096 && out->h <= 4096 && svg->render(out->w, out->h, out->px)) {
        // vector source: rendered at the requested size, no resampling needed
    } else if (out->w < w || out->h < h) scale_area(*this, *out);
    else scale_bilinear(*this, *out);
    if (cache_.size() >= 6) cache_.erase(cache_.begin());
    cache_.push_back(std::move(out));
    return *cache_.back();
}

} // namespace quill
