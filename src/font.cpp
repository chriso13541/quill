#include "font.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace quill {

uint32_t utf8_next(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    auto          cont = [&](size_t k) -> int {
        if (i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80) return static_cast<unsigned char>(s[i + k]) & 0x3F;
        return -1;
    };
    if (c < 0x80) {
        i++;
        return c;
    }
    if (c >= 0xC2 && c < 0xE0) {
        int a = cont(1);
        if (a >= 0) {
            i += 2;
            return ((c & 0x1Fu) << 6) | static_cast<uint32_t>(a);
        }
    } else if (c >= 0xE0 && c < 0xF0) {
        int a = cont(1), b = cont(2);
        if (a >= 0 && b >= 0) {
            i += 3;
            return ((c & 0x0Fu) << 12) | (static_cast<uint32_t>(a) << 6) | static_cast<uint32_t>(b);
        }
    } else if (c >= 0xF0 && c < 0xF5) {
        int a = cont(1), b = cont(2), d = cont(3);
        if (a >= 0 && b >= 0 && d >= 0) {
            i += 4;
            return ((c & 0x07u) << 18) | (static_cast<uint32_t>(a) << 12) | (static_cast<uint32_t>(b) << 6) | static_cast<uint32_t>(d);
        }
    }
    i++;
    return 0xFFFD;
}

namespace {

struct Face {
    int                        id = 0;
    std::vector<unsigned char> data;
    std::shared_ptr<const std::vector<unsigned char>> shared; // web fonts: sanitized data (shared between pages)
    stbtt_fontinfo             info;
    bool                       ok = false;
};

using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;
bool in_ranges(const Ranges& r, uint32_t cp) {
    if (r.empty()) return true;
    for (auto& x : r)
        if (cp >= x.first && cp <= x.second) return true;
    return false;
}

// The names of a CSS font-family list, lower-cased and unquoted.
std::vector<std::string> family_names(const std::string& css) {
    std::vector<std::string> out;
    size_t                   pos = 0;
    while (pos <= css.size()) {
        size_t comma = css.find(',', pos);
        if (comma == std::string::npos) comma = css.size();
        std::string tok = css.substr(pos, comma - pos);
        pos             = comma + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '"' || tok.front() == '\'')) tok.erase(0, 1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '"' || tok.back() == '\'')) tok.pop_back();
        for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

struct Glyph {
    int                  w = 0, h = 0, xoff = 0, yoff = 0;
    float                adv = 0;
    std::vector<uint8_t> bmp;
};

struct SetNames {
    const char *r, *b, *i, *bi;
};

