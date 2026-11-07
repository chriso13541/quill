// Browser-level tests: navigation, the http warning rule, :visited, framesets. Uses fake_net.cpp instead of libcurl,
// so it needs no network; pages are checked by drawing them and looking at pixels (solid page colours).
#include "browser.h"
#include "fake_net.h"
#include "hsts.h"
#include "framebuffer.h"
#include "url.h"
#include "fontsan.h"
#include "history.h"
#include "image.h"

#include <fstream>
#include <sstream>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(x)) {                                                              \
            g_failures++;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);             \
        }                                                                        \
    } while (0)

static const int W = 800, H = 600;

static void settle(Browser& b) {
    for (int i = 0; i < 400; i++) {
        b.poll();
        if (b.idle() && i > 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    b.poll();
}

static Framebuffer shot(Browser& b) {
    Framebuffer fb;
    fb.resize(W, H);
    fb.clear(Color(0, 0, 0));
    b.draw(fb, IRect(0, 0, W, H));
    return fb;
}

static uint32_t rgb(const Framebuffer& fb, int x, int y) { return fb.px[static_cast<size_t>(y) * fb.w + x] & 0xffffff; }

// Some pixel in the rectangle is within `tol` of the colour (text is anti-aliased, so look for its core).
static bool has_color(const Framebuffer& fb, int x0, int y0, int x1, int y1, uint32_t c, int tol = 24) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = rgb(fb, x, y);
            if (std::abs(static_cast<int>((p >> 16) & 255) - static_cast<int>((c >> 16) & 255)) <= tol &&
                std::abs(static_cast<int>((p >> 8) & 255) - static_cast<int>((c >> 8) & 255)) <= tol &&
                std::abs(static_cast<int>(p & 255) - static_cast<int>(c & 255)) <= tol)
                return true;
        }
    return false;
}

static void click(Browser& b, int x, int y) {
    b.mouse_move(x, y);
    b.mouse_down(x, y);
    b.mouse_up(x, y);
    settle(b);
}

static bool was_fetched(const std::string& url) {
    for (auto& u : fake::fetched())
        if (u == url) return true;
    return false;
}
static int fetch_count(const std::string& url) {
    int n = 0;
    for (auto& u : fake::fetched())
        if (u == url) n++;
    return n;
}

static std::string page(const std::string& bg, const std::string& body, const std::string& body_attrs = "") {
    return "<html><head><title>t</title></head><body bgcolor=\"" + bg + "\" " + body_attrs + ">" + body + "</body></html>";
}
static std::string block_link(const std::string& href, const std::string& text, const std::string& extra = "") {
    return "<a href=\"" + href + "\" " + extra + " style=\"display:block;height:60px;font-size:40px\">" + text + "</a>";
}

