// enginedump: parse an HTML file with the new engine and print the DOM with computed styles.
//   enginedump page.html [--viewport 1000x700] [--no-styles] [--quiet] [--boxes]
// Stylesheets referenced by the page are read from the page's directory ("/x.css" is relative to it too).
#include "document.h"
#include "paint.h"
#include "url.h"

#include <cctype>
#include <chrono>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace quill;

static bool quiet_loads = false;
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

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int main(int argc, char** argv) {
    std::string file;
    int         vw = 1000, vh = 700;
    bool        styles = true, quiet = false, boxes = false;
    std::vector<std::string> rects;
    std::vector<std::pair<float, float>> hits;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--no-styles") styles = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--boxes") boxes = true;
        else if (a == "--rect" && i + 1 < argc) rects.push_back(argv[++i]);
        else if (a == "--hit" && i + 1 < argc) { float hx = 0, hy = 0; std::sscanf(argv[++i], "%f,%f", &hx, &hy); hits.push_back({hx, hy}); }
        else if (a == "--viewport" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &vw, &vh);
        else file = a;
    }
    if (file.empty()) {
        std::fprintf(stderr, "usage: enginedump page.html [--viewport WxH] [--no-styles] [--quiet] [--boxes]\n");
        return 2;
    }
    std::string html;
    if (!read_file(file, html)) {
        std::fprintf(stderr, "cannot read %s\n", file.c_str());
        return 1;
    }
    std::string dir = ".";
    size_t      sl  = file.find_last_of("/\\");
    if (sl != std::string::npos) dir = file.substr(0, sl);

    using clk = std::chrono::steady_clock;
    auto ms   = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    engine::Document doc;
    auto t0 = clk::now();
    if (!doc.parse_html(html)) {
        std::fprintf(stderr, "parse failed: %s\n", doc.last_error().c_str());
        return 1;
    }
    auto t1 = clk::now();
    doc.load_stylesheets(
        [&](const std::string& href, std::string& css) {
            ParsedUrl u;
            if (!parse_url(href, u)) { std::fprintf(stderr, "stylesheet: bad url %s\n", href.c_str()); return false; }
            std::string path = url_decode(u.path);
            bool ok = read_file(dir + path, css);
            if (!quiet_loads) std::fprintf(stderr, "stylesheet: %s %s (%zu bytes)\n", ok ? "ok  " : "MISS", path.c_str(), css.size());
            return ok;
        },
        "http://local.test/");
    auto t2 = clk::now();
    if (!doc.resolve_styles(vw, vh)) {
        std::fprintf(stderr, "style failed: %s\n", doc.last_error().c_str());
        return 1;
    }
    auto t3 = clk::now();
    if (!doc.last_error().empty()) std::fprintf(stderr, "note: %s\n", doc.last_error().c_str());
    if (std::getenv("QUILL_DUMP_GEOM")) doc.layout(vw, vh);
    if (!quiet) std::fputs((boxes ? doc.dump_boxes() : doc.dump_tree(styles)).c_str(), stdout);
    if ((!rects.empty() || std::getenv("QUILL_DUMP_GEOM")) && doc.layout(vw, vh))
        for (auto& r : rects) {
            engine::Rect rc;
            if (doc.box_rect(r, rc)) std::printf("rect %s = %g %g %g %g\n", r.c_str(), rc.x, rc.y, rc.w, rc.h);
            else std::printf("rect %s = (none)\n", r.c_str());
        }
    if (!hits.empty() && doc.layout(vw, vh))
        for (auto& h : hits) {
            const engine::Box* b = engine::hit_test(*doc.box_root(), h.first, h.second);
            std::printf("hit %g,%g:", h.first, h.second);
            for (const engine::Box* p = b; p; p = p->parent) std::printf(" <%s%s%s>", p->tag.c_str(), p->attr("class") ? "." : "", p->attr("class") ? p->attr("class")->substr(0, 24).c_str() : "");
            std::printf("\n");
        }
    auto vs = doc.var_stats();
    std::fprintf(stderr, "vars: defined=%zu substituted=%zu dropped=%zu scoped-skipped=%zu\n", vs.defined, vs.substituted, vs.unresolved, vs.skipped_scoped);
    std::fprintf(stderr, "elements=%zu sheets=%zu parse=%.1fms stylesheets=%.1fms styles=%.1fms\n", doc.element_count(),
                 doc.stylesheet_count(), ms(t0, t1), ms(t1, t2), ms(t2, t3));
    return 0;
}