const SetNames kSans[] = {
    {"LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf", "LiberationSans-BoldItalic.ttf"},
    {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"},
    {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf"},
    {"NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSans-Italic.ttf", "NotoSans-BoldItalic.ttf"},
    {"Roboto-Regular.ttf", "Roboto-Bold.ttf", "Roboto-Italic.ttf", "Roboto-BoldItalic.ttf"},
    {"FreeSans.ttf", "FreeSansBold.ttf", "FreeSansOblique.ttf", "FreeSansBoldOblique.ttf"},
};
const SetNames kSerif[] = {
    {"LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf", "LiberationSerif-Italic.ttf", "LiberationSerif-BoldItalic.ttf"},
    {"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"},
    {"DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf", "DejaVuSerif-Italic.ttf", "DejaVuSerif-BoldItalic.ttf"},
    {"NotoSerif-Regular.ttf", "NotoSerif-Bold.ttf", "NotoSerif-Italic.ttf", "NotoSerif-BoldItalic.ttf"},
    {"FreeSerif.ttf", "FreeSerifBold.ttf", "FreeSerifItalic.ttf", "FreeSerifBoldItalic.ttf"},
};
const SetNames kMono[] = {
    {"LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf", "LiberationMono-Italic.ttf", "LiberationMono-BoldItalic.ttf"},
    {"consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"},
    {"cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"},
    {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-BoldOblique.ttf"},
    {"NotoSansMono-Regular.ttf", "NotoSansMono-Bold.ttf", nullptr, nullptr},
    {"FreeMono.ttf", "FreeMonoBold.ttf", "FreeMonoOblique.ttf", "FreeMonoBoldOblique.ttf"},
};

// Broad coverage fonts tried (lazily, in order) when the primary face lacks a glyph.
const char* kFallbacks[] = {"DejaVuSans.ttf",   "NotoSans-Regular.ttf",       "FreeSans.ttf",
                            "segoeui.ttf",      "seguisym.ttf",               "NotoSansSymbols-Regular.ttf",
                            "NotoSansSymbols2-Regular.ttf", "NotoSansCJK-Regular.ttc", "wqy-microhei.ttc",
                            "wqy-zenhei.ttc",   "msyh.ttc",                   "msgothic.ttc",
                            "malgun.ttf",       "unifont.ttf"};

enum FamilyClass { Sans = 0, Serif = 1, Mono = 2 };

FamilyClass classify_family(const std::string& css) {
    std::string l = css;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t pos = 0;
    while (pos <= l.size()) {
        size_t comma = l.find(',', pos);
        if (comma == std::string::npos) comma = l.size();
        std::string tok = l.substr(pos, comma - pos);
        pos             = comma + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '"' || tok.front() == '\'')) tok.erase(0, 1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '"' || tok.back() == '\'')) tok.pop_back();
        auto has = [&](const char* s) { return tok.find(s) != std::string::npos; };
        if (has("mono") || has("courier") || has("consolas") || has("menlo") || has("fixed") || tok == "ui-monospace") return Mono;
        if (tok == "serif" || has("times") || has("georgia") || has("garamond") || has("palatino") || has("cambria") ||
            has("book antiqua") || has("charter"))
            return Serif;
        if (tok == "sans-serif" || tok == "system-ui" || tok == "ui-sans-serif" || has("arial") || has("helvetica") ||
            has("verdana") || has("tahoma") || has("segoe") || has("roboto") || has("inter") || has("open sans") ||
            has("lucida grande") || has("trebuchet") || has("noto sans") || has("sans"))
            return Sans;
    }
    return Sans;
}

} // namespace

// ---------------------------------------------------------------------------
// FontManager
// ---------------------------------------------------------------------------
struct FontManager::Impl {
    std::mutex                                    mu;
    std::string                                   extra_dir;
    bool                                          indexed = false;
    std::unordered_map<std::string, std::string>  index; // lowercase file name -> path
    std::unordered_map<std::string, std::unique_ptr<Face>> faces; // by path
    int                                           next_id = 1;
    std::vector<std::string>                      fallback_paths;
    bool                                          fallback_built = false;
    std::vector<Face*>                            fallback_faces; // lazily loaded, parallel to fallback_paths
    std::unordered_map<std::string, std::unique_ptr<Font>> fonts;
    bool                                          warned = false;

    struct WebEntry {
        std::string           family; // lower case
        int                   wlo = 400, whi = 400;
        bool                  italic = false, variable = false;
        Ranges                ranges;
        std::unique_ptr<Face> face;
    };
    struct Scope {
        std::vector<WebEntry>              faces;
        std::vector<std::unique_ptr<Font>> retired; // fonts picked before a face arrived: still referenced until the page goes
    };
    std::unordered_map<uint64_t, Scope> scopes;
    uint64_t                            next_scope = 1;