#ifndef QUILL_SOURCE_DIR
#define QUILL_SOURCE_DIR "."
#endif
static std::string read_file(const std::string& rel) {
    std::ifstream f(std::string(QUILL_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
extern "C" int stbi_write_png_to_func(void (*f)(void*, void*, int), void* c, int w, int h, int comp, const void* d, int stride);
static void png_sink(void* c, void* d, int n) { static_cast<std::string*>(c)->append(static_cast<char*>(d), static_cast<size_t>(n)); }
static std::string solid_png(int w, int h, uint32_t rgb) {
    std::string px;
    for (int i = 0; i < w * h; i++) { px += static_cast<char>(rgb >> 16); px += static_cast<char>(rgb >> 8); px += static_cast<char>(rgb); px += '\xff'; }
    std::string out;
    stbi_write_png_to_func(png_sink, &out, w, h, 4, px.data(), w * 4);
    return out;
}
// A 2x2 24-bit .ico (Windows DIB with an AND mask): blue, with the top-left pixel transparent.
static std::string tiny_ico() {
    std::string dib;
    auto le16 = [&](uint32_t v) { dib += static_cast<char>(v & 255); dib += static_cast<char>((v >> 8) & 255); };
    auto le32 = [&](uint32_t v) { le16(v & 0xffff); le16(v >> 16); };
    le32(40); le32(2); le32(4); le16(1); le16(24); le32(0); le32(0); le32(0); le32(0); le32(0); le32(0);
    for (int row = 0; row < 2; row++) { for (int x = 0; x < 2; x++) { dib += '\xff'; dib += '\0'; dib += '\0'; } dib += std::string(2, '\0'); } // BGR + pad
    // AND mask, bottom-up rows of 4 bytes: bottom row opaque, top row: first pixel transparent
    dib += std::string(4, '\0');
    dib += '\x80'; dib += std::string(3, '\0');
    std::string ico;
    auto i16 = [&](uint32_t v) { ico += static_cast<char>(v & 255); ico += static_cast<char>((v >> 8) & 255); };
    auto i32 = [&](uint32_t v) { i16(v & 0xffff); i16(v >> 16); };
    i16(0); i16(1); i16(1);
    ico += '\x02'; ico += '\x02'; ico += '\0'; ico += '\0'; i16(1); i16(24); i32(static_cast<uint32_t>(dib.size())); i32(22);
    return ico + dib;
}

static void build_site() {
    auto& s = fake::site();
    // the plain-http old web
    s["http://old.example/"].body = page("#ff0000", block_link("http://other.example/b.html", "NEXT"), "link=\"#0000ff\" vlink=\"#ffff00\"");
    s["http://other.example/b.html"].body = page("#00ff00", block_link("http://old.example/", "BACK"));
    s["http://redirector.example/"].redirect = "http://landing.example/";
    s["http://landing.example/"].body = page("#00ffff", "landed");
    s["http://old2.example/"].body = page("#ff0000", block_link("http://redirector.example/", "GO"));
    // https pages linking down to http
    s["https://secure.example/"].body = page("#ffffff", block_link("http://plain.example/", "DOWN"));
    s["http://plain.example/"].body   = page("#000000", "plain");
    // a frameset (https so the http rule stays out of the way)
    s["https://frames.example/"].body =
        "<html><head><title>Frames</title></head><frameset cols=\"200,*\">"
        "<frame src=\"menu.html\" name=\"menu\"><frame src=\"main.html\" name=\"main\"></frameset></html>";
    s["https://frames.example/menu.html"].body =
        page("#ff0000", block_link("c.html", "C", "target=\"main\"") + block_link("top.html", "TOP", "target=\"_top\""));
    s["https://frames.example/main.html"].body = page("#0000ff", "main");
    s["https://frames.example/c.html"].body    = page("#00ff00", "c");
    s["https://frames.example/top.html"].body  = page("#ffff00", "top");
    // <base target> and rows
    s["https://rows.example/"].body =
        "<html><frameset rows=\"100,*\" border=\"0\"><frame src=\"nav.html\"><frame src=\"body.html\" name=\"content\"></frameset></html>";
    s["https://rows.example/nav.html"].body =
        "<html><head><base target=\"content\"></head><body bgcolor=\"#ff00ff\">" + block_link("next.html", "N") + "</body></html>";
    s["https://rows.example/body.html"].body = page("#00ffff", "body");
    s["https://rows.example/next.html"].body = page("#808000", "next");
    // meta refresh: an instant redirect page (DuckDuckGo's /l/ style, quoted URL inside <noscript>) and a slow one
    s["https://start.example/"].body = page("#ffffff", block_link("https://jump.example/go", "JUMP"));
    s["https://jump.example/go"].body =
        "<html><head><meta name=\"referrer\" content=\"origin\"></head><body><noscript>"
        "<META http-equiv=\"refresh\" content=\"0;URL='https://dest.example/page'\"></noscript></body></html>";
    s["https://dest.example/page"].body = page("#00ff00", "destination");
    s["https://slow.example/"].body = "<html><head><meta http-equiv=\"refresh\" content=\"1; url=/after\"></head>"
                                       "<body bgcolor=\"#ff0000\">wait</body></html>";
    s["https://slow.example/after"].body = page("#0000ff", "after");
    s["https://loop.example/"].body = "<html><head><meta http-equiv=\"refresh\" content=\"0\"></head><body>x</body></html>";
    // a DuckDuckGo-style results page: the click goes straight to the result, not through duckduckgo.com/l/
    s["https://html.duckduckgo.com/html/?q=google"].body =
        page("#ffffff", block_link("//duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.google.com%2F&amp;rut=7bcd28c0", "Google"));
    s["https://www.google.com/"].body = page("#4285f4", "google");
    // a long page with a fixed header, a form, floats and backgrounds: for the rendered-page cache
    {
        std::string body = "<div style=\"position:fixed;top:0;left:0;right:0;height:40px;background:#334;color:#fff\">fixed header</div>"
                           "<form style=\"margin-top:60px\"><input type=checkbox id=cb> check <input name=t value=\"abc\"></form>";
        for (int i = 0; i < 120; i++)
            body += "<p style=\"background:" + std::string(i % 2 ? "#eef" : "#fee") + "\"><img style=\"float:left;width:30px;height:30px;background:#9c9\">"
                    "Paragraph " + std::to_string(i) + " with some text that wraps across the line a few times so lines are "
                    "cut by the strip edges at different places, <b>bold</b> and <a href=\"#\">links</a>.</p>";
        s["https://long.example/"].body = "<html><body style=\"margin:8px\">" + body + "</body></html>";
    }
    // three slow stylesheets: downloaded in parallel, applied in document order (the last one wins)
    s["https://css.example/"].body =
        "<html><head><link rel=stylesheet href=a.css><link rel=stylesheet href=b.css><link rel=stylesheet href=c.css></head>"
        "<body><p>text</p></body></html>";
    s["https://css.example/a.css"] = {"body{background:#ff0000}", "text/css", "", 300, "", "", "", ""};
    s["https://css.example/b.css"] = {"body{background:#00ff00}", "text/css", "", 300, "", "", "", ""};
    s["https://css.example/c.css"] = {"body{background:#0000ff}", "text/css", "", 300, "", "", "", ""};
    // a slow fixed-size thumbnail (painted on arrival without a relayout) and one without a size (needs layout)
    {
        std::string mime, gif;
        decode_data_uri("data:image/gif;base64,R0lGODdhBAAEAIEAAP8AAAAAAAAAAAAAACwAAAAABAAEAAAICQABCBxIsCCAgAA7", mime, gif);
        s["https://img.example/red.gif"]  = {gif, "image/gif", "", 200, "", "", "", ""};
        s["https://img.example/red2.gif"] = {gif, "image/gif", "", 200, "", "", "", ""};
        s["https://img.example/"].body =
            "<html><body style=\"margin:0\"><img src=red.gif width=50 height=50><br><img src=red2.gif style=\"width:100px\"></body></html>";
    }
    // HSTS: a site says "https only" once; afterwards an http link to it (from an http page) never goes out as http
    s["https://strict.example/"] = {page("#00ff00", "secure"), "text/html", "", 0, "max-age=31536000", "", "", ""};
    s["http://strict.example/"].body = page("#ff0000", "downgraded!");
    s["http://oldsite2.example/"].body = page("#ffffff", block_link("http://strict.example/", "GO"));
    // <iframe>s: a frame whose links stay inside it, one that targets the whole tab, a site that refuses framing,
    // a private address from a public page, mixed content, srcdoc, and one that scrolls on its own
    {
        auto fr = [](const std::string& src, int x, int y, const std::string& extra = "") {
            return "<iframe " + src + " frameborder=0 " + extra + " style=\"position:absolute;left:" + std::to_string(x) + "px;top:" +
                   std::to_string(y) + "px;width:200px;height:150px\"></iframe>";
        };
        s["https://ifr.example/"].body =
            "<html><body style=\"margin:0;background:#ffffff\">" + fr("src=\"inner.html\" name=\"box\"", 0, 0) +
            fr("src=\"https://nofr.example/\"", 220, 0) + fr("src=\"http://192.168.1.1/\"", 440, 0) +
            fr("src=\"http://mixed.example/\"", 0, 200) + fr("srcdoc=\"<body bgcolor=#ff00ff>inline</body>\"", 220, 200) +
            fr("src=\"long.html\"", 440, 200) +
            "<a href=\"targeted.html\" target=\"box\" style=\"position:absolute;left:0;top:400px;display:block;width:200px;height:50px\">T</a>"
            "<div style=\"height:3000px\"></div></body></html>";
        s["https://ifr.example/inner.html"].body =
            page("#00ff00", block_link("next.html", "NEXT") + block_link("top.html", "TOP", "target=\"_top\""), "style=\"margin:0\"");
        s["https://ifr.example/next.html"].body     = page("#0000ff", "next");
        s["https://ifr.example/top.html"].body      = page("#ffff00", "top");
        s["https://ifr.example/targeted.html"].body = page("#00ffff", "targeted");
        s["https://ifr.example/long.html"].body =
            "<html><body style=\"margin:0\"><div style=\"height:100px;background:#ff0000\"></div><div style=\"height:900px;background:#008000\">"
            "</div></body></html>";
        s["https://nofr.example/"]       = {page("#ff0000", "framed!"), "text/html", "", 0, "", "DENY", "", ""};
        s["https://nofr2.example/"]      = {page("#ff0000", "framed!"), "text/html", "", 0, "", "", "default-src 'self'; frame-ancestors 'self'", ""};
        s["https://okfr.example/"]       = {page("#00ff00", "ok"), "text/html", "", 0, "", "", "frame-ancestors https://ifr2.example", ""};
        s["http://192.168.1.1/"].body    = page("#ff0000", "router!");
        s["http://mixed.example/"].body  = page("#ff0000", "mixed!");
        s["https://ifr2.example/"].body =
            "<html><body style=\"margin:0\">" + fr("src=\"https://nofr2.example/\"", 0, 0) + fr("src=\"https://okfr.example/\"", 220, 0) +
            "</body></html>";
        // new windows
        s["https://win.example/"].body = page("#ffffff", block_link("blank.html", "B", "target=\"_blank\"") +
                                                              block_link("named.html", "N", "target=\"somewhere\"") +
                                                              "<form action=\"post.html\" method=post target=_blank><input type=hidden name=q value=1>"
                                                              "<input type=submit id=go style=\"width:100px;height:40px\"></form>",
                                              "style=\"margin:0\"");
        s["https://win.example/blank.html"].body = page("#00ff00", "blank");
        s["https://map.example/"].body =
            "<html><body style=\"margin:0\"><img src=\"https://img.example/red.gif\" width=200 height=100 usemap=\"#m\" style=\"display:block\">"
            "<map name=\"m\"><area shape=rect coords=\"0,0,100,100\" href=\"left.html\">"
            "<area shape=circle coords=\"150,50,30\" href=\"right.html\" target=\"_blank\"><area shape=default nohref></map>"
            "<a href=\"cgi/map\"><img src=\"https://img.example/red.gif\" ismap width=100 height=50 style=\"display:block\"></a>"
            "<img src=\"https://img.example/red.gif\" width=200 height=100 usemap=\"#p\" style=\"display:block\">"
            "<map id=\"p\"><area shape=poly coords=\"0,0,200,0,0,100\" href=\"tri.html\"></map></body></html>";
        s["https://map.example/left.html"].body  = page("#00ff00", "left");
        s["https://map.example/right.html"].body = page("#0000ff", "right");
        s["https://map.example/tri.html"].body   = page("#ffff00", "tri");
        s["https://details.example/"].body =
            "<html><body style=\"margin:0\"><details><summary style=\"height:40px;font-size:30px\">More</summary>"
            "<div style=\"height:100px;background:#00ff00\"></div></details>"
            "<details open><summary style=\"height:40px\">Open</summary><div style=\"height:100px;background:#0000ff\"></div></details>"
            "</body></html>";
    }
    // @font-face: icon fonts (private-use code points nothing else draws) in each format, a broken one, a slow one, one
    // whose unicode-range the text doesn't use, and one the page never uses
    {
        std::string ttf = read_file("tests/fonts/quill-test-icons.ttf"), woff = read_file("tests/fonts/quill-test-icons.woff"),
                    woff2 = read_file("tests/fonts/quill-test-icons.woff2");
        s["https://fonts.example/icons.ttf"]   = {ttf, "font/ttf", "", 0, "", "", "", ""};
        s["https://fonts.example/icons.woff"]  = {woff, "font/woff", "", 0, "", "", "", ""};
        s["https://fonts.example/icons.woff2"] = {woff2, "font/woff2", "", 0, "", "", "", ""};
        s["https://fonts.example/slow.ttf"]    = {ttf, "font/ttf", "", 2500, "", "", "", ""};
        s["https://fonts.example/bad.ttf"]     = {std::string(2000, '\x01'), "font/ttf", "", 0, "", "", "", ""};
        s["https://fonts.example/range.ttf"]   = {ttf, "font/ttf", "", 0, "", "", "", ""};
        s["https://fonts.example/unused.ttf"]  = {ttf, "font/ttf", "", 0, "", "", "", ""};
        auto row = [](const char* fam) { return std::string("<div style=\"font-family:") + fam + "\">&#xE900;</div>"; };
        s["https://fonts.example/"].body =
            "<html><head><style>"
            "@font-face{font-family:TestIcons;src:url(icons.woff2) format('woff2'),url(icons.woff) format('woff')}"
            "@font-face{font-family:'Ttf Icons';src:url(missing.woff2) format('woff2'),url(icons.ttf)}"
            "@font-face{font-family:BadIcons;src:url(bad.ttf)}"
            "@font-face{font-family:SlowIcons;src:url(slow.ttf)}"
            "@font-face{font-family:Arial;src:url(icons.ttf)}"
            "@font-face{font-family:RangeIcons;src:url(range.ttf);unicode-range:U+0400-04FF}"
            "@font-face{font-family:Unused;src:url(unused.ttf)}"
            "body{margin:0;background:#fff;color:#000}div{font-size:60px;height:80px;line-height:80px}"
            "</style></head><body>" +
            row("TestIcons") + row("'Ttf Icons'") + row("BadIcons") + row("SlowIcons") + row("Arial") + row("RangeIcons") + "</body></html>";
        s["https://fonts.example/swap.ttf"] = {ttf, "font/ttf", "", 700, "", "", "", ""};
        s["https://fonts.example/opt.ttf"]  = {ttf, "font/ttf", "", 400, "", "", "", ""};
        s["https://fonts.example/blk.ttf"]  = {ttf, "font/ttf", "", 400, "", "", "", ""};
        s["https://fonts.example/display"].body =
            "<html><head><style>"
            "@font-face{font-family:Swap;src:url(swap.ttf);font-display:swap}"
            "@font-face{font-family:Opt;src:url(opt.ttf);font-display:optional}"
            "@font-face{font-family:Blk;src:url(blk.ttf);font-display:block}"
            "body{margin:0;background:#fff;color:#000}div{font-size:60px;height:80px;line-height:80px}"
            "</style></head><body>" + row("Swap") + row("Opt") + row("Blk") + "</body></html>";
        s["https://other-tab.example/"].body =
            "<html><body style=\"margin:0;background:#fff\"><div style=\"font-family:Arial;font-size:60px\">&#xE900;</div></body></html>";
    }
    // site icons
    s["https://icon.example/"].body = "<html><head><link rel=\"shortcut icon\" href=\"/i.png\"></head><body>x</body></html>";
    s["https://icon.example/i.png"] = {solid_png(16, 16, 0xff0000), "image/png", "", 0, "", "", "", ""};
    s["https://ico.example/"].body  = page("#ffffff", "no link: /favicon.ico");
    s["https://ico.example/favicon.ico"] = {tiny_ico(), "image/x-icon", "", 0, "", "", "", ""};
    s["https://noicon.example/"].body    = page("#ffffff", "none");
    // <base href> (resolved before styles exist)
    s["https://basehref.example/dir/page.html"].body =
        "<html><head><base href=\"https://basehref.example/other/\"></head><body>" + block_link("x.html", "X") + "</body></html>";
}

static void test_http_rule() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    fake::clear_log();

    // typed http address: warning page, nothing fetched
    b.navigate("http://old.example/");
    settle(b);
    CHECK(!was_fetched("http://old.example/"));
    CHECK(rgb(shot(b), 400, 500) != 0xff0000);
    // "Continue over HTTP" on the warning page
    b.navigate_link("quill:allow-http?u=http%3A%2F%2Fold.example%2F");
    settle(b);
    CHECK(was_fetched("http://old.example/"));
    Framebuffer a = shot(b);
    CHECK(rgb(a, 400, 500) == 0xff0000);
    CHECK(has_color(a, 8, 8, 300, 68, 0x0000ff));  // body link= colour, not visited yet
    CHECK(!has_color(a, 8, 8, 300, 68, 0xffff00));
    CHECK(b.security().level == Browser::Security::Level::Insecure && b.security().host == "old.example");

    // a link from this plain-http page to another http host opens without a warning
    click(b, 30, 30);
    CHECK(was_fetched("http://other.example/b.html"));
    Framebuffer bb = shot(b);
    CHECK(rgb(bb, 400, 500) == 0x00ff00);
    // the link back to old.example is :visited (default purple)
    CHECK(has_color(bb, 8, 8, 300, 68, 0x551a8b));
    CHECK(!has_color(bb, 8, 8, 300, 68, 0x0000ee, 10));

    // back: old.example again, its link now in vlink= yellow; reload/back never ask again
    b.back();
    settle(b);
    Framebuffer c = shot(b);
    CHECK(rgb(c, 400, 500) == 0xff0000);
    CHECK(has_color(c, 8, 8, 300, 68, 0xffff00));
    CHECK(!has_color(c, 8, 8, 300, 68, 0x0000ff, 10));

    // an http -> http redirect followed from an http page doesn't ask either
    b.navigate_link("http://old2.example/", "http://old.example/"); // opened from the old.example page (e.g. a new tab)
    settle(b);
    CHECK(was_fetched("http://old2.example/"));
    click(b, 30, 30);
    CHECK(was_fetched("http://landing.example/"));
    CHECK(rgb(shot(b), 400, 500) == 0x00ffff);
}

static void test_https_downgrade_and_modes() {
    Settings st;
    st.doh_url = "";
    {
        Browser b(st, [] {});
        b.set_viewport(W, H);
        fake::clear_log();
        b.navigate("https://secure.example/");
        settle(b);
        CHECK(was_fetched("https://secure.example/"));
        click(b, 30, 30); // https page -> http link: still asks (a downgrade)
        CHECK(!was_fetched("http://plain.example/"));
        CHECK(b.url_for_bar() == "http://plain.example/");
        CHECK(rgb(shot(b), 400, 500) != 0x000000);
    }
    {
        st.http_warning = "always";
        Browser b(st, [] {});
        b.set_viewport(W, H);
        fake::clear_log();
        b.navigate_link("quill:allow-http?u=x"); // ignored: not on an internal page
        b.navigate("http://old.example/");
        settle(b);
        b.navigate_link("quill:allow-http?u=http%3A%2F%2Fold.example%2F");
        settle(b);
        CHECK(was_fetched("http://old.example/"));
        click(b, 30, 30); // "always": even a link from an http page asks
        CHECK(!was_fetched("http://other.example/b.html"));
    }
    {
        st.http_warning = "never";
        Browser b(st, [] {});
        b.set_viewport(W, H);
        fake::clear_log();
        b.navigate("http://old.example/");
        settle(b);
        CHECK(was_fetched("http://old.example/"));
    }
}

static void test_frameset() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    fake::clear_log();
    b.navigate("https://frames.example/");
    settle(b);
    Framebuffer a = shot(b);
    CHECK(rgb(a, 100, 300) == 0xff0000); // menu
    CHECK(rgb(a, 500, 300) == 0x0000ff); // main
    CHECK(rgb(a, 202, 300) != 0xff0000 && rgb(a, 202, 300) != 0x0000ff); // the border between them
    CHECK(b.title() == "Frames");
    CHECK(b.url_for_bar() == "https://frames.example/");

    // a link with target=main loads into the other frame; the menu stays
    click(b, 30, 30);
    Framebuffer c = shot(b);
    CHECK(rgb(c, 100, 300) == 0xff0000);
    CHECK(rgb(c, 500, 300) == 0x00ff00);
    CHECK(fetch_count("https://frames.example/menu.html") == 1);
    CHECK(b.can_back());

    // back / forward step through the frame's pages without reloading the frameset
    b.back();
    settle(b);
    CHECK(rgb(shot(b), 500, 300) == 0x0000ff);
    CHECK(fetch_count("https://frames.example/") == 1);
    CHECK(fetch_count("https://frames.example/menu.html") == 1);
    b.forward();
    settle(b);
    CHECK(rgb(shot(b), 500, 300) == 0x00ff00);

    // hover / status / scrolling are routed to the frame under the mouse
    b.mouse_move(30, 100);
    CHECK(b.hover_link() == "https://frames.example/top.html");
    CHECK(b.link_source() == "https://frames.example/menu.html");

    // target=_top replaces the whole frameset
    click(b, 30, 100);
    Framebuffer t = shot(b);
    CHECK(rgb(t, 100, 300) == 0xffff00 && rgb(t, 500, 300) == 0xffff00);
    CHECK(b.url_for_bar() == "https://frames.example/top.html");
    // back to the frameset: the main frame shows what it showed when we left
    b.back();
    settle(b);
    Framebuffer r = shot(b);
    CHECK(rgb(r, 100, 300) == 0xff0000);
    CHECK(rgb(r, 500, 300) == 0x00ff00);
}

static void test_rows_and_base_target() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://rows.example/");
    settle(b);
    Framebuffer a = shot(b);
    CHECK(rgb(a, 400, 50) == 0xff00ff);   // nav row, 100px tall
    CHECK(rgb(a, 400, 101) == 0x00ffff);  // border="0": the body frame starts right below
    click(b, 30, 30);                     // <base target="content"> sends the link to the lower frame
    Framebuffer c = shot(b);
    CHECK(rgb(c, 400, 50) == 0xff00ff);
    CHECK(rgb(c, 400, 300) == 0x808000);
}

