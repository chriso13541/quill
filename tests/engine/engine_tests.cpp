// Engine feature runner. Each tests/engine/cases/*.html may contain assertions in comments:
//   <!-- expect: <selector> <property> = <value> -->
//   <!-- viewport: 800x600 -->           (optional; default 1024x768)
// Selectors are simple ([tag][#id][.class]); lengths compare in px. Usage:
//   engine_tests <cases-dir> [--html report.html]
#include "document.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using quill::engine::Document;

namespace {
std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
std::string trim(std::string s) { size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return ""; size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1); }
std::string esc(const std::string& s) { std::string o; for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else o += c; } return o; }

struct Result { std::string file, what, expected, actual; bool pass; };
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: engine_tests <cases-dir> [--html report.html]\n"); return 2; }
    fs::path dir = argv[1];
    std::string html_out;
    for (int i = 2; i + 1 < argc; ++i) if (std::string(argv[i]) == "--html") html_out = argv[i + 1];

    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".html") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    std::vector<Result> results;
    for (auto& f : files) {
        std::string src = slurp(f), name = f.filename().string();
        Document doc;
        int vw = 1024, vh = 768;
        size_t vp = src.find("<!-- viewport:");
        if (vp != std::string::npos) std::sscanf(src.c_str() + vp, "<!-- viewport: %dx%d", &vw, &vh);
        if (!doc.parse_html(src)) { results.push_back({name, "parse", "ok", doc.last_error(), false}); continue; }
        fs::path base = f.parent_path();
        doc.load_stylesheets([&](const std::string& href, std::string& css) {
            fs::path p = base / href; if (!fs::exists(p)) return false; css = slurp(p); return true; }, "");
        doc.resolve_styles(vw, vh);
        doc.layout(vw, vh);
        size_t pos = 0;
        while ((pos = src.find("<!-- expect:", pos)) != std::string::npos) {
            size_t end = src.find("-->", pos);
            if (end == std::string::npos) break;
            std::string body = trim(src.substr(pos + 12, end - pos - 12));
            pos = end + 3;
            size_t eq = body.find('=');
            if (eq == std::string::npos) continue;
            std::string lhs = trim(body.substr(0, eq)), want = trim(body.substr(eq + 1));
            size_t sp = lhs.find(' ');
            if (sp == std::string::npos) continue;
            std::string sel = trim(lhs.substr(0, sp)), prop = trim(lhs.substr(sp + 1)), got;
            bool ok = doc.computed_value(sel, prop, got) && got == want;
            results.push_back({name, sel + " " + prop, want, got, ok});
        }
        // layout assertions:  <!-- rect: sel = x y w h -->   <!-- line: sel idx = x y w h -->
        for (int pass2 = 0; pass2 < 2; ++pass2) {
            const char* tag = pass2 ? "<!-- line:" : "<!-- rect:";
            size_t p2 = 0;
            while ((p2 = src.find(tag, p2)) != std::string::npos) {
                size_t e2 = src.find("-->", p2);
                if (e2 == std::string::npos) break;
                std::string body = trim(src.substr(p2 + std::strlen(tag), e2 - p2 - std::strlen(tag)));
                p2 = e2 + 3;
                size_t eq = body.find('=');
                if (eq == std::string::npos) continue;
                std::string lhs = trim(body.substr(0, eq)), want = trim(body.substr(eq + 1));
                std::string sel = lhs;
                size_t idx = 0;
                if (pass2) { size_t sp = lhs.find(' '); sel = trim(lhs.substr(0, sp)); idx = static_cast<size_t>(std::atoi(lhs.substr(sp + 1).c_str())); }
                quill::engine::Rect rc;
                bool ok = pass2 ? doc.line_rect(sel, idx, rc) : doc.box_rect(sel, rc);
                auto fmt = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); std::string s(b); while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); return s; };
                std::string got = ok ? fmt(rc.x) + " " + fmt(rc.y) + " " + fmt(rc.w) + " " + fmt(rc.h) : "<no such box>";
                results.push_back({name, std::string(pass2 ? "line " : "rect ") + lhs, want, got, got == want});
            }
        }
        // golden box-tree dumps:  <!-- boxes: <selector>\n  line\n  line -->   (indentation relative)
        pos = 0;
        while ((pos = src.find("<!-- boxes:", pos)) != std::string::npos) {
            size_t end = src.find("-->", pos);
            if (end == std::string::npos) break;
            std::string body = src.substr(pos + 11, end - pos - 11);
            pos = end + 3;
            size_t nl = body.find('\n');
            std::string sel = trim(nl == std::string::npos ? body : body.substr(0, nl));
            std::vector<std::string> want;
            if (nl != std::string::npos) {
                std::stringstream ss(body.substr(nl + 1));
                std::string line;
                size_t      indent = std::string::npos;
                while (std::getline(ss, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line.empty()) continue;
                    indent = std::min(indent, line.find_first_not_of(' '));
                    want.push_back(line);
                }
                for (auto& l : want) l = l.substr(indent);
            }
            std::stringstream as(doc.dump_boxes(sel));
            std::string       line, wants, gots;
            while (std::getline(as, line)) gots += "\n      " + line;
            for (auto& l : want) wants += "\n      " + l;
            results.push_back({name, "boxes " + sel, wants, gots, wants == gots});
        }
    }

    size_t pass = 0;
    std::string cur;
    for (auto& r : results) {
        if (r.pass) ++pass;
        if (r.file != cur) { cur = r.file; std::printf("\n%s\n", cur.c_str()); }
        bool multi = r.expected.find('\n') != std::string::npos || r.actual.find('\n') != std::string::npos;
        if (r.pass && multi) std::printf("  PASS  %s\n", r.what.c_str());
        else if (r.pass) std::printf("  PASS  %s = %s\n", r.what.c_str(), r.expected.c_str());
        else if (multi) std::printf("  FAIL  %s\n    expected:%s\n    got:%s\n", r.what.c_str(), r.expected.c_str(), r.actual.c_str());
        else std::printf("  FAIL  %s: expected %s, got %s\n", r.what.c_str(), r.expected.c_str(), r.actual.c_str());
    }
    std::printf("\n%zu/%zu passed\n", pass, results.size());

    if (!html_out.empty()) {
        std::ofstream o(html_out);
        o << "<!doctype html><meta charset=utf-8><title>Quill engine tests</title><style>body{font:14px sans-serif;margin:24px}"
             "table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:3px 8px;text-align:left}.p{color:#0a0}.f{color:#c00;font-weight:bold}</style>"
          << "<h1>Quill engine tests: " << pass << "/" << results.size() << " passed</h1><table><tr><th>file<th>check<th>expected<th>actual<th>";
        for (auto& r : results)
            o << "<tr><td>" << esc(r.file) << "<td>" << esc(r.what) << "<td><pre>" << esc(r.expected) << "</pre><td><pre>" << esc(r.actual) << "</pre>"
              << (r.pass ? "<td class=p>PASS" : "<td class=f>FAIL");
        o << "</table>";
    }
    return pass == results.size() ? 0 : 1;
}