    void ensure_index() {
        if (indexed) return;
        indexed = true;
        std::vector<std::string> dirs;
        if (!extra_dir.empty()) dirs.push_back(extra_dir);
        if (const char* e = std::getenv("QUILL_FONT_DIR")) dirs.push_back(e);
#ifdef _WIN32
        if (const char* w = std::getenv("WINDIR")) dirs.push_back(std::string(w) + "\\Fonts");
        else dirs.push_back("C:\\Windows\\Fonts");
        if (const char* l = std::getenv("LOCALAPPDATA")) dirs.push_back(std::string(l) + "\\Microsoft\\Windows\\Fonts");
#else
        dirs.push_back("/usr/share/fonts");
        dirs.push_back("/usr/local/share/fonts");
        dirs.push_back("/Library/Fonts");
        dirs.push_back("/System/Library/Fonts");
        if (const char* h = std::getenv("HOME")) {
            dirs.push_back(std::string(h) + "/.fonts");
            dirs.push_back(std::string(h) + "/.local/share/fonts");
        }
#endif
        size_t seen = 0;
        for (const auto& d : dirs) {
            std::error_code ec;
            if (!fs::is_directory(d, ec)) continue;
            fs::recursive_directory_iterator it(d, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec), end;
            while (!ec && it != end) {
                if (++seen > 200000) return;
                std::error_code e2;
                if (it->is_regular_file(e2)) {
                    std::string name = it->path().filename().string();
                    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    index.emplace(name, it->path().string()); // first hit wins (extra dir has priority)
                }
                it.increment(ec);
            }
        }
    }

    std::string find(const char* name) {
        if (!name) return {};
        std::string l = name;
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = index.find(l);
        return it == index.end() ? std::string() : it->second;
    }

    Face* load(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = faces.find(path);
        if (it != faces.end()) return it->second->ok ? it->second.get() : nullptr;
        auto f = std::make_unique<Face>();
        f->id  = next_id++;
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            std::streamoff n = in.tellg();
            in.seekg(0);
            if (n > 0 && n < (200 << 20)) {
                f->data.resize(static_cast<size_t>(n));
                in.read(reinterpret_cast<char*>(f->data.data()), n);
                int off = stbtt_GetFontOffsetForIndex(f->data.data(), 0);
                if (off >= 0 && stbtt_InitFont(&f->info, f->data.data(), off)) f->ok = true;
            }
        }
        Face* raw = f->ok ? f.get() : nullptr;
        faces[path] = std::move(f);
        return raw;
    }

    // Picks the first family set whose regular face exists, then the best style.
    Face* pick(FamilyClass fc, bool bold, bool italic, bool& synth_b, bool& synth_i) {
        const SetNames* sets;
        size_t          n;
        if (fc == Serif) { sets = kSerif; n = sizeof(kSerif) / sizeof(kSerif[0]); }
        else if (fc == Mono) { sets = kMono; n = sizeof(kMono) / sizeof(kMono[0]); }
        else { sets = kSans; n = sizeof(kSans) / sizeof(kSans[0]); }
        synth_b = synth_i = false;
        for (size_t k = 0; k < n; k++) {
            Face* r = load(find(sets[k].r));
            if (!r) continue;
            Face* b  = load(find(sets[k].b));
            Face* i  = load(find(sets[k].i));
            Face* bi = load(find(sets[k].bi));
            if (bold && italic) {
                if (bi) return bi;
                if (b) { synth_i = true; return b; }
                if (i) { synth_b = true; return i; }
                synth_b = synth_i = true;
                return r;
            }
            if (bold) {
                if (b) return b;
                synth_b = true;
                return r;
            }
            if (italic) {
                if (i) return i;
                synth_i = true;
                return r;
            }
            return r;
        }
        // Last resort: any fallback font at all.
        for (const char* fbn : kFallbacks) {
            Face* f = load(find(fbn));
            if (f) {
                synth_b = bold;
                synth_i = italic;
                return f;
            }
        }
        return nullptr;
    }

    Face* fallback(size_t i) {
        std::lock_guard<std::mutex> lk(mu);
        ensure_index();
        if (!fallback_built) {
            fallback_built = true;
            for (const char* n : kFallbacks) {
                std::string p = find(n);
                if (!p.empty()) fallback_paths.push_back(p);
            }
            fallback_faces.assign(fallback_paths.size(), nullptr);
        }
        if (i >= fallback_paths.size()) return nullptr;
        if (!fallback_faces[i]) fallback_faces[i] = load(fallback_paths[i]);
        return fallback_faces[i];
    }
};

FontManager::FontManager() : impl_(new Impl) {}
FontManager::~FontManager() = default;