static void test_base_href() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://basehref.example/dir/page.html");
    settle(b);
    b.mouse_move(30, 30);
    CHECK(b.hover_link() == "https://basehref.example/other/x.html");
}

static void test_meta_refresh_and_redirect_links() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    fake::clear_log();
    b.navigate("https://start.example/");
    settle(b);
    click(b, 30, 30);
    for (int i = 0; i < 20 && b.url_for_bar() != "https://dest.example/page"; i++) settle(b);
    CHECK(b.url_for_bar() == "https://dest.example/page");
    CHECK(rgb(shot(b), 400, 500) == 0x00ff00);
    b.back(); // the instant redirect page replaced itself: back goes to the page with the link
    settle(b);
    CHECK(b.url_for_bar() == "https://start.example/");

    b.navigate("https://slow.example/");
    settle(b);
    CHECK(rgb(shot(b), 400, 500) == 0xff0000); // still waiting
    CHECK(b.next_wake_ms() > 0 && b.next_wake_ms() <= 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    settle(b);
    CHECK(b.url_for_bar() == "https://slow.example/after");
    CHECK(rgb(shot(b), 400, 500) == 0x0000ff);
    b.back(); // a timed refresh is a normal navigation: back returns to the waiting page
    settle(b);
    CHECK(b.url_for_bar() == "https://slow.example/");

    fake::clear_log();
    b.navigate("https://loop.example/"); // content="0" with no URL: not an endless reload
    settle(b);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    settle(b);
    CHECK(fetch_count("https://loop.example/") == 1);

    fake::clear_log();
    b.navigate("https://html.duckduckgo.com/html/?q=google");
    settle(b);
    b.mouse_move(30, 30);
    CHECK(b.hover_link() == "https://www.google.com/");
    click(b, 30, 30);
    CHECK(b.url_for_bar() == "https://www.google.com/");
    CHECK(!was_fetched("https://duckduckgo.com/l/"));
    CHECK(rgb(shot(b), 400, 500) == 0x4285f4);
}

static void test_parallel_stylesheets() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    auto t0 = std::chrono::steady_clock::now();
    b.navigate("https://css.example/");
    settle(b);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(rgb(shot(b), 400, 500) == 0x0000ff); // cascade order kept
    CHECK(ms < 750);                           // 3 x 300 ms one after another would be 900+
    if (ms >= 750) std::printf("  stylesheets took %.0f ms\n", ms);
}

static void test_late_images() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://img.example/");
    settle(b);
    for (int i = 0; i < 40 && !b.idle(); i++) settle(b);
    settle(b);
    Framebuffer f = shot(b);
    CHECK(rgb(f, 25, 25) == 0xff0000); // fixed-size image: painted once it arrived
    CHECK(has_color(f, 10, 52, 90, 150, 0xff0000, 0)); // sized by its picture (4x4 scaled to 100 wide = 100 tall)
    CHECK(has_color(f, 10, 140, 90, 150, 0xff0000, 0));
}

static void test_hsts() {
    Settings st;
    st.doh_url = "";
    st.http_warning = "never";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    Hsts::instance().clear_for_tests();
    b.navigate("https://strict.example/");
    settle(b);
    fake::clear_log();
    b.navigate("http://oldsite2.example/");
    settle(b);
    click(b, 30, 30);
    CHECK(b.url_for_bar() == "https://strict.example/");
    CHECK(!was_fetched("http://strict.example/"));
    CHECK(rgb(shot(b), 400, 500) == 0x00ff00);
    Hsts::instance().clear_for_tests();
}

static void test_iframes() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    fake::clear_log();
    b.navigate("https://ifr.example/");
    settle(b);
    Framebuffer f = shot(b);
    CHECK(rgb(f, 150, 120) == 0x00ff00);                      // inner.html inside the first frame
    CHECK(rgb(f, 210, 120) == 0xffffff);                      // the page around it
    CHECK(was_fetched("https://nofr.example/"));
    CHECK(rgb(f, 400, 140) != 0xff0000);                      // X-Frame-Options: DENY -> not shown
    CHECK(!was_fetched("http://192.168.1.1/"));               // public page -> private address: blocked
    CHECK(rgb(f, 600, 140) != 0xff0000);
    CHECK(!was_fetched("http://mixed.example/"));             // https page -> http frame: blocked
    CHECK(rgb(f, 150, 340) != 0xff0000);
    CHECK(rgb(f, 400, 340) == 0xff00ff);                      // srcdoc
    CHECK(rgb(f, 600, 250) == 0xff0000);                      // long.html, top
    // the wheel over a frame scrolls the frame, not the page
    b.mouse_move(600, 250);
    b.scroll_by(150);
    settle(b);
    CHECK(b.scroll_y() == 0);
    CHECK(rgb(shot(b), 600, 250) == 0x008000);
    // the wheel elsewhere scrolls the page
    b.mouse_move(700, 500);
    b.scroll_by(40);
    CHECK(b.scroll_y() == 40);
    b.scroll_to(0);
    // hovering a link inside the frame shows its URL; clicking it navigates the frame only
    b.mouse_move(50, 30);
    CHECK(b.hover_link() == "https://ifr.example/next.html");
    click(b, 50, 30);
    f = shot(b);
    CHECK(rgb(f, 150, 120) == 0x0000ff);
    CHECK(b.url_for_bar() == "https://ifr.example/");
    // a link on the page targeting the frame by name
    b.navigate("https://ifr.example/");
    settle(b);
    click(b, 50, 420);
    f = shot(b);
    CHECK(rgb(f, 150, 120) == 0x00ffff);
    CHECK(b.url_for_bar() == "https://ifr.example/");
    // target=_top from inside the frame: the whole tab goes
    b.navigate("https://ifr.example/");
    settle(b);
    click(b, 50, 90);
    CHECK(b.url_for_bar() == "https://ifr.example/top.html");
    CHECK(rgb(shot(b), 700, 500) == 0xffff00);
    // CSP frame-ancestors
    b.navigate("https://ifr2.example/");
    settle(b);
    f = shot(b);
    CHECK(rgb(f, 100, 100) != 0xff0000);
    CHECK(rgb(f, 320, 100) == 0x00ff00);
}

