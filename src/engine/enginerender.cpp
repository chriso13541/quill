#include <algorithm>
// enginerender: render a saved HTML page to a PNG with the new engine.
//   enginerender page.html out.png [--width 1000] [--height 700] [--full] [--fake-font] [--anim-ms T]
// --anim-ms T paints animated GIFs as they look T milliseconds after the page loaded.
// --bench-scroll paints the page the way scrolling does (one viewport per 64 px wheel step, top to bottom) and prints
// the time per frame: the number that decides whether scrolling feels smooth.
// Stylesheets and images are read from the page's directory (the "save as complete page" layout).
#include "document.h"
#include "paint.h"

#include "font.h"
#include "image.h"
#include "url.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

extern "C" int stbi_write_png_to_func(void (*func)(void* context, void* data, int size), void* context, int w, int h, int comp, const void* data,
                                      int stride_in_bytes);

using namespace quill;

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
static std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}
static void png_sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<char*>(data), static_cast<size_t>(size)); }

struct ImageStore {
    std::string dir;
    std::map<std::string, std::shared_ptr<Image>> cache;
    const Image* get(const std::string& src) {
        auto it = cache.find(src);
        if (it != cache.end()) return it->second.get();
        std::shared_ptr<Image> img;
        std::string bytes;
        if (src.rfind("data:", 0) == 0) {
            std::string mime;
            if (decode_data_uri(src, mime, bytes)) img = decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        } else if (src.rfind("http", 0) != 0) {
            std::string rel = url_decode(src);
            size_t q = rel.find_first_of("?#");
            if (q != std::string::npos) rel = rel.substr(0, q);
            if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
            if (read_file(dir + "/" + rel, bytes)) img = decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        }
        cache[src] = img;
        return img.get();
    }
};