FontManager& FontManager::instance() {
    static FontManager* m = new FontManager(); // intentionally leaked: avoids exit-order problems with worker threads
    return *m;
}

void FontManager::set_extra_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->indexed) impl_->extra_dir = dir;
}

bool FontManager::have_fonts() {
    bool a, b;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->ensure_index();
    return impl_->pick(Sans, false, false, a, b) != nullptr;
}

// ---------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------
struct Font::Impl {
    std::mutex                                 mu;
    Face*                                      primary = nullptr;
    std::vector<std::pair<Face*, Ranges>> web; // web font faces, tried first (primary is web[0] then); ranges copied:
                                                       // the scope's face list may grow (and move) while this font is in use
    Face*                                      system = nullptr; // with web faces: the system font behind them
    bool                                       synth_bold = false, synth_italic = false;
    float                                      size = 16;
    std::unordered_map<int, float>             scales;
    std::unordered_map<uint64_t, Glyph>        glyphs;
    std::unordered_map<uint32_t, Face*>        face_for_cp;
    FontManager::Impl*                         mgr = nullptr;
    // Measuring is the hot path of layout: stb_truetype's kerning lookup searches the font's tables every time, and
    // layout measures the same words over and over (intrinsic widths, then line breaking).
    std::unordered_map<uint64_t, float>        kerns;  // (face, previous cp, cp) -> pixels
    std::unordered_map<std::string, float>     widths; // whole strings (mostly single words)

    float kern(Face* f, uint32_t prev, uint32_t cp) {
        uint64_t key = (static_cast<uint64_t>(f->id) << 42) | (static_cast<uint64_t>(prev & 0x1FFFFF) << 21) | (cp & 0x1FFFFF);
        auto     it  = kerns.find(key);
        if (it != kerns.end()) return it->second;
        if (kerns.size() > 200000) kerns.clear();
        float k = stbtt_GetCodepointKernAdvance(&f->info, static_cast<int>(prev), static_cast<int>(cp)) * scale_for(f);
        kerns.emplace(key, k);
        return k;
    }

    float scale_for(Face* f) {
        auto it = scales.find(f->id);
        if (it != scales.end()) return it->second;
        float s = stbtt_ScaleForMappingEmToPixels(&f->info, size);
        scales[f->id] = s;
        return s;
    }

    Face* choose_face(uint32_t cp) {
        auto it = face_for_cp.find(cp);
        if (it != face_for_cp.end()) return it->second;
        for (auto& w : web) // the page's own font (one face per unicode-range subset)
            if (in_ranges(w.second, cp) && stbtt_FindGlyphIndex(&w.first->info, static_cast<int>(cp))) {
                face_for_cp[cp] = w.first;
                return w.first;
            }
        Face* r = web.empty() ? primary : system ? system : primary;
        if (!r || !stbtt_FindGlyphIndex(&r->info, static_cast<int>(cp))) {
            for (size_t i = 0;; i++) {
                Face* f = mgr->fallback(i);
                if (!f) break;
                if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(cp))) {
                    r = f;
                    break;
                }
            }
        }
        if (r && !stbtt_FindGlyphIndex(&r->info, static_cast<int>(cp)) && !web.empty()) r = primary; // nobody has it
        face_for_cp[cp] = r;
        return r;
    }

    const Glyph& glyph(Face* f, uint32_t cp) {
        uint64_t key = (static_cast<uint64_t>(f->id) << 32) | cp;
        auto     it  = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        Glyph g;
        // Private-use code points are icon-font glyphs (Font Awesome, Material Icons, ...) that only the page's own web
        // font has. Without it, draw nothing rather than a row of "missing glyph" boxes.
        bool pua = (cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0xF0000;
        if (pua && !stbtt_FindGlyphIndex(&f->info, static_cast<int>(cp))) return glyphs.emplace(key, g).first->second;
        float s = scale_for(f);
        int   adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f->info, static_cast<int>(cp), &adv, &lsb);
        g.adv = adv * s;
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f->info, static_cast<int>(cp), s, s, &x0, &y0, &x1, &y1);
        g.w    = x1 - x0;
        g.h    = y1 - y0;
        g.xoff = x0;
        g.yoff = y0;
        if (g.w > 0 && g.h > 0 && g.w < 4096 && g.h < 4096) {
            g.bmp.resize(static_cast<size_t>(g.w) * g.h);
            stbtt_MakeCodepointBitmap(&f->info, g.bmp.data(), g.w, g.h, g.w, s, s, static_cast<int>(cp));
        } else {
            g.w = g.h = 0;
        }
        return glyphs.emplace(key, std::move(g)).first->second;
    }

    static bool skip_cp(uint32_t cp) {
        return cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 ||
               cp == 0xFEFF || (cp >= 0x202A && cp <= 0x202E);
    }
};