static void test_new_windows() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    std::vector<Browser::NewTab> asked;
    b.set_open_tab([&](const Browser::NewTab& r) { asked.push_back(r); });
    b.navigate("https://win.example/");
    settle(b);
    click(b, 30, 30); // target=_blank
    CHECK(asked.size() == 1 && asked.back().url == "https://win.example/blank.html");
    CHECK(asked.size() == 1 && asked.back().source_page == "https://win.example/");
    CHECK(b.url_for_bar() == "https://win.example/");
    click(b, 30, 90); // a window name nothing has
    CHECK(asked.size() == 2 && asked.back().url == "https://win.example/named.html");
    click(b, 50, 150); // a POST form with target=_blank
    CHECK(asked.size() == 3 && asked.back().method == "POST" && asked.back().body == "q=1" && asked.back().form_submit);
    CHECK(b.url_for_bar() == "https://win.example/");
    // the new tab's side
    Browser t(st, [] {});
    t.set_viewport(W, H);
    t.open_request(asked[0]);
    settle(t);
    CHECK(t.url_for_bar() == "https://win.example/blank.html");
    CHECK(rgb(shot(t), 400, 500) == 0x00ff00);
    // without a handler (headless) the tab itself navigates, as before
    Browser h(st, [] {});
    h.set_viewport(W, H);
    h.navigate("https://win.example/");
    settle(h);
    click(h, 30, 30);
    CHECK(h.url_for_bar() == "https://win.example/blank.html");
}

static void test_details() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://details.example/");
    settle(b);
    Framebuffer f = shot(b);
    CHECK(!has_color(f, 0, 0, W, H, 0x00ff00, 0));          // closed: its content is hidden
    CHECK(rgb(f, 100, 40 + 40 + 50) == 0x0000ff);              // the open one shows its content
    CHECK(has_color(f, 0, 0, 40, 40, 0x000000, 80));           // a disclosure triangle before "More"
    click(b, 100, 20);                                         // open the first
    f = shot(b);
    CHECK(rgb(f, 100, 90) == 0x00ff00);
    CHECK(rgb(f, 100, 140 + 40 + 50) == 0x0000ff);             // the second moved down
    click(b, 100, 20);                                         // and close it again
    CHECK(!has_color(shot(b), 0, 0, W, H, 0x00ff00, 0));
}

static void test_image_maps() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    std::vector<Browser::NewTab> asked;
    b.set_open_tab([&](const Browser::NewTab& r) { asked.push_back(r); });
    b.navigate("https://map.example/");
    settle(b);
    b.mouse_move(50, 50);
    CHECK(b.hover_link() == "https://map.example/left.html");
    CHECK(b.cursor() == "pointer");
    b.mouse_move(150, 50);
    CHECK(b.hover_link() == "https://map.example/right.html");
    b.mouse_move(190, 95); // only the nohref default area
    CHECK(b.hover_link().empty());
    click(b, 150, 50);     // target=_blank on an area
    CHECK(asked.size() == 1 && asked.back().url == "https://map.example/right.html");
    b.mouse_move(20, 170); // polygon (triangle): inside, then outside
    CHECK(b.hover_link() == "https://map.example/tri.html");
    b.mouse_move(190, 240);
    CHECK(b.hover_link().empty());
    click(b, 30, 120);     // ismap: the click position goes to the server
    CHECK(b.url_for_bar() == "https://map.example/cgi/map?30,20");
    if (b.url_for_bar() != "https://map.example/cgi/map?30,20") std::printf("  got %s\n", b.url_for_bar().c_str());
    b.navigate("https://map.example/");
    settle(b);
    click(b, 50, 50);
    CHECK(b.url_for_bar() == "https://map.example/left.html");
    CHECK(rgb(shot(b), 400, 500) == 0x00ff00);
}

static void wait_ms(Browser& b, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    b.poll();
}

static void test_font_display() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    auto inked = [](const Framebuffer& f, int row) { return has_color(f, 0, row * 80, 300, row * 80 + 80, 0x000000, 60); };
    auto t0 = std::chrono::steady_clock::now();
    b.navigate("https://fonts.example/display");
    while (b.loading() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) wait_ms(b, 5);
    auto shown = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(shown < 650); // doesn't wait for the swap font (700 ms) — only for the block one (400 ms)
    Framebuffer f = shot(b);
    CHECK(!inked(f, 0) && !inked(f, 1) && inked(f, 2));
    wait_ms(b, 1200);
    f = shot(b);
    CHECK(inked(f, 0));  // swap: in once it arrives
    CHECK(!inked(f, 1)); // optional and late: not this time
}

static void test_web_fonts() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    fake::clear_log();
    b.navigate("https://fonts.example/");
    settle(b);
    auto inked = [](const Framebuffer& f, int row) { return has_color(f, 0, row * 80, 300, row * 80 + 80, 0x000000, 60); };
    Framebuffer f = shot(b);
    CHECK(inked(f, 0) == woff2_supported());              // WOFF2 (if built with Brotli; else the WOFF fallback below)
    if (!woff2_supported()) CHECK(was_fetched("https://fonts.example/icons.woff"));
    CHECK(inked(f, 0) || was_fetched("https://fonts.example/icons.woff"));
    CHECK(inked(f, 1));                                   // a src list: the missing WOFF2 is skipped, the TTF used
    CHECK(!inked(f, 2));                                  // garbage: refused, nothing drawn
    CHECK(!inked(f, 3));                                  // still downloading: the fallback (blank for an icon)
    CHECK(inked(f, 4));                                   // a page may call its own font "Arial"...
    CHECK(!was_fetched("https://fonts.example/range.ttf")); // unicode-range Cyrillic: the text has none
    CHECK(!inked(f, 5));
    CHECK(!was_fetched("https://fonts.example/unused.ttf"));
    wait_ms(b, 3000);                                     // ...and the slow one arrives: the page lays out again
    CHECK(inked(shot(b), 3));
    // ...but its fonts stay on its page
    Browser o(st, [] {});
    o.set_viewport(W, H);
    o.navigate("https://other-tab.example/");
    settle(o);
    CHECK(!has_color(shot(o), 0, 0, 300, 80, 0x000000, 60));
    // reloading: from the memory cache, no new download
    fake::clear_log();
    b.reload();
    settle(b);
    CHECK(!was_fetched("https://fonts.example/icons.ttf"));
    CHECK(inked(shot(b), 1));
    // web fonts off
    Settings off = st;
    off.web_fonts = false;
    Browser n(off, [] {});
    n.set_viewport(W, H);
    n.navigate("https://fonts.example/");
    settle(n);
    CHECK(!inked(shot(n), 1));
}

static void test_favicons() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://icon.example/");
    settle(b);
    wait_ms(b, 200);
    auto ic = b.favicon();
    CHECK(ic && ic->w == 32 && ic->h == 32);
    if (ic) CHECK((ic->px[16 * 32 + 16] & 0xffffff) == 0xff0000);
    b.navigate("https://ico.example/");
    settle(b);
    wait_ms(b, 200);
    ic = b.favicon();
    CHECK(ic != nullptr);
    if (ic) { // the 2x2 icon enlarged by whole pixels: top-left quarter transparent, the rest blue
        CHECK((ic->px[0] >> 24) == 0 && (ic->px[15 * 32 + 15] >> 24) == 0);
        CHECK(ic->px[31] == 0xff0000ffu);
        CHECK(ic->px[31 * 32] == 0xff0000ffu);
        CHECK(ic->px[31 * 32 + 31] == 0xff0000ffu);
    }
    b.navigate("https://noicon.example/");
    settle(b);
    wait_ms(b, 200);
    CHECK(b.favicon() == nullptr);
    b.navigate("about:home");
    settle(b);
    CHECK(b.favicon() == nullptr);
    Settings off = st;
    off.favicons = false;
    Browser n(off, [] {});
    n.set_viewport(W, H);
    fake::clear_log();
    n.navigate("https://ico.example/");
    settle(n);
    wait_ms(n, 100);
    CHECK(!was_fetched("https://ico.example/favicon.ico"));
}

static void test_tile_cache() {
    Settings cached, direct;
    cached.doh_url = direct.doh_url = "";
    direct.render_cache_mb = 0;
    Browser a(cached, [] {}), b(direct, [] {});
    for (Browser* x : {&a, &b}) {
        x->set_viewport(W, H);
        x->navigate("https://long.example/");
        settle(*x);
    }
    CHECK(a.content_height() > 5 * H);
    auto same = [&](const char* what) {
        Framebuffer fa = shot(a), fb = shot(b);
        bool eq = fa.px == fb.px;
        if (!eq) std::printf("  tile cache differs from a direct paint: %s (scroll %d)\n", what, a.scroll_y());
        return eq;
    };
    // scrolling down and back up in wheel steps, including odd offsets that cut strips mid-way
    bool all = true;
    for (int y = 0; y < a.content_height(); y += 157) {
        a.scroll_to(y);
        b.scroll_to(y);
        all &= same("scrolling");
    }
    for (int y = a.content_height(); y >= 0; y -= 211) {
        a.scroll_to(y);
        b.scroll_to(y);
        all &= same("scrolling back");
    }
    CHECK(all);
    // idle time fills the cache ahead of the screen
    a.scroll_to(0);
    b.scroll_to(0);
    shot(a);
    for (int i = 0; i < 50; i++) a.poll();
    // clicking a checkbox and typing must not leave stale strips behind (and must really change the page)
    Framebuffer before = shot(a);
    click(a, 13, 68);
    click(b, 13, 68);
    CHECK(shot(a).px != before.px);
    CHECK(same("checkbox click"));
    click(a, 120, 68);
    click(b, 120, 68);
    CHECK(a.form_wants_text());
    before = shot(a);
    a.form_text("xyz");
    b.form_text("xyz");
    CHECK(shot(a).px != before.px);
    CHECK(same("typing"));
    a.form_blur();
    b.form_blur();
    a.scroll_to(1234);
    b.scroll_to(1234);
    CHECK(same("after edits, scrolled"));
}

