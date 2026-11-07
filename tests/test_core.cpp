// Unit tests for the parts of Quill that carry security or correctness weight.
// No framework: CHECK() prints failures and the exit code is the failure count.
#include "certpin.h"
#include "chrome.h"
#include "cookies.h"
#include "hsts.h"
#include "font.h"
#include "framebuffer.h"
#include "image.h"
#include "net.h"
#include "input.h"
#include "policy.h"
#include "settings.h"
#include "url.h"
#include "engine/document.h"
#include "diskcache.h"
#include "fontsan.h"
#include "forms.h"
#include "history.h"
#include "httpcache.h"

#include <filesystem>
#include <sstream>

#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace quill;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            g_failures++;                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        g_checks++;                                                                       \
        std::string _a = (a), _b = (b);                                                   \
        if (_a != _b) {                                                                   \
            g_failures++;                                                                 \
            std::printf("FAIL %s:%d: %s\n   got:      %s\n   expected: %s\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); \
        }                                                                                 \
    } while (0)

static bool nonpublic(const char* host) {
    unsigned char b[16];
    int           len = 0;
    if (!parse_ip_literal(host, b, len)) return false;
    return ip_is_non_public(b, len);
}

static void test_url_resolution() {
    // RFC 3986 section 5.4.1 normal examples
    const std::string base = "http://a/b/c/d;p?q";
    struct { const char* ref; const char* want; } cases[] = {
        {"g:h", "g:h"},
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g/"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"},
        {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        // abnormal examples
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g#s/../x", "http://a/b/c/g#s/../x"},
    };
    for (auto& c : cases) CHECK_EQ(resolve_url(base, c.ref), c.want);

    CHECK_EQ(resolve_url("https://example.com/dir/page.html", "img/a.png"), "https://example.com/dir/img/a.png");
    CHECK_EQ(resolve_url("https://example.com", "x.css"), "https://example.com/x.css");
    CHECK_EQ(resolve_url("https://example.com/a/b", "//cdn.example.net/x.js"), "https://cdn.example.net/x.js");
    CHECK_EQ(resolve_url("https://example.com/a/b", "data:image/png;base64,AAAA"), "data:image/png;base64,AAAA");
    CHECK_EQ(strip_fragment("http://x/y#z"), "http://x/y");

    ParsedUrl u;
    CHECK(parse_url("HTTPS://User:pw@Example.COM:8443/p/a?x=1#f", u));
    CHECK_EQ(u.scheme, "https");
    CHECK_EQ(u.host, "example.com");
    CHECK_EQ(u.userinfo, "User:pw");
    CHECK(u.port == 8443);
    CHECK_EQ(u.path, "/p/a");
    CHECK_EQ(u.query, "x=1");
    CHECK_EQ(u.fragment, "f");
    CHECK(parse_url("http://[::1]:8080/x", u));
    CHECK_EQ(u.host, "::1");
    CHECK(u.port == 8080);
    CHECK_EQ(serialize_url(u), "http://[::1]:8080/x");
    CHECK(!parse_url("http://host:99999/", u));
    CHECK(!parse_url("http://host:12ab/", u));
    CHECK(!parse_url("/relative/path", u));
    CHECK(!parse_url("http://[::1/", u));

    CHECK_EQ(percent_encode("hello world&x=ü"), "hello+world%26x%3D%C3%BC");
    CHECK_EQ(percent_decode("a%20b+c%zz", true), "a b c%zz");
    CHECK_EQ(query_param("a=1&u=http%3A%2F%2Fx.y%2Fz%3Fq%3D1&b=2", "u"), "http://x.y/z?q=1");
    CHECK_EQ(query_param("a=1", "missing"), "");
    CHECK_EQ(html_escape("<a href=\"x\">&'"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;");
}

static void test_ip_classification() {
    const char* priv[] = {"0.0.0.0", "10.1.2.3", "127.0.0.1", "127.255.255.254", "169.254.169.254", "172.16.0.1",
                          "172.31.255.255", "192.168.0.1", "100.64.0.1", "100.127.255.255", "224.0.0.1", "255.255.255.255",
                          "198.18.0.1", "192.0.0.5", "::", "::1", "fe80::1", "fd12:3456::1", "fc00::1", "ff02::1",
                          "::ffff:192.168.1.1", "::ffff:127.0.0.1", "2002:c0a8:0101::1", "64:ff9b::a00:1"};
    for (const char* h : priv) {
        CHECK(nonpublic(h));
        if (!nonpublic(h)) std::printf("   ^ expected non-public: %s\n", h);
    }
    const char* pub[] = {"8.8.8.8", "9.9.9.9", "1.1.1.1", "172.15.255.255", "172.32.0.1", "100.63.255.255", "100.128.0.1",
                         "192.169.0.1", "93.184.216.34", "2606:4700:4700::1111", "2001:4860:4860::8888", "::ffff:8.8.8.8"};
    for (const char* h : pub) {
        CHECK(!nonpublic(h));
        if (nonpublic(h)) std::printf("   ^ expected public: %s\n", h);
    }
    unsigned char b[16];
    int           len = 0;
    CHECK(!parse_ip_literal("example.com", b, len));
    CHECK(!parse_ip_literal("1.2.3", b, len));
    CHECK(!parse_ip_literal("0x7f.1", b, len));
    CHECK(!parse_ip_literal("2130706433", b, len)); // decimal-dword forms are not literals; the socket check catches them
}

static void test_policy() {
    Policy p;
    p.doh_url = "https://9.9.9.9/dns-query";
    p.private_allow.insert("app.example.org");
    p.private_allow.insert("*.corp.example.org");

    CHECK(p.is_private_host("router"));
    CHECK(p.is_private_host("nas.lan"));
    CHECK(p.is_private_host("printer.local"));
    CHECK(p.is_private_host("localhost"));
    CHECK(p.is_private_host("LOCALHOST."));
    CHECK(p.is_private_host("foo.localhost"));
    CHECK(p.is_private_host("192.168.1.1"));
    CHECK(p.is_private_host("::1"));
    CHECK(p.is_private_host("app.example.org"));
    CHECK(p.is_private_host("x.corp.example.org"));
    CHECK(!p.is_private_host("example.org"));
    CHECK(!p.is_private_host("evil.example.org"));
    CHECK(!p.is_private_host("example.com"));
    CHECK(!p.is_private_host("8.8.8.8"));
    CHECK(!p.is_private_host("duckduckgo.com"));
    CHECK(!p.is_private_host("notlocalhost.com"));
    CHECK(!p.is_private_host("fakelan.com"));

    auto ev = [&](const char* url, RequestKind k, const char* init, bool redirect) {
        ParsedUrl u;
        CHECK(parse_url(url, u));
        return evaluate_request(p, u, k, init, redirect);
    };

    Decision d = ev("https://example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Allow && d.use_doh && !d.allow_private_ip);

    d = ev("http://example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::WarnHttp);
    d = ev("http://example.com/img.png", RequestKind::Subresource, "https://example.com/", false);
    CHECK(d.verdict == Verdict::Block);
    // a plain-http page (already accepted by the user) may load plain-http images from other public hosts...
    d = ev("http://counter.example.net/hits.gif", RequestKind::Subresource, "http://oldsite.example.org/", false);
    CHECK(d.verdict == Verdict::Allow && d.use_doh);
    // ...and follow their http redirects, but never into the private network
    d = ev("http://cdn.example.net/a.gif", RequestKind::Subresource, "http://counter.example.net/hits.gif", true);
    CHECK(d.verdict == Verdict::Allow);
    d = ev("http://192.168.1.1/x.gif", RequestKind::Subresource, "http://oldsite.example.org/", false);
    CHECK(d.verdict == Verdict::Block);

    d = ev("http://router.lan/admin", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Allow && !d.use_doh && d.allow_private_ip);
    d = ev("http://192.168.1.1/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Allow && !d.use_doh && d.allow_private_ip);

    // public page must not reach into the private network (CSRF against routers)
    d = ev("http://192.168.1.1/reboot", RequestKind::Subresource, "https://evil.example.com/", false);
    CHECK(d.verdict == Verdict::Block);
    d = ev("http://localhost:8080/x", RequestKind::Subresource, "https://evil.example.com/", false);
    CHECK(d.verdict == Verdict::Block);
    // private page may load private subresources
    d = ev("http://192.168.1.1/style.css", RequestKind::Subresource, "http://192.168.1.1/", false);
    CHECK(d.verdict == Verdict::Allow);
    // redirect from public host to private address is blocked even for top-level navigation
    d = ev("http://192.168.1.1/reboot", RequestKind::TopLevel, "https://evil.example.com/", true);
    CHECK(d.verdict == Verdict::Block);
    // ...but the user typing/clicking a private URL is fine
    d = ev("http://192.168.1.1/", RequestKind::TopLevel, "https://evil.example.com/", false);
    CHECK(d.verdict == Verdict::Allow);
    // https->http downgrade via redirect on a public host asks the user
    d = ev("http://example.com/", RequestKind::TopLevel, "https://example.com/", true);
    CHECK(d.verdict == Verdict::WarnHttp);
    // http_warning = typed: following a link from a plain-http page (or an http -> http redirect) doesn't ask...
    {
        ParsedUrl u;
        CHECK(parse_url("http://other.example.org/page.html", u));
        CHECK(evaluate_request(p, u, RequestKind::TopLevel, "", false, "http://oldsite.example.org/").verdict == Verdict::Allow);
        CHECK(evaluate_request(p, u, RequestKind::TopLevel, "http://oldsite.example.org/", true).verdict == Verdict::Allow);
        // ...but a link from an https page (a downgrade) and a typed address still do
        CHECK(evaluate_request(p, u, RequestKind::TopLevel, "", false, "https://secure.example.org/").verdict == Verdict::WarnHttp);
        CHECK(evaluate_request(p, u, RequestKind::TopLevel, "", false).verdict == Verdict::WarnHttp);
        Policy always = p;
        always.http_warn = Policy::HttpWarn::Always;
        CHECK(evaluate_request(always, u, RequestKind::TopLevel, "", false, "http://oldsite.example.org/").verdict == Verdict::WarnHttp);
        Policy never = p;
        never.http_warn = Policy::HttpWarn::Never;
        CHECK(evaluate_request(never, u, RequestKind::TopLevel, "", false).verdict == Verdict::Allow);
        // the private-network rule still wins over all of it
        ParsedUrl lan;
        CHECK(parse_url("http://192.168.1.1/", lan));
        CHECK(evaluate_request(never, lan, RequestKind::TopLevel, "http://evil.example.org/", true).verdict == Verdict::Block);
    }

    d = ev("https://user:pw@example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Block);
    d = ev("ftp://example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Block);
    d = ev("file:///etc/passwd", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Block);
    d = ev("javascript:alert(1)", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Block);

    p.http_allow.insert("example.com");
    d = ev("http://example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Allow);

    p.doh_url.clear();
    d = ev("https://example.com/", RequestKind::TopLevel, "", false);
    CHECK(d.verdict == Verdict::Allow && !d.use_doh);
}

static void test_chrome_geometry() {
    // what the user clicks is what was drawn: tab and toolbar hit-testing agree with the layout
    for (int n : {1, 3, 12, 40}) {
        for (int W : {640, 1280}) {
            for (int i = 0; i < n; i++) {
                IRect r = ui::tab_rect(i, n, W);
                ui::TabHit h = ui::tab_hit(r.x + r.w / 2, r.y + r.h / 2, n, W, i);
                CHECK(h.index == i && !h.plus);
                ui::TabHit c = ui::tab_hit(r.right() - 16, r.y + r.h / 2, n, W, i); // the active tab always has a close button
                CHECK(c.index == i && c.close);
            }
        }
    }
    ui::ToolbarLayout l = ui::toolbar_layout(1000);
    CHECK(ui::toolbar_hit(l, l.back.x + 5, l.back.y + 5) == ui::Button::Back);
    CHECK(ui::toolbar_hit(l, l.home.x + 5, l.home.y + 5) == ui::Button::Home);
    CHECK(ui::toolbar_hit(l, l.identity.x + 5, l.identity.y + 5) == ui::Button::Identity);
    CHECK(ui::toolbar_hit(l, l.field.right() - 5, l.field.y + 5) == ui::Button::Field);
    CHECK(l.field.right() <= 1000 && l.back.y >= ui::kTabH && l.field.bottom() <= ui::kTop);
    for (const char* icon : {"back", "forward", "reload", "close", "add", "home", "shield-check", "lock", "lock-open", "key", "info", "warning", "search"})
        CHECK(ui::have_icon(icon));
    CHECK(ui::text_index_at("hello", 0) == 0 && ui::text_index_at("hello", 1000) == 5);
}

static void test_https_hardening() {
    // punycode (RFC 3492 / IDNA test vectors) and lookalike display
    CHECK_EQ(punycode_label("b\xC3\xBC" "cher"), "xn--bcher-kva");
    CHECK_EQ(punycode_label("m\xC3\xBCnchen"), "xn--mnchen-3ya");
    CHECK_EQ(punycode_label("\xE4\xBE\x8B\xE3\x81\x88"), "xn--r8jz45g");                 // 例え
    CHECK_EQ(punycode_label("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"), "xn--zckzah");  // テスト
    CHECK_EQ(display_host("\xD0\xB0pple.com"), "xn--pple-43d.com");                          // Cyrillic а
    CHECK_EQ(display_host("example.com"), "example.com");
    CHECK_EQ(display_url("https://\xD0\xB0pple.com/login?x=1"), "https://xn--pple-43d.com/login?x=1");
    CHECK_EQ(display_url("https://example.com/caf\xC3\xA9"), "https://example.com/caf\xC3\xA9"); // only hosts change

    // HSTS
    Hsts& h = Hsts::instance();
    h.clear_for_tests();
    ParsedUrl u;
    CHECK(parse_url("http://shop.example.com/cart", u));
    CHECK(!h.upgrade(u));
    h.note("shop.example.com", "max-age=31536000");
    CHECK(h.upgrade(u) && serialize_url(u) == "https://shop.example.com/cart");
    CHECK(!h.must_upgrade("cdn.shop.example.com"));            // no includeSubDomains
    h.note("bank.example.org", "max-age=600; includeSubDomains; preload");
    CHECK(h.must_upgrade("www.bank.example.org"));
    CHECK(!h.must_upgrade("example.org"));                     // never upwards
    h.note("bank.example.org", "max-age=0");                   // the site switches it off
    CHECK(!h.must_upgrade("www.bank.example.org"));
    h.note("10.0.0.1", "max-age=600");                         // not for IP addresses
    CHECK(!h.must_upgrade("10.0.0.1"));
    h.note("nomaxage.example.com", "includeSubDomains");       // max-age is required
    CHECK(!h.must_upgrade("nomaxage.example.com"));
    CHECK(h.must_upgrade("en.wikipedia.org") && h.must_upgrade("myproject.dev") && !h.must_upgrade("example.net"));
    CHECK(parse_url("http://example.app:80/", u) && h.upgrade(u) && serialize_url(u) == "https://example.app/");
    h.clear_for_tests();

    // Secure cookies can't be overwritten from plain http (RFC 6265bis "leave secure cookies alone")
    CookieJar& jar = CookieJar::instance();
    jar.clear();
    ParsedUrl s, p;
    CHECK(parse_url("https://site.example.com/", s) && parse_url("http://site.example.com/", p));
    jar.store(s, "sid=good; Secure; Path=/");
    jar.store(p, "sid=evil; Path=/");
    CHECK(jar.header_for(s) == "sid=good");
    jar.store(p, "sid=; Max-Age=0; Path=/"); // nor deleted
    CHECK(jar.header_for(s) == "sid=good");
    jar.clear();
}

static void test_unwrap_redirect() {
    CHECK_EQ(unwrap_redirect("https://duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.google.com%2F&rut=7bcd28c08acd"), "https://www.google.com/");
    CHECK_EQ(unwrap_redirect("https://html.duckduckgo.com/l/?uddg=http%3A%2F%2Fold.example%2Fa%3Fb%3Dc"), "http://old.example/a?b=c");
    CHECK_EQ(unwrap_redirect("https://www.google.com/url?q=https://example.org/x&sa=U"), "https://example.org/x");
    // only real web destinations, only the known redirectors
    CHECK_EQ(unwrap_redirect("https://duckduckgo.com/l/?uddg=javascript%3Aalert(1)"), "https://duckduckgo.com/l/?uddg=javascript%3Aalert(1)");
    CHECK_EQ(unwrap_redirect("https://notduckduckgo.com/l/?uddg=https%3A%2F%2Fevil.example%2F"), "https://notduckduckgo.com/l/?uddg=https%3A%2F%2Fevil.example%2F");
    CHECK_EQ(unwrap_redirect("https://duckduckgo.com/?q=test"), "https://duckduckgo.com/?q=test");
}

static void test_input() {
    Settings s;
    Policy   p;
    p.doh_url = s.doh_url;

    auto url = [&](const char* in) { return resolve_input(in, s, p).url; };
    CHECK_EQ(url("example.com"), "https://example.com");
    CHECK_EQ(url("  example.com/path?q=1  "), "https://example.com/path?q=1");
    CHECK_EQ(url("https://example.com/a b"), "https://example.com/a b");
    CHECK_EQ(url("http://example.com"), "http://example.com");
    CHECK_EQ(url("192.168.1.1"), "http://192.168.1.1");
    CHECK_EQ(url("router.lan"), "http://router.lan");
    CHECK_EQ(url("localhost:8080"), "http://localhost:8080");
    CHECK_EQ(url("localhost"), "http://localhost");
    CHECK_EQ(url("nas/"), "http://nas/");
    CHECK_EQ(url("[::1]:8080/x"), "http://[::1]:8080/x");
    CHECK_EQ(url("about:home"), "about:home");
    CHECK_EQ(url("javascript:alert(1)"), "javascript:alert(1)");
    CHECK_EQ(url("8.8.8.8"), "https://8.8.8.8");

    InputResult r = resolve_input("hello world", s, p);
    CHECK(r.is_search);
    CHECK_EQ(r.url, "https://html.duckduckgo.com/html/?q=hello+world");
    CHECK(resolve_input("dns", s, p).is_search);       // single word: search, not a host
    CHECK(resolve_input("3.14", s, p).is_search);       // not a host
    CHECK(resolve_input("what is 1.1.1.1 ?", s, p).is_search);
    CHECK_EQ(resolve_input("c++ templates", s, p).url, "https://html.duckduckgo.com/html/?q=c%2B%2B+templates");
    CHECK(resolve_input("   ", s, p).url.empty());

    s.search_url = "https://example.org/s?term=";
    CHECK_EQ(resolve_input("a b", s, p).url, "https://example.org/s?term=a+b");
}

static void test_settings() {
    const char* path = "quill_test_config.ini";
    {
        std::ofstream f(path);
        f << "# comment\nsearch_url = https://duckduckgo.com/?q=%s\n"
             "doh_url =\n"
             "http_allow = nas.example.com, Router.Lan.\n"
             "private_allow = *.home.example.net\n"
             "image_threads = 99\n"
             "bogus = 1\n";
    }
    std::string err;
    Settings    s = load_settings(path, &err);
    std::remove(path);
    CHECK_EQ(s.search_url, "https://duckduckgo.com/?q=%s");
    CHECK(s.doh_url.empty());
    CHECK(s.http_allow.count("nas.example.com") == 1);
    CHECK(s.http_allow.count("router.lan") == 1);
    CHECK(s.private_allow.count("*.home.example.net") == 1);
    CHECK(s.image_threads == 16);
    CHECK(err.find("bogus") != std::string::npos);

    std::string err2;
    Settings    d = load_settings("/nonexistent/quill.ini", &err2);
    CHECK(err2.empty());
    CHECK_EQ(d.search_url, "https://html.duckduckgo.com/html/?q=%s");
}

static void test_certpin() {
    CHECK_EQ(hex_colon(sha256_bytes("abc")).substr(0, 11), "BA:78:16:BF");
    CHECK_EQ(hex_colon(sha256_bytes("")).substr(0, 11), "E3:B0:C4:42");
    // 56 bytes: the padding spills into a second block
    CHECK_EQ(hex_colon(sha256_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).substr(0, 11), "24:8D:6A:61");
    std::string raw;
    CHECK(base64_decode("aGVsbG8gd29ybGQ=", raw) && raw == "hello world");
    CHECK_EQ(base64_encode("hello world"), "aGVsbG8gd29ybGQ=");
    CHECK_EQ(base64_encode("ab"), "YWI=");
    CHECK_EQ(base64_encode("a"), "YQ==");

    const std::string ec =
        "-----BEGIN CERTIFICATE-----\n"
        "MIIBnDCCAUKgAwIBAgIUTY+hNGrW8AZXkgBhCE+WOFgAnnIwCgYIKoZIzj0EAwIw\n"
        "FTETMBEGA1UEAwwKY3JhZnR5LmxhbjAeFw0yNjEwMjkwNDU4NTRaFw0zNjEwMjYw\n"
        "NDU4NTRaMBUxEzARBgNVBAMMCmNyYWZ0eS5sYW4wWTATBgcqhkjOPQIBBggqhkjO\n"
        "PQMBBwNCAASHVVhXsOJ5Baj3ybWKFk2rbdY5y8C2kY7HBp/yRuOM4lTLJjWDIj/h\n"
        "sTwNv+weD6rk41ynvxmlErwrJJau4w0to3AwbjAdBgNVHQ4EFgQUi3fWCXB7SlDk\n"
        "0k1RWFS7zKt5wacwHwYDVR0jBBgwFoAUi3fWCXB7SlDk0k1RWFS7zKt5wacwDwYD\n"
        "VR0TAQH/BAUwAwEB/zAbBgNVHREEFDASggpjcmFmdHkubGFuhwR/AAABMAoGCCqG\n"
        "SM49BAMCA0gAMEUCIB10XhDoTh+ASa1jv+lnVjF5lD4Z5czmwdNCMcWQf8UpAiEA\n"
        "8whHgZ0huKDNMGNN7hp+/hVWlCyMmhoi2FUuC/0R1e0=\n"
        "-----END CERTIFICATE-----\n";
    const std::string rsa =
        "-----BEGIN CERTIFICATE-----\n"
        "MIIDCzCCAfOgAwIBAgIUVtjB4OYozcN8yQfFlk427qde4MIwDQYJKoZIhvcNAQEL\n"
        "BQAwFTETMBEGA1UEAwwKcm91dGVyLmxhbjAeFw0yNjEwMjkwNDU4NTVaFw0zNjEw\n"
        "MjYwNDU4NTVaMBUxEzARBgNVBAMMCnJvdXRlci5sYW4wggEiMA0GCSqGSIb3DQEB\n"
        "AQUAA4IBDwAwggEKAoIBAQDFOTaufIRo+qh+x+mMqU+rZSWwsxd9GSSy6/2X1nAo\n"
        "flQ4Dy5mty3MNN/+4LVuk3pXZMWB2rydDyMHszd2p/TIwe9573xKX19EjjVYFHxR\n"
        "qRvzXbXu5FW441l9F3UgK/BI3dGPeDTfKGJJcOv1ykdn+EyrEUep6n8KIoaW9qtV\n"
        "CCq8Sxz/JtTTB7x+gzN4sjzBoXyHednO6kFl0pu+04JF5I9ylfdilpp9I6e921ts\n"
        "CoVWry8IZRtep4IGEfyZ/6NxrLWX/Nm18b6d+DJaiTzTti3/orrMjd9TJfppnVwH\n"
        "jRruDsuCKLS1sGzqcn1as3Zhahw/iDLY2UJJ4bDVtdRpAgMBAAGjUzBRMB0GA1Ud\n"
        "DgQWBBTA7zOuyXw1a7Eajnxn4Jk0+pdW6TAfBgNVHSMEGDAWgBTA7zOuyXw1a7Ea\n"
        "jnxn4Jk0+pdW6TAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQCj\n"
        "DU5BYS9ZxGzrn3aDpj0z6rF5bs8wD3R+HpQa9fWnOG+7uskqqlthqpFh/1I5JtuJ\n"
        "rkS1XbFg083kw6inY2IgHbwYfdamzdtC4z7XkbM4fYkSontEE9IeniMfOYF2AMiE\n"
        "sHp8+ufG1RPc2Vjp4FkqNLblvyORz0xwNToc2imWfkwRQfkNjEdUAtPAgGobP7AY\n"
        "x1C6j1r0IufFC2Y3XzzIYopqB/XiDWp3pkZqw0f1smfssLHv+vA888xb8hVdQ7F/\n"
        "igwCPLzw06tofBuPHowJ/rQ0GzFSQ5Igv+tDqishp1au0wFY4m7Q4F3ybaAy5dCe\n"
        "6g2kB0rSBvS3JQYLU37l\n"
        "-----END CERTIFICATE-----\n";
    CHECK_EQ(spki_pin_from_pem(ec), "sha256//RXb7xKTR6ilriXbLog38NYLZ75ZO4UHwDHZvKh/V8V8=");  // from openssl
    CHECK_EQ(spki_pin_from_pem(rsa), "sha256//H27kbxT2+O7nL+msInveeM1pHmCyJmifUi3oe/TAdlA="); // from openssl
    CHECK_EQ(spki_pin_from_pem("not a certificate"), "");
    CHECK_EQ(spki_pin_from_pem("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----"), "");
    CHECK(is_valid_pin("sha256//RXb7xKTR6ilriXbLog38NYLZ75ZO4UHwDHZvKh/V8V8="));
    CHECK(!is_valid_pin("sha256//short"));
    CHECK(!is_valid_pin("md5//RXb7xKTR6ilriXbLog38NYLZ75ZO4UHwDHZvKh/V8V8="));
}

static void test_cookies() {
    CHECK_EQ(registrable_domain("www.example.com"), "example.com");
    CHECK_EQ(registrable_domain("a.b.example.co.uk"), "example.co.uk");
    CHECK_EQ(registrable_domain("example.com"), "example.com");
    CHECK_EQ(registrable_domain("localhost"), "localhost");
    CHECK_EQ(registrable_domain("192.168.1.1"), "192.168.1.1");
    CHECK(same_site("www.example.com", "cdn.example.com"));
    CHECK(!same_site("example.com", "example.org"));
    CHECK(!same_site("a.co.uk", "b.co.uk"));

    CookieJar& j = CookieJar::instance();
    j.clear();
    ParsedUrl u;
    CHECK(parse_url("https://www.example.com/app/login", u));
    j.store(u, "sid=abc123; Path=/; HttpOnly; Secure");
    j.store(u, "theme=dark; Domain=example.com; Max-Age=3600");
    j.store(u, "narrow=1; Path=/app/admin");
    ParsedUrl a, b, c, d;
    CHECK(parse_url("https://www.example.com/app/x", a));
    CHECK(parse_url("http://www.example.com/app/x", b));
    CHECK(parse_url("https://cdn.example.com/app/x", c));
    CHECK(parse_url("https://example.org/", d));
    CHECK_EQ(j.header_for(a), "theme=dark; sid=abc123");       // longer path first (RFC 6265); narrow path not matched
    CHECK_EQ(j.header_for(b), "theme=dark");                    // Secure cookie not sent over http
    CHECK_EQ(j.header_for(c), "theme=dark");                    // host-only cookie not sent to a sibling host
    CHECK_EQ(j.header_for(d), "");
    j.store(u, "theme=; Domain=example.com; Max-Age=0");        // deletion
    CHECK_EQ(j.header_for(a), "sid=abc123");
    j.store(u, "bad=1; Domain=example.org");                    // not a parent of the host
    j.store(u, "bad2=1; Domain=com");                           // TLD-wide
    j.store(u, "bad3=1; Domain=co.uk");
    CHECK_EQ(j.header_for(a), "sid=abc123");
    ParsedUrl h;
    CHECK(parse_url("http://nas.lan/", h));
    j.store(h, "x=1; Secure");                                  // Secure cookie from http is refused
    CHECK_EQ(j.header_for(h), "");
    j.store(h, "s=2; Expires=Wed, 21 Oct 2015 07:28:00 GMT");   // already expired
    CHECK_EQ(j.header_for(h), "");
    j.store(h, "s=3; Expires=Wed, 21 Oct 2099 07:28:00 GMT");
    CHECK_EQ(j.header_for(h), "s=3");
    j.clear();
}

static void test_svg_and_webp() {
    auto dec = [](const std::string& s) { return decode_image(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
    const std::string svg = "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"10\">"
                            "<rect width=\"20\" height=\"10\" fill=\"#ff0000\"/></svg>";
    auto        a         = dec(svg);
    CHECK(a != nullptr);
    if (a) {
        CHECK(a->w == 20 && a->h == 10);
        CHECK(a->svg != nullptr);
        CHECK(a->px[5 * 20 + 10] == 0xFFFF0000u);
        const Image& big = a->scaled(80, 40); // re-rendered from the vector source, still solid red in the middle
        CHECK(big.w == 80 && big.h == 40);
        CHECK(big.px[20 * 80 + 40] == 0xFFFF0000u);
    }
    CHECK(dec("\xEF\xBB\xBF  <svg viewBox=\"0 0 4 4\" xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"4\" height=\"4\"/></svg>") != nullptr);
    CHECK(dec("<html><body><svg width=\"5\" height=\"5\"></svg></body></html>") == nullptr); // an HTML page is not an image
    CHECK(dec("<svg") == nullptr);
    CHECK(dec("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"0\"></svg>") == nullptr);

    static const unsigned char webp[] = {0x52, 0x49, 0x46, 0x46, 0x1c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
                                         0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x03, 0xc0, 0x00,
                                         0x00, 0x07, 0x10, 0xfd, 0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00};
    auto w = decode_image(webp, sizeof(webp));
    if (webp_supported()) {
        CHECK(w != nullptr);
        if (w) {
            CHECK(w->w == 4 && w->h == 4);
            CHECK(w->px[0] == 0xFFFF0000u);
        }
        CHECK(decode_image(webp, 20) == nullptr); // truncated
    } else {
        CHECK(w == nullptr);
    }
}

static void test_images_and_framebuffer() {
    std::string mime, bytes;
    CHECK(decode_data_uri("data:image/png;base64,"
                          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==",
                          mime, bytes));
    CHECK_EQ(mime, "image/png");
    auto img = decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    CHECK(img != nullptr);
    if (img) CHECK(img->w == 1 && img->h == 1);
    CHECK(decode_image(reinterpret_cast<const unsigned char*>("not an image"), 12) == nullptr);
    CHECK(decode_data_uri("data:text/plain,hello%20there", mime, bytes) && bytes == "hello there");

    // animated GIF: 2x1, frame 0 red with a 0 ms delay (played at 100 ms like every browser), frame 1 blue for 250 ms
    CHECK(decode_data_uri("data:image/gif;base64,R0lGODlhAgABAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQEAAAAACwAAAAAAgABAAAIBQ"
                          "ABAAgIACH5BAQZAAAALAAAAAACAAEAgQAA/wAAAAAAAAAAAAgFAAEACAgAOw==",
                          mime, bytes));
    auto gif = decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    CHECK(gif != nullptr);
    if (gif) {
        CHECK(gif->w == 2 && gif->h == 1);
        CHECK(gif->animated() && gif->frames.size() == 1);
        CHECK(gif->total_ms == 350);
        int next = 0;
        CHECK(gif->frame_at(0, &next).px[0] == 0xFFFF0000u && next == 100);
        CHECK(gif->frame_at(120, &next).px[0] == 0xFF0000FFu && next == 230);
        CHECK(gif->frame_at(350 + 50).px[0] == 0xFFFF0000u); // loops
        CHECK(decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), 40) == nullptr ||
              !decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), 40)->animated()); // truncated
    }

    Image two;
    two.w  = 2;
    two.h  = 2;
    two.px = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    const Image& one = two.scaled(1, 1);
    CHECK(one.w == 1 && one.h == 1);
    CHECK((one.px[0] >> 24) == 0xFF);
    const Image& big = two.scaled(4, 4);
    CHECK(big.w == 4 && big.h == 4);

    Framebuffer fb;
    fb.resize(10, 10);
    fb.clear(Color(255, 255, 255));
    fb.push_clip(IRect(2, 2, 4, 4));
    fb.fill_rect(IRect(0, 0, 10, 10), Color(255, 0, 0));
    fb.pop_clip();
    CHECK(fb.px[0] == 0xFFFFFFFFu);                  // outside clip untouched
    CHECK(fb.px[3 * 10 + 3] == 0xFFFF0000u);         // inside clip painted
    CHECK(fb.px[6 * 10 + 6] == 0xFFFFFFFFu);
    fb.fill_rect(IRect(0, 0, 10, 10), Color(0, 0, 0, 128)); // alpha blend
    uint32_t p = fb.px[0];
    CHECK(((p >> 16) & 255) > 100 && ((p >> 16) & 255) < 140);
    fb.fill_rect(IRect(-5, -5, 100, 100), Color(1, 2, 3)); // must not crash on out-of-range rects
}

static void test_form_policy() {
    Settings s;
    s.doh_url.clear();
    Network n(s, nullptr);
    FetchRequest r;
    r.url         = "http://192.168.1.1/login";
    r.kind        = RequestKind::TopLevel;
    r.method      = "POST";
    r.body        = "a=b";
    r.content_type = "application/x-www-form-urlencoded";
    r.initiator   = "https://evil.example.com/";
    r.form_submit = true;
    FetchResult res = n.fetch(r);
    CHECK(res.blocked);
    CHECK(!res.ok);
}

static void test_fonts() {
    FontManager& fm = FontManager::instance();
    if (!fm.have_fonts()) {
        std::printf("note: no system fonts found, skipping font tests\n");
        return;
    }
    Font* f = fm.get("Helvetica, Arial, sans-serif", 16, 400, false);
    CHECK(f != nullptr);
    float w1 = f->text_width("Hello"), w2 = f->text_width("Hello, world");
    CHECK(w1 > 10 && w2 > w1);
    CHECK(f->ascent > 0 && f->line_height >= f->ascent + f->descent);
    CHECK(f->text_width("") == 0);
    CHECK(fm.get("sans-serif", 16, 400, false) == f);           // cached
    CHECK(fm.get("monospace", 16, 400, false) != f);
    Font* big = fm.get("serif", 32, 700, true);
    CHECK(big->text_width("Hi") > f->text_width("Hi"));

    Framebuffer fb;
    fb.resize(80, 30);
    fb.clear(Color(255, 255, 255));
    f->draw(fb, "Hello", 4, 20, Color(0, 0, 0));
    int dark = 0;
    for (uint32_t px : fb.px)
        if ((px & 255) < 128) dark++;
    CHECK(dark > 20);
    f->draw(fb, "\xFF\xFE bad utf8 \xC3", 4, 20, Color(0, 0, 0)); // invalid UTF-8 must not crash
}

// srcset / sizes / <picture>: which URL each <img> box ends up with, and how big a dense picture is drawn.
static const engine::Box* find_id(const engine::Box& b, const std::string& id) {
    if (const std::string* v = b.attr("id"); v && *v == id) return &b;
    for (auto& c : b.children)
        if (const engine::Box* r = find_id(*c, id)) return r;
    for (auto& l : b.lines)
        for (auto& f : l.frags)
            if (f.box && f.box->parent != &b)
                if (const engine::Box* r = find_id(*f.box, id)) return r;
    return nullptr;
}
static void test_responsive_images() {
    engine::Document d;
    d.parse_html("<html><body style='margin:0'>"
                 "<img id=a src=fallback.png srcset='big.png 2x, small.png 1x'>"
                 "<img id=b src=fallback.png srcset='hi.png 2x'>"
                 "<img id=c srcset='w100.png 100w, w400.png 400w, w800.png 800w' sizes='(max-width: 500px) 100vw, 300px'>"
                 "<img id=d srcset='w100.png 100w, w400.png 400w' sizes='50px'>"
                 "<img id=e srcset='data:image/gif;base64,R0lG,ODdh 1x, other.png 2x'>"
                 "<picture><source media='(min-width: 2000px)' srcset=huge.png><source type=image/avif srcset=x.avif>"
                 "<source srcset='pic1.png 1x, pic2.png 2x'><img id=f src=pfall.png></picture>"
                 "<picture><source media='(min-width: 2000px)' srcset=huge.png><img id=g src=pfall.png></picture>"
                 "<img id=h srcset='w100.png 100w, w400.png 400w' sizes='calc(100vw - 900px)'>"
                 "</body></html>");
    d.resolve_styles(1000, 800);
    d.set_image_sizer([](const std::string&, int& w, int& h) { w = 400; h = 200; return true; });
    d.layout(1000, 800);
    auto src = [&](const char* id) {
        const engine::Box* b = d.box_root() ? find_id(*d.box_root(), id) : nullptr;
        const std::string* v = b ? b->attr("src") : nullptr;
        return v ? *v : std::string("(none)");
    };
    auto width = [&](const char* id) {
        const engine::Box* b = d.box_root() ? find_id(*d.box_root(), id) : nullptr;
        return b ? b->w : -1.f;
    };
    CHECK_EQ(src("a"), "small.png");
    CHECK_EQ(src("b"), "fallback.png");   // src counts as the 1x candidate
    CHECK_EQ(src("c"), "w400.png");       // 1000px wide viewport -> the 300px slot -> smallest >= 300w
    CHECK(std::abs(width("c") - 300) < 0.5f); // a 400w picture in a 300px slot is drawn 300px wide
    CHECK_EQ(src("d"), "w100.png");
    CHECK_EQ(src("e"), "data:image/gif;base64,R0lG,ODdh"); // commas inside a data: URL
    CHECK_EQ(src("f"), "pic1.png");       // media and type skip the first two <source>s
    CHECK_EQ(src("g"), "pfall.png");      // no <source> applies: the <img> itself
    CHECK_EQ(src("h"), "w100.png");       // calc(100vw - 900px) = 100px
    d.resolve_styles(400, 800);           // a narrow window: the (max-width: 500px) branch, 100vw = 400px
    d.layout(400, 800);
    CHECK_EQ(src("c"), "w400.png");
    d.resolve_styles(300, 800);
    d.layout(300, 800);
    CHECK_EQ(src("c"), "w400.png");       // 300px slot still needs the 400w one
    d.resolve_styles(90, 800);
    d.layout(90, 800);
    CHECK_EQ(src("c"), "w100.png");
}

#ifndef QUILL_SOURCE_DIR
#define QUILL_SOURCE_DIR "."
#endif
static std::string slurp(const std::string& rel) {
    std::ifstream f(std::string(QUILL_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_font_sanitizer() {
    for (const char* name : {"tests/fonts/quill-test-icons.ttf", "tests/fonts/quill-test-icons.woff", "tests/fonts/quill-test-icons.woff2"}) {
        std::string   in = slurp(name);
        SanitizedFont sf;
        std::string   err;
        bool ok = sanitize_font(reinterpret_cast<const unsigned char*>(in.data()), in.size(), sf, &err);
        bool want = std::string(name).find("woff2") == std::string::npos || woff2_supported();
        CHECK(ok == want);
        if (!ok) continue;
        CHECK(sf.mapped >= 5);       // Q, W, the square, and the two private-use code points
        CHECK(sf.dropped_glyphs == 0);
        // the rebuilt font sanitizes to itself (what the disk cache relies on)
        SanitizedFont again;
        CHECK(sanitize_font(sf.data.data(), sf.data.size(), again) && again.data == sf.data);
        // every way of cutting it short is refused or still safe
        for (size_t cut : {size_t(12), size_t(50), sf.data.size() / 2, sf.data.size() - 1}) {
            SanitizedFont t;
            sanitize_font(sf.data.data(), cut, t); // must not crash (ASan builds check)
        }
    }
    SanitizedFont sf;
    std::string   err;
    const unsigned char otto[] = {'O', 'T', 'T', 'O', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!sanitize_font(otto, sizeof otto, sf, &err) && err.find("CFF") != std::string::npos);
    const unsigned char junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    CHECK(!sanitize_font(junk, sizeof junk, sf, &err));
    std::string woff = slurp("tests/fonts/quill-test-icons.woff");
    if (woff.size() > 60) { // a table that claims to inflate to more than it does
        woff[44 + 12] = '\x7f';
        CHECK(!sanitize_font(reinterpret_cast<const unsigned char*>(woff.data()), woff.size(), sf));
    }
}

static void test_disk_cache() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("quill-cache-test-" + std::to_string(std::rand()));
    DiskCache& c = DiskCache::instance();
    c.configure(dir.string(), true);
    std::string got;
    CHECK(!c.get("fonts", "https://a/x.woff2", got, 1000));
    c.put("fonts", "https://a/x.woff2", "hello", 100);
    CHECK(c.get("fonts", "https://a/x.woff2", got, 1000) && got == "hello");
    CHECK(!c.get("icons", "https://a/x.woff2", got, 1000)); // buckets are separate
    CHECK(!c.get("../etc", "https://a/x.woff2", got, 1000)); // no path tricks in bucket names
    c.put("fonts", "https://a/x.woff2", "replaced", 100);
    CHECK(c.get("fonts", "https://a/x.woff2", got, 1000) && got == "replaced");
    CHECK(!c.get("fonts", "https://a/x.woff2", got, -1));   // too old: dropped
    CHECK(!c.get("fonts", "https://a/x.woff2", got, 1000));
    for (int i = 0; i < 10; i++) c.put("icons", "k" + std::to_string(i), std::string(30, 'x'), 100); // cap 100 bytes
    uintmax_t total = 0;
    for (auto& e : fs::directory_iterator(dir / "icons")) total += e.file_size();
    CHECK(total <= 100);
    CHECK(c.get("icons", "k9", got, 1000)); // the newest stays
    for (auto& e : fs::directory_iterator(dir / "icons")) CHECK(e.path().filename().string().size() == 64); // hashed names
    c.configure(dir.string(), false);
    CHECK(!c.get("icons", "k9", got, 1000));
    c.configure("", false);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_forms_encoding() {
    using namespace quill;
    FormFields f = {{"a b", "x&y=z"}, {"t", "l1\nl2"}};
    CHECK_EQ(form_urlencode(f), std::string("a+b=x%26y%3Dz&t=l1%0D%0Al2"));
    std::string body, ct;
    std::vector<FormFile> none = {{"up", "", 1}};
    CHECK(form_multipart({{"na\"me", "v\n"}, {"k", "w"}}, none, body, ct, nullptr, "seed"));
    std::string bd = ct.substr(ct.find('=') + 1);
    CHECK(ct.rfind("multipart/form-data; boundary=----QuillFormBoundary", 0) == 0);
    CHECK_EQ(body, "--" + bd + "\r\nContent-Disposition: form-data; name=\"na%22me\"\r\n\r\nv\r\n\r\n--" + bd +
                       "\r\nContent-Disposition: form-data; name=\"up\"; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n--" + bd +
                       "\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nw\r\n--" + bd + "--\r\n");
    std::string err;
    std::vector<FormFile> missing = {{"f", "/nonexistent/quill/file.bin", 0}};
    CHECK(!form_multipart({}, missing, body, ct, &err));
    CHECK(!err.empty());
    CHECK_EQ(mime_for_filename("Photo.JPG"), std::string("image/jpeg"));
    CHECK_EQ(mime_for_filename("dir.v2/README"), std::string("application/octet-stream"));
}

static void test_history_store() {
    using namespace quill;
    std::string path = (std::filesystem::temp_directory_path() / "quill-core-history.tsv").string();
    std::time_t now  = std::time(nullptr);
    {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        o << (now - 200 * 86400) << "\thttps://old.example/\tOld\n";          // too old: dropped
        o << (now - 3600) << "\thttps://a.example/\tA page\n";
        o << "garbage line\n";                                              // dropped
        o << (now - 60) << "\tjavascript:alert(1)\tnope\n";                  // not a web page: dropped
    }
    History& h = History::instance();
    h.configure(true, path, 90);
    CHECK(h.size() == 1);
    h.add("https://b.example/x", "Tab\tand\nnewline");
    h.add("quill:clear-history", "x"); // never recorded
    h.add("about:home", "x");
    CHECK(h.size() == 2);
    auto l = h.list();
    CHECK(l.size() == 2 && l[0].url == "https://b.example/x" && l[0].title == "Tab and newline");
    h.add("https://a.example/", "A page", now - 1800); // same day again: listed once
    CHECK(h.list("a.example").size() == 1);
    CHECK(h.list("TAB AND").size() == 1);
    h.configure(true, path, 90); // what reached the file comes back
    CHECK(h.size() == 3);
    h.clear();
    h.configure(true, path, 90);
    CHECK(h.size() == 0);
    h.configure(false, "", 90);
    std::filesystem::remove(path);
}

static void test_referrer() {
    using namespace quill;
    const std::string pg = "https://user:pw@a.example:8443/p/q?x=1#f";
    CHECK_EQ(referrer_value(pg, "https://a.example:8443/z", "", "strict"), std::string("https://a.example:8443/p/q?x=1"));
    CHECK_EQ(referrer_value(pg, "https://b.example/", "", "strict"), std::string("https://a.example:8443/"));
    CHECK_EQ(referrer_value(pg, "http://b.example/", "", "strict"), std::string()); // https -> http: nothing
    CHECK_EQ(referrer_value(pg, "http://b.example/", "unsafe-url", "strict"), std::string()); // capped
    CHECK_EQ(referrer_value(pg, "http://b.example/", "unsafe-url", "full"), std::string("https://a.example:8443/p/q?x=1"));
    CHECK_EQ(referrer_value(pg, "https://a.example:8443/z", "origin", "strict"), std::string("https://a.example:8443/"));
    CHECK_EQ(referrer_value(pg, "https://b.example/", "same-origin", "strict"), std::string());
    CHECK_EQ(referrer_value(pg, "https://a.example:8443/z", "", "origin"), std::string("https://a.example:8443/"));
    CHECK_EQ(referrer_value(pg, "https://a.example:8443/z", "", "off"), std::string());
    CHECK_EQ(referrer_value("about:history", "https://b.example/", "", "strict"), std::string());
    CHECK_EQ(referrer_value("http://a.example/x", "http://a.example/y", "", "strict"), std::string("http://a.example/x"));
    CHECK_EQ(referrer_value("https://a.example/x", "https://a.example:443/y", "", "strict"), std::string("https://a.example/x")); // same origin
    CHECK_EQ(parse_referrer_policy("no-referrer, bogus"), std::string("no-referrer"));
    CHECK_EQ(parse_referrer_policy("origin, strict-origin"), std::string("strict-origin"));
    CHECK_EQ(parse_referrer_policy("Never"), std::string("no-referrer"));
    CHECK_EQ(parse_referrer_policy("whatever"), std::string());
}

static void test_http_cache() {
    using namespace quill;
    const std::time_t now = 1700000000; // 2023-11-14 22:13:20 UTC
    CacheHeaders h;
    h.cache_control = "public, max-age=600";
    CHECK(HttpCache::freshness(h, now) == now + 600);
    h.age = "100";
    CHECK(HttpCache::freshness(h, now) == now + 500);
    h = CacheHeaders();
    h.cache_control = "no-cache, max-age=600";
    CHECK(HttpCache::freshness(h, now) == 0);
    h = CacheHeaders();
    h.date    = "Tue, 14 Nov 2023 22:13:20 GMT";
    h.expires = "Tue, 14 Nov 2023 23:13:20 GMT";
    CHECK(HttpCache::freshness(h, now) == now + 3600);
    h.expires = "0"; // already expired
    CHECK(HttpCache::freshness(h, now) == 0);
    h = CacheHeaders();
    h.date          = "Tuesday, 14-Nov-23 22:13:20 GMT"; // RFC 850
    h.last_modified = "Tue Nov  4 22:13:20 2023";        // asctime: 10 days unchanged -> fresh a day (the cap)
    CHECK(HttpCache::freshness(h, now) == now + 86400);
    h.last_modified = "Tue, 14 Nov 2023 12:13:20 GMT"; // 10 h -> 1 h
    CHECK(HttpCache::freshness(h, now) == now + 3600);
    CacheHeaders ns;
    ns.cache_control = "private, no-store";
    CHECK(!HttpCache::storable(200, ns));
    CHECK(!HttpCache::storable(404, CacheHeaders()));
    CacheHeaders vy;
    vy.vary = "Accept-Encoding, User-Agent";
    CHECK(HttpCache::storable(200, vy));
    vy.vary = "Cookie";
    CHECK(!HttpCache::storable(200, vy));

    HttpCache& c = HttpCache::instance();
    c.clear();
    c.set_capacity(1000);
    auto entry = [&](const std::string& body, std::time_t fresh, const std::string& etag) {
        CachedResponse r;
        r.url = "https://cdn.example/a.css";
        r.content_type = "text/css";
        r.body = body;
        r.etag = etag;
        r.stored = now;
        r.fresh_until = fresh;
        return r;
    };
    std::string ka = HttpCache::key("a.example", "https://cdn.example/a.css#x"), kb = HttpCache::key("b.example", "https://cdn.example/a.css");
    CHECK(ka != kb); // partitioned by site
    c.store(ka, entry("body{}", now + 60, "\"v1\""));
    CachedResponse got;
    CHECK(c.lookup(ka, now, got) == HttpCache::Look::Fresh && got.body == "body{}");
    CHECK(c.lookup(kb, now, got) == HttpCache::Look::Miss); // another site doesn't see it
    CHECK(c.lookup(HttpCache::key("a.example", "https://cdn.example/a.css"), now, got) == HttpCache::Look::Fresh); // #fragment ignored
    CHECK(c.lookup(ka, now + 61, got) == HttpCache::Look::Stale && got.etag == "\"v1\"");
    CacheHeaders h304;
    h304.cache_control = "max-age=120";
    CHECK(c.refresh(ka, h304, now + 61, got) && got.body == "body{}");
    CHECK(c.lookup(ka, now + 100, got) == HttpCache::Look::Fresh);
    c.store(kb, entry("no validators, not fresh", 0, "")); // useless: not kept
    CHECK(c.lookup(kb, now, got) == HttpCache::Look::Miss);
    c.store(HttpCache::key("x", "1"), entry(std::string(100, 'a'), now + 60, ""));
    c.store(HttpCache::key("x", "2"), entry(std::string(100, 'b'), now + 60, ""));
    c.lookup(ka, now, got); // ka most recently used
    for (int i = 3; i < 12; i++) c.store(HttpCache::key("x", std::to_string(i)), entry(std::string(100, 'c'), now + 60, ""));
    CHECK(c.bytes() <= 1000);
    CHECK(c.lookup(HttpCache::key("x", "1"), now, got) == HttpCache::Look::Miss); // the oldest went first
    c.store(HttpCache::key("x", "big"), entry(std::string(200, 'd'), now + 60, "")); // over 1/8 of the cache: not kept
    CHECK(c.lookup(HttpCache::key("x", "big"), now, got) == HttpCache::Look::Miss);
    c.clear();
    c.set_capacity(64u << 20);
}

int main() {
    test_forms_encoding();
    test_http_cache();
    test_referrer();
    test_history_store();
    test_url_resolution();
    test_ip_classification();
    test_policy();
    test_input();
    test_settings();
    test_unwrap_redirect();
    test_https_hardening();
    test_chrome_geometry();
    test_images_and_framebuffer();
    test_svg_and_webp();
    test_certpin();
    test_cookies();
    test_form_policy();
    test_fonts();
    test_responsive_images();
    test_font_sanitizer();
    test_disk_cache();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