Font::Font() : impl(new Impl) {}
Font::~Font() = default;

float Font::text_width(const std::string& s) {
    std::lock_guard<std::mutex> lk(impl->mu);
    bool cacheable = s.size() <= 64;
    if (cacheable) {
        auto it = impl->widths.find(s);
        if (it != impl->widths.end()) return it->second;
    }
    float                       w = 0;
    Face*                       prev_face = nullptr;
    uint32_t                    prev_cp   = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8_next(s, i);
        if (cp == 0xA0) cp = 0x20;
        if (Impl::skip_cp(cp)) continue;
        Face* f = impl->choose_face(cp);
        if (!f) {
            w += impl->size * 0.5f;
            continue;
        }
        const Glyph& g = impl->glyph(f, cp);
        if (prev_face == f) w += impl->kern(f, prev_cp, cp);
        w += g.adv;
        prev_face = f;
        prev_cp   = cp;
    }
    if (cacheable) {
        if (impl->widths.size() > 50000) impl->widths.clear();
        impl->widths.emplace(s, w);
    }
    return w;
}

void Font::draw(Framebuffer& fb, const std::string& s, int x, int baseline, Color color) {
    std::lock_guard<std::mutex> lk(impl->mu);
    float                       pen       = static_cast<float>(x);
    Face*                       prev_face = nullptr;
    uint32_t                    prev_cp   = 0;
    IRect                       clip      = fb.clip();
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8_next(s, i);
        if (cp == 0xA0) cp = 0x20;
        if (Impl::skip_cp(cp)) continue;
        Face* f = impl->choose_face(cp);
        if (!f) {
            pen += impl->size * 0.5f;
            continue;
        }
        const Glyph& g = impl->glyph(f, cp);
        if (prev_face == f) pen += impl->kern(f, prev_cp, cp);
        int gx = static_cast<int>(std::lround(pen)) + g.xoff;
        int gy = baseline + g.yoff;
        if (g.w > 0 && gx < clip.right() + 2 && gx + g.w + 2 > clip.x && gy < clip.bottom() && gy + g.h > clip.y) {
            for (int r = 0; r < g.h; r++) {
                int y = gy + r;
                if (y < clip.y || y >= clip.bottom()) continue;
                int shift = 0;
                if (impl->synth_italic) shift = static_cast<int>(std::lround((baseline - y) * 0.2f));
                const uint8_t* row = &g.bmp[static_cast<size_t>(r) * g.w];
                for (int c = 0; c < g.w; c++) {
                    if (!row[c]) continue;
                    int px = gx + c + shift;
                    fb.blend_px(px, y, color, row[c]);
                    if (impl->synth_bold) fb.blend_px(px + 1, y, color, row[c]);
                }
            }
        }
        pen += g.adv;
        prev_face = f;
        prev_cp   = cp;
    }
}