// ------------------------------------------------------------------ this round: list boxes, file upload, hover, sticky, gradients
static void build_site2() {
    auto& s = fake::site();
    const std::string row = "style=\"display:block;box-sizing:border-box;width:200px;margin:0;";
    s["https://forms2.example/"].body =
        "<html><body style=\"margin:0\"><form action=\"up\" method=post enctype=\"multipart/form-data\" target=_blank>"
        "<select name=m multiple " + row + "height:100px;border:0;padding:0;font-size:16px\">"
        "<option value=a>A<option value=b selected>B<option value=c>C<option value=d>D<option value=e>E</select>"
        "<input type=file name=f " + row + "height:30px\">"
        "<input type=text name=t value=\"x\" " + row + "height:20px\">"
        "<input type=submit " + row + "height:40px\"></form>"
        "<form action=\"get\" target=_blank><select name=s size=3 " + row + "height:60px;border:0;padding:0\">"
        "<option>one<option selected>two<option>three</select><input type=file name=g " + row + "height:30px\">"
        "<input type=submit " + row + "height:40px\"></form></body></html>";
    s["https://hover.example/"].body =
        "<html><head><style>body{margin:0} #a{display:block;height:50px;background:#0000ff} #a:hover{background:#ff0000}"
        "#b{display:block;height:50px;width:100px;background:#00ff00} #b:hover{width:300px}"
        "#c{height:50px;background:#000080}</style></head><body><a id=a href=x>a</a><div id=b></div><div id=c></div></body></html>";
    s["https://sticky.example/"].body =
        "<html><head><style>body{margin:0} #s{position:sticky;top:0;height:40px;background:#ff00ff}"
        "#f{position:fixed;left:0;bottom:0;width:100px;height:40px;background:#00ffff;display:block}"
        ".tall{height:3000px;background:#ffffff}</style></head><body><div style=\"height:100px;background:#808080\"></div>"
        "<a id=s href=sticky.html style=\"display:block\"></a><div class=tall></div><a id=f href=fixed.html></a></body></html>";
    s["https://grad.example/"].body =
        "<html><body style=\"margin:0\"><div style=\"height:100px;background:linear-gradient(to right,#ff0000,#0000ff)\"></div>"
        "<div style=\"height:100px;background:#00ff00 linear-gradient(#ff000000,#ff000000)\"></div>"
        "<div style=\"width:100px;height:100px;margin:50px;background:#0000ff;transform:rotate(45deg)\"></div>"
        "<div style=\"width:100px;height:50px;background:#ff0000;transform:translate(300px,-100px) scale(2)\"></div></body></html>";
}

static void build_site3() {
    auto& s = fake::site();
    s["https://obj.example/"].body =
        "<html><body style=\"margin:0\">"
        "<object data=\"https://img.example/red.gif\" width=100 height=50 style=\"display:block\">no</object>"                 // 0..50: a picture
        "<object data=\"movie.swf\" type=\"application/x-shockwave-flash\" style=\"display:block\"><param name=movie value=movie.swf>"
        "<div style=\"height:50px;background:#00ff00\"></div></object>"                                                          // 50..100: fallback
        "<object data=\"inner.html\" style=\"display:block;width:200px;height:100px;border:0\"></object>"                    // 100..200: a page
        "<embed src=\"x.swf\" type=\"application/x-shockwave-flash\" width=100 height=50 style=\"display:block\">"          // 200..250: nothing
        "<embed src=\"https://img.example/red.gif\" width=100 height=50 style=\"display:block\">"                             // 250..300: a picture
        "</body></html>";
    s["https://obj.example/inner.html"].body = page("#0000ff", "");
    s["https://old.example/"].body =
        "<html><body style=\"margin:0\"><marquee scrollamount=40 scrolldelay=50 truespeed style=\"width:400px;height:40px\">"
        "<a href=\"m.html\" style=\"display:inline-block;width:50px;height:40px;background:#0000ff\"></a></marquee>"
        "<blink style=\"font-size:40px;color:#ff0000\">XXXX</blink>"
        "<marquee behavior=slide scrollamount=1000 scrolldelay=60 style=\"width:400px;height:40px\">"
        "<span style=\"display:inline-block;width:50px;height:40px;background:#00ff00\"></span></marquee></body></html>";
}

static void test_object_embed() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://obj.example/");
    settle(b);
    settle(b);
    Framebuffer f = shot(b);
    CHECK(rgb(f, 50, 25) == 0xff0000);  // object data=picture
    CHECK(rgb(f, 50, 75) == 0x00ff00);  // fallback content of a Flash object
    CHECK(rgb(f, 100, 150) == 0x0000ff); // object data=page: shown like an iframe
    CHECK(rgb(f, 50, 225) == 0xffffff);  // an embed Quill can't show: an empty box
    CHECK(rgb(f, 50, 275) == 0xff0000);  // embed src=picture
}

static void test_marquee_blink() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://old.example/");
    settle(b);
    auto blue_x = [](const Framebuffer& f) {
        for (int x = 0; x < 400; x++)
            if (rgb(f, x, 20) == 0x0000ff) return x;
        return -1;
    };
    std::vector<int> xs;
    bool seen_on = false, seen_off = false, link = false;
    for (int i = 0; i < 25; i++) {
        Framebuffer f = shot(b);
        int x = blue_x(f);
        xs.push_back(x);
        CHECK(rgb(f, 420, 20) == 0xffffff); // never drawn outside the marquee
        if (x >= 0 && x < 360) {
            b.mouse_move(x + 25, 20); // the link is where it is drawn
            link = link || b.hover_link() == "https://old.example/m.html";
        }
        if (has_color(f, 0, 40, 200, 80, 0xff0000, 30)) seen_on = true;
        else seen_off = true;
        wait_ms(b, 60);
    }
    int moved = 0, visible = 0;
    for (size_t i = 1; i < xs.size(); i++) {
        if (xs[i] >= 0) visible++;
        if (xs[i] >= 0 && xs[i - 1] >= 0 && xs[i] < xs[i - 1]) moved++;
    }
    CHECK(visible > 3 && moved > 2); // right to left
    CHECK(link);
    CHECK(seen_on && seen_off);       // blinking
    Framebuffer f = shot(b);
    CHECK(rgb(f, 25, 100) == 0x00ff00); // the slide has arrived at the left and stays
}

static void build_site4() {
    auto& s = fake::site();
    // the Gentoo packages layout: a full-height flex column body that hides sideways overflow
    s["https://tall.example/"].body =
        "<html><head><style>html,body{height:100%;margin:0}body{display:flex;flex-direction:column;overflow-x:hidden}"
        "</style></head><body><div style=\"height:1000px;background:#ff0000;flex-shrink:0\"></div>"
        "<div style=\"height:500px;background:#00ff00;flex-shrink:0\"></div></body></html>";
    // the page scrolls inside <body>: html hides, body scrolls
    s["https://inner.example/"].body =
        "<html><head><style>html{overflow:hidden;height:100%}body{margin:0;height:100%;overflow-y:auto}</style></head><body>"
        "<div style=\"height:1000px;background:#ff0000\"></div><div style=\"height:500px;background:#0000ff\"></div></body></html>";
    // an app layout: a full-height flex row whose columns scroll on their own
    s["https://app.example/"].body =
        "<html><head><style>html,body{height:100%;margin:0;overflow:hidden}body{display:flex}nav{width:200px;overflow-y:auto}"
        "main{flex:1;overflow-y:auto}</style></head><body><nav><div style=\"height:900px;background:#00ffff\"></div></nav>"
        "<main><div style=\"height:700px;background:#ff0000\"></div><div style=\"height:2000px;background:#0000ff\"></div></main></body></html>";
    s["https://box.example/"].body =
        "<html><body style=\"margin:0\"><div style=\"height:200px;width:300px;overflow:auto\">"
        "<div style=\"height:150px;background:#ff0000\"></div><a href=\"in.html\" style=\"display:block;height:150px;background:#00ff00\"></a>"
        "<div style=\"height:300px;background:#0000ff\"></div></div>"
        "<div style=\"height:2000px;background:#ffff00\"></div></body></html>";
}

static void test_scrolling() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://tall.example/");
    settle(b);
    CHECK(b.content_height() == 1500);
    b.mouse_move(400, 300);
    b.scroll_by(900);
    CHECK(b.scroll_y() == 900);
    Framebuffer f = shot(b);
    CHECK(rgb(f, 400, 50) == 0xff0000 && rgb(f, 400, 200) == 0x00ff00); // painted below the first screen too
    // dragging the page's scroll bar
    b.scroll_to(0);
    b.mouse_down(W - 4, 10);
    b.mouse_move(W - 4, 160);
    b.mouse_up(W - 4, 160);
    CHECK(b.scroll_y() > 200 && b.scroll_y() <= 900);
    int after = b.scroll_y();
    click(b, 400, 300); // and an ordinary click doesn't move it
    CHECK(b.scroll_y() == after);

    b.navigate("https://inner.example/");
    settle(b);
    b.mouse_move(400, 300);
    CHECK(rgb(shot(b), 400, 300) == 0xff0000);
    b.scroll_by(800); // <body> is the scroller
    f = shot(b);
    CHECK(b.scroll_y() == 0);
    CHECK(rgb(f, 400, 100) == 0xff0000 && rgb(f, 400, 300) == 0x0000ff);
    CHECK(has_color(f, W - 12, 0, W, H, 0x000000, 140)); // its scroll bar

    b.navigate("https://app.example/");
    settle(b);
    CHECK(b.content_height() == H); // the window itself doesn't scroll
    b.mouse_move(500, 300);
    CHECK(rgb(shot(b), 500, 300) == 0xff0000);
    b.scroll_by(600);
    CHECK(rgb(shot(b), 500, 300) == 0x0000ff); // <main> did
    CHECK(rgb(shot(b), 100, 300) == 0x00ffff);

    b.navigate("https://box.example/");
    settle(b);
    b.mouse_move(100, 50);
    CHECK(rgb(shot(b), 100, 50) == 0xff0000);
    b.scroll_by(150); // over the box: the box scrolls, the page doesn't
    CHECK(b.scroll_y() == 0);
    f = shot(b);
    CHECK(rgb(f, 100, 50) == 0x00ff00);
    CHECK(rgb(f, 100, 175) == 0x0000ff);
    b.mouse_move(100, 50);
    CHECK(b.hover_link() == "https://box.example/in.html"); // hit where it is drawn
    b.scroll_by(1000); // to its end (400)...
    CHECK(b.scroll_y() == 0);
    CHECK(rgb(shot(b), 100, 100) == 0x0000ff);
    b.scroll_by(100); // ...then the page takes over
    CHECK(b.scroll_y() == 100);
    b.scroll_to(0);
    b.mouse_move(500, 300); // outside the box: the page
    b.scroll_by(60);
    CHECK(b.scroll_y() == 60);
    // dragging the box's own scroll bar back to the top
    b.scroll_to(0);
    b.mouse_down(295, 190);
    b.mouse_move(295, -400);
    b.mouse_up(295, -400);
    CHECK(rgb(shot(b), 100, 50) == 0xff0000);
    CHECK(b.scroll_y() == 0);
}