int main(int argc, char** argv) {
    std::string file, out;
    int vw = 1000, vh = 700;
    bool full = false, fake = false;
    long long anim_ms = -1;
    bool bench = false, verify_cull = false, bench_hover = false;
    int  geom_depth = 0;
    float zoom = 1.f;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--width" && i + 1 < argc) vw = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) vh = std::atoi(argv[++i]);
        else if (a == "--full") full = true;
        else if (a == "--fake-font") fake = true;
        else if (a == "--anim-ms" && i + 1 < argc) anim_ms = std::atoll(argv[++i]);
        else if (a == "--bench-scroll") bench = true;
        else if (a == "--bench-hover") bench_hover = true;
        else if (a == "--geom" && i + 1 < argc) geom_depth = std::atoi(argv[++i]);
        else if (a == "--zoom" && i + 1 < argc) zoom = std::strtof(argv[++i], nullptr) / 100.f; // percent
        else if (a == "--verify-cull") bench = verify_cull = true; // every bench frame also painted without culling
        else if (file.empty()) file = a;
        else out = a;
    }
    if (file.empty() || out.empty()) {
        std::fprintf(stderr, "usage: enginerender page.html out.png [--width W] [--height H] [--full] [--anim-ms T]\n");
        return 2;
    }
    std::string html;
    if (!read_file(file, html)) { std::fprintf(stderr, "cannot read %s\n", file.c_str()); return 1; }
    std::string dir = ".";
    size_t sl = file.find_last_of("/\\");
    if (sl != std::string::npos) dir = file.substr(0, sl);

    engine::Document doc;
    auto parse_t0 = std::chrono::steady_clock::now();
    if (!doc.parse_html(html)) { std::fprintf(stderr, "parse failed: %s\n", doc.last_error().c_str()); return 1; }
    doc.load_stylesheets([&](const std::string& href, std::string& css) {
        ParsedUrl u;
        if (!parse_url(href, u)) return false;
        return read_file(dir + url_decode(u.path), css);
    }, "http://local.test/");

    ImageStore images{dir, {}};
    engine::FontTextMeasurer fonts;
    if (!fake) doc.set_text_measurer(&fonts);
    doc.set_image_sizer([&](const std::string& src, int& w, int& h) {
        const Image* im = images.get(src);
        if (!im) return false;
        w = im->w; h = im->h;
        return true;
    });
    doc.set_image_broken([&](const std::string& src) { return images.get(src) == nullptr; }); // missing file = broken
    using clk = std::chrono::steady_clock;
    if (std::getenv("QUILL_PROF")) std::fprintf(stderr, "parse+stylesheet load %.1fms\n", std::chrono::duration<double, std::milli>(clk::now() - parse_t0).count());
    auto t0 = clk::now();
    doc.set_zoom(zoom);
    if (!doc.resolve_styles(vw, vh)) { std::fprintf(stderr, "style failed: %s\n", doc.last_error().c_str()); return 1; }
    auto t1 = clk::now();
    doc.layout(vw, vh);
    auto t2 = clk::now();
    const engine::Box* root = doc.box_root();
    if (geom_depth > 0) { // the box tree's geometry, a few levels deep, and how far the page scrolls
        std::function<void(const engine::Box&, int)> pr = [&](const engine::Box& b, int d) {
            if (b.kind == engine::Box::Kind::Text || d > geom_depth) return;
            std::string cls;
            if (const std::string* c = b.attr("class")) cls = "." + c->substr(0, 30);
            std::printf("%*s%s%s  x=%.0f y=%.0f w=%.0f h=%.0f%s\n", d * 2, "", b.tag.empty() ? "(anon)" : b.tag.c_str(), cls.c_str(), b.x, b.y, b.w, b.h,
                        b.style && (b.style->overflow_y != engine::Overflow::Visible) ? " clips" : "");
            for (auto& c : b.children) pr(*c, d + 1);
        };
        pr(*root, 0);
        std::printf("scroll extent %.0f (viewport %d)\n", engine::scroll_extent(*root), vh);
    }
    int doc_h = static_cast<int>(std::ceil(engine::scroll_extent(*root)));
    int h = full ? std::min(std::max(doc_h, vh), 20000) : vh;

    if (bench_hover) { // hover over every 40th pixel down the left half: restyle (+ layout unless paint-only)
        std::vector<double> ms;
        size_t fast0 = doc.paint_only_restyles(), changes = 0;
        for (int y = 0; y < std::min(doc_h, 6000); y += 40) {
            const engine::Box* b = engine::hit_test(*doc.box_root(), static_cast<float>(vw / 4), static_cast<float>(y));
            const void* node = nullptr;
            for (const engine::Box* p = b; p && !node; p = p->parent) node = p->node;
            auto a = clk::now();
            if (doc.set_hover(node)) {
                changes++;
                doc.resolve_styles(vw, vh);
                if (!doc.restyle_was_paint_only()) doc.layout(vw, vh);
                ms.push_back(std::chrono::duration<double, std::milli>(clk::now() - a).count());
            }
        }
        double sum = 0, mx = 0;
        for (double m : ms) { sum += m; mx = std::max(mx, m); }
        std::printf("hover bench: %zu hover changes, %zu repainted without layout; avg %.2f ms, max %.2f ms\n", changes,
                    doc.paint_only_restyles() - fast0, ms.empty() ? 0 : sum / static_cast<double>(ms.size()), mx);
        return 0;
    }
    if (bench) {
        Framebuffer vfb;
        vfb.resize(vw, vh);
        engine::PaintEnv benv;
        benv.image        = [&](const std::string& src) { return images.get(src); };
        benv.image_broken = [&](const std::string& src) { return images.get(src) == nullptr; };
        engine::paint_document(*root, vfb, 0, 0, benv); // warm caches (glyphs, scaled images)
        std::vector<double> ms;
        for (int y = 0; y + vh <= std::max(doc_h, vh); y += 64) {
            auto a = clk::now();
            engine::paint_document_area(*root, vfb, IRect(0, 0, vw, vh), 0, static_cast<float>(y), benv);
            ms.push_back(std::chrono::duration<double, std::milli>(clk::now() - a).count());
            if (verify_cull) {
                Framebuffer ref;
                ref.resize(vw, vh);
                engine::PaintEnv renv = benv;
                renv.cull = false;
                engine::paint_document_area(*root, ref, IRect(0, 0, vw, vh), 0, static_cast<float>(y), renv);
                if (ref.px != vfb.px) {
                    size_t diff = 0;
                    for (size_t i = 0; i < ref.px.size(); i++) diff += ref.px[i] != vfb.px[i];
                    std::printf("CULL MISMATCH at scroll %d: %zu pixels differ\n", y, diff);
                    return 2;
                }
            }
        }
        if (verify_cull) std::printf("culling verified: %zu frames identical to a full paint\n", ms.size());
        double sum = 0, mx = 0;
        for (double m : ms) { sum += m; mx = std::max(mx, m); }
        std::sort(ms.begin(), ms.end());
        std::printf("scroll bench: doc %dpx, %zu frames of %dx%d: avg %.2f ms, p95 %.2f ms, max %.2f ms\n", doc_h, ms.size(), vw, vh,
                    sum / static_cast<double>(ms.size()), ms[ms.size() * 95 / 100], mx);
        return 0;
    }
    Framebuffer fb;
    fb.resize(vw, h);
    fb.push_clip(IRect(0, 0, vw, h));
    engine::PaintEnv env;
    env.zoom  = doc.zoom();
    env.image = [&](const std::string& src) { return images.get(src); };
    env.image_broken = [&](const std::string& src) { return images.get(src) == nullptr; };
    int anim_next = -1;
    env.anim_ms      = anim_ms;
    env.anim_next_ms = &anim_next;
    engine::paint_document(*root, fb, 0, 0, env);
    if (anim_ms >= 0) std::fprintf(stderr, "next animation frame in %d ms\n", anim_next);
    auto t3 = clk::now();

    std::vector<unsigned char> rgb(static_cast<size_t>(vw) * h * 3);
    for (size_t i = 0; i < fb.px.size(); ++i) {
        uint32_t p = fb.px[i];
        rgb[i * 3] = (p >> 16) & 0xff; rgb[i * 3 + 1] = (p >> 8) & 0xff; rgb[i * 3 + 2] = p & 0xff;
    }
    std::string png;
    if (!stbi_write_png_to_func(png_sink, &png, vw, h, 3, rgb.data(), vw * 3)) { std::fprintf(stderr, "png encode failed\n"); return 1; }
    std::ofstream(out, std::ios::binary).write(png.data(), static_cast<long>(png.size()));
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::fprintf(stderr, "document height=%d  style+boxes=%.0fms layout=%.0fms paint=%.0fms -> %s (%dx%d)\n", doc_h, ms(t0, t1), ms(t1, t2), ms(t2, t3), out.c_str(), vw, h);
    return 0;
}