namespace {
// CSS Fonts 4 5.2, simplified: the face whose weight range is nearest to `w`, in the direction the spec prefers.
// Returns a sortable badness (lower is better).
double weight_badness(int w, int lo, int hi) {
    if (w >= lo && w <= hi) return 0;
    int v = w < lo ? lo : hi; // nearest weight the face has
    if (w > 500) return v > w ? v - w : 10000 + (w - v);
    if (w < 400) return v < w ? w - v : 10000 + (v - w);
    // 400..500: heavier up to 500 first, then lighter, then heavier than 500
    if (v > w && v <= 500) return v - w;
    if (v < w) return 1000 + (w - v);
    return 20000 + (v - w);
}

void set_metrics(Font& font, Face* f, float size_px) {
    float s   = font.impl ? stbtt_ScaleForMappingEmToPixels(&f->info, size_px) : 0;
    int   asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    font.ascent      = static_cast<int>(std::lround(asc * s));
    font.descent     = static_cast<int>(std::lround(-desc * s));
    font.line_height = font.ascent + font.descent + static_cast<int>(std::lround(gap * s));
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(&f->info, 'x', &x0, &y0, &x1, &y1)) font.x_height = static_cast<int>(std::lround(y1 * s));
    else font.x_height = static_cast<int>(size_px * 0.5f + 0.5f);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, '0', &adv, &lsb);
    font.ch_width = static_cast<int>(std::lround(adv * s));
    if (font.ch_width <= 0) font.ch_width = static_cast<int>(size_px * 0.5f + 0.5f);
    if (font.ascent + font.descent <= 0) { // an icon font with silly metrics: don't collapse the line
        font.ascent      = static_cast<int>(size_px * 0.8f + 0.5f);
        font.descent     = static_cast<int>(size_px * 0.2f + 0.5f);
        font.line_height = static_cast<int>(size_px * 1.2f + 0.5f);
    }
}
} // namespace

uint64_t FontManager::new_scope() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    uint64_t id = impl_->next_scope++;
    impl_->scopes[id];
    return id;
}

void FontManager::drop_scope(uint64_t scope) {
    if (!scope) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::string prefix = "w" + std::to_string(scope) + ":";
    for (auto it = impl_->fonts.begin(); it != impl_->fonts.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) it = impl_->fonts.erase(it);
        else ++it;
    }
    impl_->scopes.erase(scope);
}

bool FontManager::add_web_face(uint64_t scope, const WebFace& wf) {
    if (!wf.data || wf.data->empty()) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto sit = impl_->scopes.find(scope);
    if (sit == impl_->scopes.end() || sit->second.faces.size() >= 200) return false;
    auto f    = std::make_unique<Face>();
    f->id     = impl_->next_id++;
    f->shared = wf.data;
    if (!stbtt_InitFont(&f->info, wf.data->data(), 0)) return false;
    f->ok = true;
    Impl::WebEntry e;
    for (char c : wf.family) e.family += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    e.wlo      = std::min(wf.weight_lo, wf.weight_hi);
    e.whi      = std::max(wf.weight_lo, wf.weight_hi);
    e.italic   = wf.italic;
    e.variable = wf.variable;
    e.ranges   = wf.ranges;
    e.face     = std::move(f);
    sit->second.faces.push_back(std::move(e));
    // fonts already picked for this scope may now be wrong: keep them alive (styles still point at them), stop handing
    // them out, and tell everyone to ask again
    std::string prefix = "w" + std::to_string(scope) + ":";
    for (auto it = impl_->fonts.begin(); it != impl_->fonts.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            sit->second.retired.push_back(std::move(it->second));
            it = impl_->fonts.erase(it);
        } else {
            ++it;
        }
    }
    gen_++;
    return true;
}