static void test_request_headers() {
    auto& site = fake::site();
    site["https://ref.example/page"].body =
        "<html><head><link rel=stylesheet href=\"s.css\"></head><body style=\"margin:0\">" + block_link("next#top", "same") +   // y 0..60
        block_link("https://other.example/x", "cross") + block_link("https://other.example/nr", "noref", "rel=\"external noreferrer\"") +
        "<form method=post action=\"/post\"><input type=submit style=\"display:block;height:60px;width:200px\"></form>" + // y 180..240
        "<img src=\"https://cdn.ref.example/i.png\" width=10 height=10></body></html>";
    site["https://ref.example/page?x=1"] = site["https://ref.example/page"];
    site["https://ref.example/s.css"] = {"body{color:#000}", "text/css", "", 0, "", "", "", ""};
    site["https://ref.example/next"].body = page("#ffffff", "next");
    site["https://ref.example/post"].body = page("#ffffff", "posted");
    site["https://other.example/x"].body = page("#ffffff", "x");
    site["https://other.example/nr"].body = page("#ffffff", "nr");
    site["https://ref.example/quiet"].body =
        "<html><head><meta name=\"referrer\" content=\"no-referrer\"></head><body style=\"margin:0\">" + block_link("next", "n") + "</body></html>";
    site["https://ref.example/loud"].body =
        "<html><head><meta name=\"referrer\" content=\"unsafe-url\"></head><body style=\"margin:0\">" + block_link("https://other.example/x", "n") + "</body></html>";
    site["https://ref.example/hdr"] = {"<html><body style=\"margin:0\">" + block_link("next", "n") + "</body></html>", "text/html", "", 0, "", "", "", "origin"};
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    auto hv = [](const char* u, const char* n) { return fake::header_value(u, n); };
    b.navigate("https://ref.example/page?x=1#frag"); // typed
    settle(b);
    CHECK(hv("https://ref.example/page?x=1", "Referer").empty());
    CHECK(hv("https://ref.example/page?x=1", "Sec-Fetch-Site") == "none");
    CHECK(hv("https://ref.example/page?x=1", "Sec-Fetch-Dest") == "document" && hv("https://ref.example/page?x=1", "Sec-Fetch-User") == "?1");
    CHECK(hv("https://ref.example/page?x=1", "Upgrade-Insecure-Requests") == "1");
    CHECK(hv("https://ref.example/page?x=1", "Accept").rfind("text/html,", 0) == 0);
    CHECK(hv("https://ref.example/s.css", "Referer") == "https://ref.example/page?x=1"); // no #fragment
    CHECK(hv("https://ref.example/s.css", "Sec-Fetch-Dest") == "style" && hv("https://ref.example/s.css", "Sec-Fetch-Site") == "same-origin");
    CHECK(hv("https://cdn.ref.example/i.png", "Referer") == "https://ref.example/"); // another origin: just the origin
    CHECK(hv("https://cdn.ref.example/i.png", "Sec-Fetch-Site") == "same-site" && hv("https://cdn.ref.example/i.png", "Sec-Fetch-Mode") == "no-cors");
    click(b, 30, 30);
    settle(b);
    CHECK(hv("https://ref.example/next", "Referer") == "https://ref.example/page?x=1");
    CHECK(hv("https://ref.example/next", "Sec-Fetch-Site") == "same-origin");
    b.back();
    settle(b);
    click(b, 30, 90);
    settle(b);
    CHECK(hv("https://other.example/x", "Referer") == "https://ref.example/");
    CHECK(hv("https://other.example/x", "Sec-Fetch-Site") == "cross-site");
    b.back();
    settle(b);
    click(b, 30, 150);
    settle(b);
    CHECK(b.url_for_bar() == "https://other.example/nr");
    CHECK(hv("https://other.example/nr", "Referer").empty()); // rel=noreferrer
    b.back();
    settle(b);
    click(b, 30, 210); // the form: a POST from the same origin
    settle(b);
    CHECK(b.url_for_bar() == "https://ref.example/post");
    CHECK(hv("https://ref.example/post", "Referer") == "https://ref.example/page?x=1");
    CHECK(hv("https://ref.example/post", "Sec-Fetch-Site") == "same-origin");
    // reload sends the same Referer again
    fake::clear_log();
    b.navigate("https://ref.example/page?x=1");
    settle(b);
    click(b, 30, 30);
    settle(b);
    b.reload();
    settle(b);
    CHECK(hv("https://ref.example/next", "Referer") == "https://ref.example/page?x=1");
    // the page's policy: <meta name=referrer> and the Referrer-Policy header
    b.navigate("https://ref.example/quiet");
    settle(b);
    click(b, 30, 30);
    settle(b);
    CHECK(hv("https://ref.example/next", "Referer").empty());
    b.navigate("https://ref.example/hdr");
    settle(b);
    click(b, 30, 30);
    settle(b);
    CHECK(hv("https://ref.example/next", "Referer") == "https://ref.example/");
    b.navigate("https://ref.example/loud"); // unsafe-url is capped: other sites still only get the origin
    settle(b);
    click(b, 30, 30);
    settle(b);
    CHECK(hv("https://other.example/x", "Referer") == "https://ref.example/");
    // referrer = off
    Settings st2 = st;
    st2.referrer = "off";
    Browser c(st2, [] {});
    c.set_viewport(W, H);
    c.navigate("https://ref.example/page");
    settle(c);
    click(c, 30, 30);
    settle(c);
    CHECK(hv("https://ref.example/next", "Referer").empty());
    CHECK(hv("https://ref.example/next", "Sec-Fetch-Site") == "same-origin");
    // pressing a link to another host warms up a connection to it
    size_t before = fake::preconnected().size();
    c.navigate("https://ref.example/page");
    settle(c);
    c.mouse_down(30, 90);
    for (int i = 0; i < 50 && fake::preconnected().size() == before; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(fake::preconnected().size() == before + 1 && fake::preconnected().back() == "https://other.example/x");
    c.mouse_up(30, 90);
    settle(c);
    c.back();
    settle(c);
    before = fake::preconnected().size();
    c.mouse_down(30, 30); // same host: nothing to warm up
    c.mouse_up(30, 30);
    settle(c);
    CHECK(fake::preconnected().size() == before);
}

static void test_zoom() {
    auto& site = fake::site();
    site["https://zoom.example/"].body =
        "<html><head><style>body{margin:0}#r{width:100px;height:50px;background:#ff0000}"
        "@media (max-width:500px){body{background:#00ff00}}"
        "#v{position:fixed;left:0;bottom:0;width:10vw;height:10vh;background:#0000ff}</style></head><body>"
        "<div id=r></div><img src=\"/p.png\" style=\"display:block\"><a href=\"/l\" style=\"display:block;width:100px;height:20px\">x</a>"
        "<iframe src=\"/in\" style=\"display:block;width:200px;height:100px;border:0\"></iframe>"
        "<div style=\"height:3000px\"></div><div id=v></div></body></html>";
    site["https://zoom.example/p.png"] = {solid_png(20, 20, 0x00ffff), "image/png", "", 0, "", "", "", ""};
    site["https://zoom.example/in"].body = "<html><body style=\"margin:0\"><div style=\"width:50px;height:20px;background:#ff00ff\"></div></body></html>";
    site["https://zoom2.example/"].body = site["https://zoom.example/"].body;
    site["https://zoom2.example/p.png"] = site["https://zoom.example/p.png"];
    site["https://zoom2.example/in"] = site["https://zoom.example/in"];
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H); // 800 x 600
    b.navigate("https://zoom.example/");
    settle(b);
    settle(b);
    Framebuffer f = shot(b);
    CHECK(b.zoom_percent() == 100);
    CHECK(rgb(f, 99, 49) == 0xff0000 && rgb(f, 101, 25) != 0xff0000);
    CHECK(rgb(f, 19, 60) == 0x00ffff && rgb(f, 21, 60) != 0x00ffff); // the picture: 20x20 at 50..70
    CHECK(rgb(f, 400, 300) != 0x00ff00);
    CHECK(rgb(f, 79, H - 59) == 0x0000ff && rgb(f, 81, H - 30) != 0x0000ff); // 10vw x 10vh
    b.zoom_in();
    b.zoom_in();
    b.zoom_in(); // 110 -> 120 -> 133
    b.zoom_in(); // 150
    settle(b);
    CHECK(b.zoom_percent() == 150);
    f = shot(b);
    CHECK(rgb(f, 148, 73) == 0xff0000 && rgb(f, 152, 37) != 0xff0000);   // 150 x 75
    CHECK(rgb(f, 29, 75 + 29) == 0x00ffff && rgb(f, 31, 90) != 0x00ffff); // 30 x 30 picture, sharp where it is
    CHECK(rgb(f, 400, 300) != 0x00ff00); // 800 / 1.5 = 533 CSS px: still wider than 500
    CHECK(rgb(f, 77, H - 57) == 0x0000ff && rgb(f, 82, H - 30) != 0x0000ff); // vw / vh: the same share of the window
    CHECK(rgb(f, 10, 75 + 30 + 30 + 10) == 0xff00ff && rgb(f, 74, 75 + 30 + 30 + 10) == 0xff00ff); // the iframe's page zooms too
    b.mouse_move(140, 75 + 30 + 10);
    CHECK(b.hover_link() == "https://zoom.example/l"); // hit where it is drawn
    b.zoom_in();
    b.zoom_in(); // 170, 200: 400 CSS px wide
    f = shot(b);
    CHECK(b.zoom_percent() == 200);
    CHECK(rgb(f, 400, 300) == 0x00ff00); // (max-width: 500px) matches now
    // the scroll position stays on the same text
    b.scroll_to(1000);
    b.zoom_out(); // 170
    CHECK(b.scroll_y() == 850);
    // the site keeps its zoom: another tab on it, a new page on it; other sites don't
    Browser c(st, [] {});
    c.set_viewport(W, H);
    c.navigate("https://zoom.example/");
    settle(c);
    CHECK(c.zoom_percent() == 170);
    c.navigate("https://zoom2.example/");
    settle(c);
    CHECK(c.zoom_percent() == 100);
    c.navigate("https://zoom.example/");
    settle(c);
    b.zoom_reset();
    b.scroll_to(0);
    CHECK(b.zoom_percent() == 100);
    c.refresh_zoom(); // what main does when the tab is shown
    CHECK(c.zoom_percent() == 100);
    f = shot(b);
    CHECK(rgb(f, 99, 49) == 0xff0000 && rgb(f, 101, 25) != 0xff0000);
    // zoom = 150 in the settings: the default (and what reset goes back to)
    Settings s2 = st;
    s2.zoom = 150;
    Browser d(s2, [] {});
    d.set_viewport(W, H);
    d.navigate("https://zoom2.example/");
    settle(d);
    CHECK(d.zoom_percent() == 150);
    d.zoom_in();
    CHECK(d.zoom_percent() == 170);
    d.zoom_reset();
    CHECK(d.zoom_percent() == 150);
    CHECK(rgb(shot(d), 148, 73) == 0xff0000);
    // the window's height changing re-lays out (vh units, fixed boxes at the bottom)
    b.set_viewport(W, 400);
    f = shot(b);
    CHECK(rgb(f, 79, 400 - 39) == 0x0000ff && rgb(f, 79, 400 - 41) != 0x0000ff);
}