Font* FontManager::get(const std::string& css_family, float size_px, int weight, bool italic, uint64_t scope) {
    if (size_px < 1) size_px = 1;
    if (size_px > 512) size_px = 512;
    FamilyClass fc   = classify_family(css_family);
    bool        bold = weight >= 600;
    std::string key  = std::to_string(static_cast<int>(fc)) + (bold ? "b" : "n") + (italic ? "i" : "n") + std::to_string(static_cast<int>(size_px * 64 + 0.5f));

    std::lock_guard<std::mutex> lk(impl_->mu);
    if (scope) { // the page's own fonts first
        auto sit = impl_->scopes.find(scope);
        if (sit != impl_->scopes.end() && !sit->second.faces.empty()) {
            auto& faces = sit->second.faces;
            for (const std::string& name : family_names(css_family)) {
                const Impl::WebEntry* best = nullptr;
                double                bestb = 1e18;
                bool                  any_style = false;
                for (auto& e : faces)
                    if (e.family == name && e.italic == italic) any_style = true;
                for (auto& e : faces) {
                    if (e.family != name || (any_style && e.italic != italic)) continue;
                    double b = weight_badness(weight, e.wlo, e.whi);
                    if (b < bestb) { bestb = b; best = &e; }
                }
                if (!best) continue;
                bool sb = bold && (best->whi < 600 || best->variable); // a variable font only has its default instance
                bool si = italic && !best->italic;
                std::string wkey = "w" + std::to_string(scope) + ":" + name + ":" + std::to_string(best->wlo) + "-" + std::to_string(best->whi) +
                                   (best->italic ? "i" : "n") + (sb ? "B" : "") + (si ? "I" : "") + ":" + key;
                auto it = impl_->fonts.find(wkey);
                if (it != impl_->fonts.end()) return it->second.get();
                impl_->ensure_index();
                auto font         = std::make_unique<Font>();
                font->size        = size_px;
                font->impl->size  = size_px;
                font->impl->mgr   = impl_.get();
                for (auto& e : faces) // every unicode-range subset with the same descriptors
                    if (e.family == name && e.wlo == best->wlo && e.whi == best->whi && e.italic == best->italic)
                        font->impl->web.push_back({e.face.get(), e.ranges});
                font->impl->primary      = font->impl->web.front().first;
                bool b2 = false, i2 = false;
                font->impl->system       = impl_->pick(fc, bold, italic, b2, i2);
                font->impl->synth_bold   = sb;
                font->impl->synth_italic = si;
                set_metrics(*font, font->impl->primary, size_px);
                Font* raw = font.get();
                impl_->fonts[wkey] = std::move(font);
                return raw;
            }
        }
    }
    auto                        it = impl_->fonts.find(key);
    if (it != impl_->fonts.end()) return it->second.get();

    impl_->ensure_index();
    auto font               = std::make_unique<Font>();
    font->size              = size_px;
    font->impl->size        = size_px;
    font->impl->mgr         = impl_.get();
    bool sb = false, si = false;
    font->impl->primary      = impl_->pick(fc, bold, italic, sb, si);
    font->impl->synth_bold   = sb;
    font->impl->synth_italic = si;

    if (!font->impl->primary) {
        if (!impl_->warned) {
            impl_->warned = true;
            std::fprintf(stderr, "quill: no usable fonts found (looked in system font directories). "
                                 "Set font_dir in the config file or QUILL_FONT_DIR.\n");
        }
        font->ascent      = static_cast<int>(size_px * 0.8f + 0.5f);
        font->descent     = static_cast<int>(size_px * 0.2f + 0.5f);
        font->line_height = static_cast<int>(size_px * 1.2f + 0.5f);
        font->x_height    = static_cast<int>(size_px * 0.5f + 0.5f);
        font->ch_width    = static_cast<int>(size_px * 0.5f + 0.5f);
    } else {
        Face* f = font->impl->primary;
        float s = font->impl->scale_for(f);
        int   asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
        font->ascent      = static_cast<int>(std::lround(asc * s));
        font->descent     = static_cast<int>(std::lround(-desc * s));
        font->line_height = font->ascent + font->descent + static_cast<int>(std::lround(gap * s));
        int x0, y0, x1, y1;
        if (stbtt_GetCodepointBox(&f->info, 'x', &x0, &y0, &x1, &y1)) font->x_height = static_cast<int>(std::lround(y1 * s));
        else font->x_height = static_cast<int>(size_px * 0.5f + 0.5f);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f->info, '0', &adv, &lsb);
        font->ch_width = static_cast<int>(std::lround(adv * s));
    }
    Font* raw = font.get();
    impl_->fonts[key] = std::move(font);
    return raw;
}

} // namespace quill