static void test_text_selection() {
    auto& site = fake::site();
    const std::string css = "<style>body{margin:0;font-size:20px;line-height:30px} p{margin:0}</style>";
    site["https://sel.example/"].body =
        "<html><head>" + css + "</head><body><p>Hello brave new world</p><p>Second paragraph here</p>"   // lines at 0, 30
        "<p><a href=\"/x\">link text</a> after</p>"                                                       // 60
        "<div style=\"width:120px\">one two three four five six seven</div>"                            // wraps, from 90
        "<pre style=\"margin:0\">pre one\npre two</pre></body></html>";
    site["https://sel.example/x"].body = page("#ffffff", "x");
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://sel.example/");
    settle(b);
    CHECK(!b.has_selection());
    // drag from the start of the first line to the end of the second
    b.mouse_down(1, 15);
    b.mouse_move(200, 20);
    b.mouse_move(W - 20, 45);
    b.mouse_up(W - 20, 45);
    CHECK(b.selected_text() == "Hello brave new world\nSecond paragraph here");
    Framebuffer f = shot(b);
    CHECK(has_color(f, 0, 0, 100, 30, 0x338eff, 8)); // highlighted
    CHECK(has_color(f, 0, 0, 100, 30, 0xffffff, 8)); // the text on it white
    // part of a line
    b.mouse_down(1, 15);
    b.mouse_move(60, 15);
    b.mouse_up(60, 15);
    std::string part = b.selected_text();
    CHECK(!part.empty() && part.size() < 10 && std::string("Hello brave new world").rfind(part, 0) == 0);
    // shift+click extends it
    Browser::set_click_modifiers(false, true);
    b.mouse_down(W - 20, 15);
    b.mouse_up(W - 20, 15);
    Browser::set_click_modifiers(false, false);
    CHECK(b.selected_text() == "Hello brave new world");
    // a click clears it
    click(b, 400, 300);
    CHECK(!b.has_selection());
    CHECK(rgb(shot(b), 400, 300) == 0xffffff);
    // double-click: a word; triple-click: the block
    Browser::set_click_count(2);
    b.mouse_down(70, 15);
    b.mouse_up(70, 15);
    Browser::set_click_count(1);
    std::string w = b.selected_text();
    CHECK(w == "Hello" || w == "brave");
    Browser::set_click_count(3);
    b.mouse_down(70, 45);
    b.mouse_up(70, 45);
    Browser::set_click_count(1);
    CHECK(b.selected_text() == "Second paragraph here");
    // wrapped lines read as one line; preformatted text keeps its lines
    Browser::set_click_count(3);
    b.mouse_down(10, 95);
    b.mouse_up(10, 95);
    CHECK(b.selected_text() == "one two three four five six seven");
    Browser::set_click_count(1);
    // a drag across a link doesn't follow it
    b.mouse_down(W - 20, 75);
    b.mouse_move(1, 75);
    b.mouse_up(1, 75);
    settle(b);
    CHECK(b.url_for_bar() == "https://sel.example/");
    CHECK(b.selected_text() == "link text after");
    // Ctrl+A: everything
    b.select_all_text();
    std::string all = b.selected_text();
    CHECK(all.rfind("Hello brave new world\nSecond paragraph here\nlink text after\none two three four five six seven\npre one\npre two", 0) == 0);
    // the selection survives a new layout (the window getting narrower)
    b.set_viewport(W - 200, H);
    CHECK(b.selected_text() == all);
    b.set_viewport(W, H);
    // and goes with the page
    b.navigate("https://sel.example/x");
    settle(b);
    CHECK(!b.has_selection());
}

static void test_http_auth() {
    auto& site = fake::site();
    site["https://nas.example/"].body = "<html><head><title>NAS home</title></head><body style=\"margin:0\">"
                                        "<img src=\"/logo.png\" width=10 height=10>signed in</body></html>";
    site["https://nas.example/logo.png"] = {solid_png(4, 4, 0x00ff00), "image/png", "", 0, "", "", "", ""};
    fake::auth_needed()["https://nas.example/"]         = {"Basic realm=\"My NAS\"", "admin", "s3cret"};
    fake::auth_needed()["https://nas.example/logo.png"] = {"Basic realm=\"My NAS\"", "admin", "s3cret"};
    site["https://evil.example/"].body = "<html><body><img src=\"https://nas.example/logo.png\" width=10 height=10></body></html>";
    CredentialStore::instance().clear();
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://nas.example/");
    settle(b);
    CHECK(b.title() == "Sign in");
    CHECK(b.url_for_bar() == "https://nas.example/");
    CHECK(b.form_wants_text()); // the username field has the keyboard (autofocus)
    auto type_and_submit = [&](const std::string& u, const std::string& p) {
        b.form_text(u);
        b.form_key(FormKey::Tab);
        b.form_text(p);
        b.form_key(FormKey::Enter);
        settle(b);
    };
    type_and_submit("admin", "nope");
    CHECK(b.title() == "Sign in"); // refused: asked again
    type_and_submit("admin", "s3cret");
    settle(b);
    CHECK(b.title() == "NAS home");
    CHECK(rgb(shot(b), 5, 5) == 0x00ff00); // its own picture got the credentials too
    // another site's page doesn't get to use them
    Browser e(st, [] {});
    e.set_viewport(W, H);
    e.navigate("https://evil.example/");
    settle(e);
    CHECK(rgb(shot(e), 5 + 8, 5 + 8) != 0x00ff00);
    // Cancel shows the site's own 401 page
    CredentialStore::instance().clear();
    Browser c(st, [] {});
    c.set_viewport(W, H);
    c.navigate("https://nas.example/");
    settle(c);
    CHECK(c.title() == "Sign in");
    c.navigate("quill:auth-cancel"); // (what its Cancel link does)
    settle(c);
    CHECK(c.title() != "Sign in");
    CHECK(c.url_for_bar() == "https://nas.example/");
    // a web page can't fake a submission into it
    site["https://evil.example/form"].body = "<html><body><form method=post action=\"quill:auth\"><input name=user value=x>"
                                             "<input type=submit style=\"display:block;width:200px;height:50px\"></form></body></html>";
    Browser d(st, [] {});
    d.set_viewport(W, H);
    d.navigate("https://evil.example/form");
    settle(d);
    click(d, 50, 30);
    settle(d);
    HttpCredentials got;
    CHECK(!CredentialStore::instance().get("https://evil.example", got));
    CredentialStore::instance().clear();
    fake::auth_needed().clear();
}

static std::string g_tmpdir();
static void test_history() {
    auto& site = fake::site();
    site["https://hist.example/a"].body = "<html><head><title>Alpha page</title></head><body>a</body></html>";
    site["https://hist.example/b"].body = "<html><head><title>Beta page</title></head><body>b</body></html>";
    site["https://hist.example/evil"].body = "<html><body style=\"margin:0\">" + block_link("quill:clear-history", "clear") + "</body></html>";
    std::string path = g_tmpdir() + "/quill-history-test.tsv";
    std::remove(path.c_str());
    History& h = History::instance();
    h.configure(true, path, 90);
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://hist.example/a");
    settle(b);
    b.navigate("https://hist.example/b");
    settle(b);
    b.navigate("https://hist.example/b"); // a reload: not a second entry
    settle(b);
    CHECK(h.size() == 2);
    auto links = [&](int x) {
        std::vector<std::string> out;
        for (int y = 0; y < H; y += 3) {
            b.mouse_move(x, y);
            std::string l = b.hover_link();
            if (!l.empty() && (out.empty() || out.back() != l)) out.push_back(l);
        }
        return out;
    };
    b.navigate("about:history");
    settle(b);
    CHECK(b.title() == "History");
    std::vector<std::string> l = links(300);
    CHECK(l.size() == 2 && l[0] == "https://hist.example/b" && l[1] == "https://hist.example/a"); // newest first
    b.navigate("about:history?q=ALPHA"); // titles are searched too, ignoring case
    settle(b);
    l = links(300);
    CHECK(l.size() == 1 && l[0] == "https://hist.example/a");
    // the page's own search box
    b.navigate("about:history");
    settle(b);
    bool typed = false;
    for (int y = 60; y < 140 && !typed; y += 4) {
        click(b, 200, y);
        if (b.form_wants_text()) typed = b.form_text("beta");
    }
    CHECK(typed);
    b.form_key(FormKey::Enter);
    settle(b);
    CHECK(b.url_for_bar() == "about:history?q=beta");
    l = links(300);
    CHECK(l.size() == 1 && l[0] == "https://hist.example/b");
    // kept on disk
    h.configure(true, path, 90);
    CHECK(h.size() == 2);
    // a web page can't clear it
    b.navigate("https://hist.example/evil");
    settle(b);
    click(b, 30, 30);
    CHECK(h.size() == 3);
    // the Clear history link on about:history can
    b.navigate("about:history");
    settle(b);
    std::vector<std::string> right;
    for (int x = 400; x < W && right.empty(); x += 10) {
        for (int y = 40; y < 160; y += 4) {
            b.mouse_move(x, y);
            if (b.hover_link() == "quill:clear-history") { click(b, x, y); right.push_back("x"); break; }
        }
    }
    CHECK(!right.empty());
    CHECK(h.size() == 0);
    h.configure(true, path, 90);
    CHECK(h.size() == 0);
    // frames' pages aren't visits
    b.navigate("https://ifr.example/"); // a page with six iframes: one visit
    settle(b);
    settle(b);
    CHECK(h.size() == 1);
    h.configure(false, "", 90); // off: nothing recorded
    b.navigate("https://hist.example/a");
    settle(b);
    CHECK(h.size() == 0);
    std::remove(path.c_str());
}

static std::string g_tmpdir() {
    const char* t = std::getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}

static void test_listbox_and_upload() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    std::vector<Browser::NewTab> asked;
    b.set_open_tab([&](const Browser::NewTab& r) { asked.push_back(r); });
    std::string path = g_tmpdir() + "/quill-upload-test.txt";
    { std::ofstream o(path, std::ios::binary); o << "hello\r\nworld"; }
    int picks = 0;
    Browser::set_file_picker([&](bool multiple, const std::string&) {
        picks++;
        return multiple ? std::vector<std::string>{} : std::vector<std::string>{path};
    });
    b.navigate("https://forms2.example/");
    settle(b);
    Framebuffer f = shot(b);
    CHECK(has_color(f, 0, 25, 200, 50, 0xc8c8c8, 4)); // "B" starts selected (grey: not focused)
    click(b, 50, 12);                                  // plain click: only A
    f = shot(b);
    CHECK(has_color(f, 0, 0, 200, 25, 0x1e64dc, 4));   // focused: blue
    CHECK(!has_color(f, 0, 27, 200, 48, 0x1e64dc, 4));
    Browser::set_click_modifiers(true, false);
    click(b, 50, 62); // ctrl: add C
    Browser::set_click_modifiers(false, true);
    click(b, 50, 87); // shift: C..D only
    Browser::set_click_modifiers(false, false);
    f = shot(b);
    CHECK(!has_color(f, 0, 2, 200, 23, 0x1e64dc, 4));
    CHECK(has_color(f, 0, 52, 200, 73, 0x1e64dc, 4) && has_color(f, 0, 77, 200, 98, 0x1e64dc, 4));
    b.mouse_move(50, 50);
    b.scroll_by(40); // the wheel scrolls the list (5 options, 4 rows), not the page
    CHECK(b.scroll_y() == 0);
    click(b, 20, 115); // the file input's button
    CHECK(picks == 1);
    click(b, 50, 170); // submit
    CHECK(asked.size() == 1);
    if (!asked.empty()) {
        const Browser::NewTab& r = asked.back();
        CHECK(r.method == "POST" && r.url == "https://forms2.example/up");
        CHECK(r.content_type.rfind("multipart/form-data; boundary=", 0) == 0);
        std::string bd = r.content_type.substr(r.content_type.find('=') + 1);
        std::string want = "--" + bd + "\r\nContent-Disposition: form-data; name=\"m\"\r\n\r\nc\r\n--" + bd +
                           "\r\nContent-Disposition: form-data; name=\"m\"\r\n\r\nd\r\n--" + bd +
                           "\r\nContent-Disposition: form-data; name=\"f\"; filename=\"quill-upload-test.txt\"\r\nContent-Type: text/plain\r\n\r\nhello\r\nworld\r\n--" +
                           bd + "\r\nContent-Disposition: form-data; name=\"t\"\r\n\r\nx\r\n--" + bd + "--\r\n";
        CHECK(r.body == want);
        if (r.body != want) std::printf("got:\n%s\n", r.body.c_str());
    }
    // a size=3 single list box with GET: one value, the file input sends an empty name
    click(b, 50, 190 + 30); // "two" -> "three" is row 2 at y 190+40..190+60
    click(b, 50, 190 + 50);
    Framebuffer g = shot(b);
    CHECK(has_color(g, 0, 232, 200, 248, 0x1e64dc, 4));
    click(b, 50, 300);
    CHECK(asked.size() == 2 && asked.back().url == "https://forms2.example/get?s=three&g=");
    std::remove(path.c_str());
    Browser::set_file_picker(nullptr);
}

static void test_hover_paint_only() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://hover.example/");
    settle(b);
    CHECK(rgb(shot(b), 400, 25) == 0x0000ff);
    int n0 = b.layout_count();
    b.mouse_move(400, 25);
    settle(b);
    CHECK(rgb(shot(b), 400, 25) == 0xff0000); // the hover colour shows...
    CHECK(b.layout_count() == n0);            // ...without a layout
    b.mouse_move(400, 300);
    settle(b);
    CHECK(rgb(shot(b), 400, 25) == 0x0000ff);
    CHECK(b.layout_count() == n0);
    b.mouse_move(50, 75); // #b:hover changes the width: that one has to lay out
    settle(b);
    CHECK(rgb(shot(b), 250, 75) == 0x00ff00);
    CHECK(b.layout_count() > n0);
    b.mouse_move(400, 300);
    settle(b);
    // the engine busy on another thread (another page parsing): the hover waits instead of blocking the mouse...
    {
        std::unique_lock<std::mutex> lk(engine_mutex());
        auto t0 = std::chrono::steady_clock::now();
        b.mouse_move(400, 25);
        b.poll();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
        CHECK(rgb(shot(b), 400, 25) == 0x0000ff);
    }
    settle(b); // ...and lands once the engine is free
    CHECK(rgb(shot(b), 400, 25) == 0xff0000);
}

static void test_sticky_and_fixed() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://sticky.example/");
    settle(b);
    Framebuffer f = shot(b);
    CHECK(rgb(f, 400, 120) == 0xff00ff); // in flow at first
    CHECK(rgb(f, 400, 20) == 0x808080);
    CHECK(rgb(f, 50, H - 20) == 0x00ffff);
    b.scroll_to(1000);
    f = shot(b);
    CHECK(rgb(f, 400, 20) == 0xff00ff); // stuck to the top
    CHECK(rgb(f, 400, 60) == 0xffffff);
    CHECK(rgb(f, 50, H - 20) == 0x00ffff);
    b.mouse_move(400, 20);
    CHECK(b.hover_link() == "https://sticky.example/sticky.html");
    b.mouse_move(50, H - 20); // a fixed box is hit where it is drawn, whatever the scroll
    CHECK(b.hover_link() == "https://sticky.example/fixed.html");
    b.mouse_move(400, 300);
    CHECK(b.hover_link().empty());
}

static void test_gradients_transforms() {
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://grad.example/");
    settle(b);
    Framebuffer f = shot(b);
    uint32_t l = rgb(f, 2, 50), m = rgb(f, 400, 50), r = rgb(f, W - 3, 50);
    CHECK(((l >> 16) & 255) > 240 && (l & 255) < 15);
    CHECK(((r >> 16) & 255) < 15 && (r & 255) > 240);
    CHECK(((m >> 16) & 255) > 100 && ((m >> 16) & 255) < 155 && (m & 255) > 100 && (m & 255) < 155);
    CHECK(rgb(f, 400, 150) == 0x00ff00); // a transparent gradient over a colour
    // rotate(45deg) about the centre (100, 300): a diamond. Corners of the old square are now outside, the tips inside
    CHECK(rgb(f, 100, 300) == 0x0000ff);
    CHECK(rgb(f, 100, 300 - 66) == 0x0000ff); // top tip (half diagonal 70.7)
    CHECK(rgb(f, 55, 255) != 0x0000ff);       // old top-left corner
    CHECK(rgb(f, 168, 300) == 0x0000ff);
    // translate(300,-100) scale(2): the 100x50 box centred at (50+300, 425-100), twice the size
    CHECK(rgb(f, 350, 325) == 0xff0000);
    CHECK(rgb(f, 260, 325) == 0xff0000 && rgb(f, 440, 325) == 0xff0000);
    CHECK(rgb(f, 350, 280) == 0xff0000 && rgb(f, 350, 370) == 0xff0000 && rgb(f, 350, 385) != 0xff0000);
    CHECK(rgb(f, 50, 425) != 0xff0000); // not at its untransformed place
}

// A page still fetching many slow images must not hold up the next page: the document load used to queue behind
// them in the shared worker pool (seconds on a slow link or CPU). Also: the old page goes away off the UI thread and
// installing the new one does not lay it out a second time.
static void test_navigation_not_queued() {
    std::string html = "<!DOCTYPE html><html><body>";
    for (int i = 0; i < 40; i++) {
        std::string u = "https://slowimg.example/i" + std::to_string(i) + ".png";
        fake::site()[u] = {"", "image/png", "", 400};
        html += "<img src=\"" + u + "\">";
    }
    html += "</body></html>";
    fake::site()["https://slowimg.example/"] = {html, "text/html", "", 0};
    fake::site()["https://quick.example/"]   = {"<body style='background:#00ff00'>quick</body>", "text/html", "", 0};
    Settings st;
    st.doh_url = "";
    Browser b(st, [] {});
    b.set_viewport(W, H);
    b.navigate("https://slowimg.example/");
    for (int i = 0; i < 200 && b.loading(); i++) { b.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(!b.idle()); // its images are still on their way
    auto t0 = std::chrono::steady_clock::now();
    b.navigate("https://quick.example/");
    for (int i = 0; i < 400 && b.loading(); i++) { b.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!b.loading());
    CHECK(ms < 600); // (queued behind the images it took 40 x 400 ms / 5 workers, about 3 s)
    if (ms >= 600) std::printf("  next page took %.0f ms\n", ms);
    CHECK(rgb(shot(b), 400, 500) == 0x00ff00);
    Browser b2(st, [] {});
    b2.set_viewport(W, H);
    b2.navigate("https://quick.example/");
    settle(b2);
    CHECK(b2.layout_count() == 0); // laid out once, on the worker: installing it adds no second layout
}

int main() {
    build_site();
    build_site2();
    build_site3();
    build_site4();
    const char* only = std::getenv("QUILL_TEST"); // run one group: QUILL_TEST=frameset
    auto run = [&](const char* name, void (*fn)()) { if (!only || std::string(only) == name) fn(); };
    run("http", test_http_rule);
    run("modes", test_https_downgrade_and_modes);
    run("frameset", test_frameset);
    run("rows", test_rows_and_base_target);
    run("basehref", test_base_href);
    run("refresh", test_meta_refresh_and_redirect_links);
    run("tiles", test_tile_cache);
    run("css", test_parallel_stylesheets);
    run("images", test_late_images);
    run("hsts", test_hsts);
    run("iframes", test_iframes);
    run("windows", test_new_windows);
    run("details", test_details);
    run("maps", test_image_maps);
    run("fonts", test_web_fonts);
    run("favicons", test_favicons);
    run("listbox", test_listbox_and_upload);
    run("hover", test_hover_paint_only);
    run("sticky", test_sticky_and_fixed);
    run("grad", test_gradients_transforms);
    run("object", test_object_embed);
    run("marquee", test_marquee_blink);
    run("scroll", test_scrolling);
    run("history", test_history);
    run("headers", test_request_headers);
    run("fontdisplay", test_font_display);
    run("zoom", test_zoom);
    run("select", test_text_selection);
    run("auth", test_http_auth);
    run("queue", test_navigation_not_queued);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
