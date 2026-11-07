#include "browser.h"

#include "font.h"
#include "diskcache.h"
#include "fontsan.h"
#include "image.h"
#include "history.h"
#include "hsts.h"
#include "input.h"
#include "url.h"

#include "engine/paint.h"

#include <algorithm>
#include <ctime>
#include <thread>
#include <chrono>
#include <future>
#include <map>
#include <sstream>
#include <cmath>
#include <cstring>

namespace quill {

namespace {

const char* kCss =
    "body{font-family:sans-serif;margin:0;padding:48px 40px;color:#222;background:#fff;}"
    ".box{max-width:620px;}"
    "h1{font-size:26px;margin:0 0 14px 0;color:#1a1a1a;}"
    "p{line-height:1.5;margin:10px 0;}"
    ".err{color:#b00020;}"
    ".muted{color:#666;font-size:13px;}"
    ".url{font-family:monospace;background:#f2f2f2;padding:2px 6px;}"
    "a{color:#0b57d0;}"
    "table{border-collapse:collapse;margin-top:12px;}"
    "td{padding:3px 14px 3px 0;font-size:13px;vertical-align:top;}"
    ".btn{display:inline-block;margin:10px 14px 0 0;padding:8px 14px;border:1px solid #999;}";

std::string page_html(const std::string& title, const std::string& body) {
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" + html_escape(title) + "</title><style>" + kCss +
           "</style></head><body><div class=\"box\">" + body + "</div></body></html>";
}

// about:history (Ctrl+H): the pages visited, newest first, grouped by day, with a search box and a way to clear it.
std::string history_html(const std::string& query) {
    History&    h = History::instance();
    std::string rows;
    std::vector<HistoryEntry> list = h.list(query, 1000);
    std::time_t now = std::time(nullptr);
    auto        local = [](std::time_t t) {
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        return tmv;
    };
    std::tm     today = local(now), yest = local(now - 86400);
    std::string last_day;
    for (auto& e : list) {
        std::tm t = local(e.when);
        char    day[64], tm[16];
        if (t.tm_year == today.tm_year && t.tm_yday == today.tm_yday) std::snprintf(day, sizeof day, "Today");
        else if (t.tm_year == yest.tm_year && t.tm_yday == yest.tm_yday) std::snprintf(day, sizeof day, "Yesterday");
        else std::strftime(day, sizeof day, "%A, %d %B %Y", &t);
        std::strftime(tm, sizeof tm, "%H:%M", &t);
        if (last_day != day) {
            rows += std::string(last_day.empty() ? "" : "</div>") + "<h2>" + html_escape(day) + "</h2><div class=\"day\">";
            last_day = day;
        }
        ParsedUrl   pu;
        std::string host = parse_url(e.url, pu) ? display_host(pu.host) : std::string();
        std::string title = e.title.empty() ? display_url(e.url) : e.title;
        rows += "<a class=\"row\" href=\"" + html_escape(e.url) + "\" title=\"" + html_escape(display_url(e.url)) + "\"><span class=\"t\">" + tm +
                "</span><span class=\"n\">" + html_escape(title) + "</span><span class=\"h\">" + html_escape(host) + "</span></a>";
    }
    if (!last_day.empty()) rows += "</div>";
    std::string note;
    if (!h.enabled()) note = "<p class=\"empty\">History is turned off (<b>history = false</b> in the settings file).</p>";
    else if (list.empty()) note = query.empty() ? "<p class=\"empty\">No pages visited yet.</p>" : "<p class=\"empty\">Nothing matches.</p>";
    const char* css =
        "html{background:#f4f4f7}body{margin:0;font-family:sans-serif;color:#15141a}"
        ".wrap{max-width:860px;margin:0 auto;padding:32px 24px}h1{font-size:26px;margin:0 0 18px}"
        ".bar{display:flex;align-items:center;margin-bottom:10px}"
        ".search{flex:1;display:flex;background:#fff;border:1px solid #cfcfd8;border-radius:20px;padding:3px 3px 3px 16px;margin-right:14px}"
        ".search .q{flex:1;border:0;font-size:15px;padding:7px 4px;background:transparent}"
        ".search .go{border:0;background:#0061e0;color:#fff;font-weight:bold;border-radius:16px;padding:7px 16px}"
        ".clear{color:#b3261e;text-decoration:none;font-size:14px;padding:8px 12px;border-radius:16px;background:#fbe9e7}"
        ".clear:hover{background:#f6d5d1}"
        "h2{font-size:14px;color:#5b5b66;margin:24px 0 6px;font-weight:bold}"
        ".day{background:#fff;border-radius:10px;box-shadow:0 1px 4px rgba(0,0,0,.08);padding:4px 0}"
        ".row{display:flex;padding:7px 16px;text-decoration:none;color:#15141a}.row:hover{background:#ebebf1}"
        ".t{width:52px;color:#8f8f9d;font-size:13px;flex-shrink:0}"
        ".n{flex:1;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;font-size:14px}"
        ".h{color:#8f8f9d;font-size:12px;margin-left:12px;white-space:nowrap}.empty{color:#5b5b66}";
    std::string body = "<div class=\"wrap\"><h1>History</h1><div class=\"bar\"><form class=\"search\" method=\"get\" action=\"about:history\">"
                       "<input class=\"q\" type=\"text\" name=\"q\" value=\"" + html_escape(query) +
                       "\" placeholder=\"Search history\"><input class=\"go\" type=\"submit\" value=\"Search\"></form>" +
                       (h.enabled() && h.size() ? "<a class=\"clear\" href=\"quill:clear-history\">Clear history</a>" : "") + "</div>" + note +
                       rows + "</div>";
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>History</title><style>" + std::string(css) + "</style></head><body>" +
           body + "</body></html>";
}

// The start page (about:home): search box, shortcut tiles, and what this Quill is set up to do.
std::string home_html(const Settings& s) {
    // search form from search_url ("https://host/path?q=%s&x=1" -> GET form with field q and hidden x=1)
    std::string form;
    {
        ParsedUrl pu;
        size_t    ph = s.search_url.find("%s");
        if (ph != std::string::npos && parse_url(s.search_url, pu) && pu.has_query) {
            std::string field, hidden;
            size_t      pos = 0;
            while (pos <= pu.query.size()) {
                size_t amp = pu.query.find('&', pos);
                if (amp == std::string::npos) amp = pu.query.size();
                std::string pair = pu.query.substr(pos, amp - pos);
                pos              = amp + 1;
                size_t eq        = pair.find('=');
                if (eq == std::string::npos) continue;
                std::string k = percent_decode(pair.substr(0, eq), true), v = pair.substr(eq + 1);
                if (v == "%s") field = k;
                else hidden += "<input type=\"hidden\" name=\"" + html_escape(k) + "\" value=\"" + html_escape(percent_decode(v, true)) + "\">";
            }
            pu.query.clear();
            pu.has_query = false;
            if (!field.empty())
                form = "<form class=\"search\" method=\"get\" action=\"" + html_escape(serialize_url(pu)) + "\">" + hidden +
                       "<input class=\"q\" type=\"text\" name=\"" + html_escape(field) +
                       "\" placeholder=\"Search the web or type an address above\"><input class=\"go\" type=\"submit\" value=\"Search\"></form>";
        }
    }
    // shortcut tiles
    std::vector<std::pair<std::string, std::string>> links;
    std::string spec = s.home_links.empty() ? "Wikipedia|https://en.wikipedia.org/, DuckDuckGo|https://html.duckduckgo.com/html/, "
                                              "The Old Net|http://theoldnet.com/, 68k.news|http://68k.news/, FrogFind|http://frogfind.com/, "
                                              "Wiby|https://wiby.me/"
                                            : s.home_links;
    size_t pos = 0;
    while (pos < spec.size() && links.size() < 12) {
        size_t      comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        std::string item = trim(spec.substr(pos, comma - pos));
        pos              = comma + 1;
        size_t bar       = item.find('|');
        if (bar == std::string::npos) continue;
        std::string name = trim(item.substr(0, bar)), url = trim(item.substr(bar + 1));
        std::string lu   = to_lower(url);
        if (!name.empty() && (lu.compare(0, 8, "https://") == 0 || lu.compare(0, 7, "http://") == 0)) links.push_back({name, url});
    }
    static const char* kColors[] = {"#0074d9", "#2e8b57", "#c45014", "#8040bf", "#00898a", "#ba285a", "#5a6e1e", "#465aa0"};
    std::string tiles;
    for (size_t i = 0; i < links.size(); i++) {
        const auto& l    = links[i];
        uint32_t    h    = 2166136261u;
        for (char ch : l.first) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
        std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(l.first[0]))));
        ParsedUrl   pu;
        std::string host = parse_url(l.second, pu) ? display_host(pu.host) : l.second;
        if (host.compare(0, 4, "www.") == 0) host = host.substr(4);
        tiles += "<a class=\"tile\" href=\"" + html_escape(l.second) + "\"><span class=\"badge\" style=\"background:" +
                 kColors[h % (sizeof(kColors) / sizeof(kColors[0]))] + "\">" + html_escape(letter) + "</span><span class=\"name\">" +
                 html_escape(l.first) + "</span><span class=\"host\">" + html_escape(host) + "</span></a>";
    }
    std::string dns = s.doh_url.empty() ? std::string("System DNS") : "DNS over HTTPS";
    std::string chips = "<span class=\"chip\">No JavaScript</span><span class=\"chip\">HTTPS-first + HSTS</span><span class=\"chip\">" +
                        html_escape(dns) + "</span><span class=\"chip\">Cookies: this session only</span>";
    const char* css =
        "html{background:#f4f4f7}body{margin:0;font-family:sans-serif;color:#15141a}"
        ".wrap{max-width:800px;margin:0 auto;padding:64px 24px 32px}"
        ".brand{text-align:center;margin-bottom:28px}.brand svg{width:56px;height:56px;vertical-align:middle}"
        ".brand .word{font-size:44px;font-weight:bold;letter-spacing:1px;vertical-align:middle;margin-left:10px;color:#2b2a33}"
        ".tag{text-align:center;color:#5b5b66;margin:6px 0 28px;font-size:15px}"
        ".search{display:flex;background:#fff;border:1px solid #cfcfd8;border-radius:26px;padding:5px 5px 5px 20px;"
        "box-shadow:0 2px 8px rgba(0,0,0,.08);margin:0 0 36px}"
        ".search .q{flex:1;border:0;font-size:17px;padding:10px 4px;background:transparent;color:#15141a}"
        ".search .go{border:0;background:#0061e0;color:#fff;font-size:15px;font-weight:bold;border-radius:21px;padding:10px 22px}"
        ".tiles{display:flex;flex-wrap:wrap;justify-content:center;margin:0 -6px}"
        ".tile{display:block;width:108px;margin:6px;padding:16px 4px 12px;background:#fff;border-radius:12px;text-align:center;"
        "text-decoration:none;color:#15141a;box-shadow:0 1px 4px rgba(0,0,0,.08)}"
        ".tile:hover{background:#ebebf1}"
        ".badge{display:block;width:40px;height:40px;line-height:40px;margin:0 auto 10px;border-radius:10px;color:#fff;font-weight:bold;"
        "font-size:20px}"
        ".name{display:block;font-size:14px;font-weight:bold}.host{display:block;font-size:11px;color:#8f8f9d;margin-top:2px;"
        "overflow:hidden;white-space:nowrap}"
        ".chips{text-align:center;margin:40px 0 18px}.chip{display:inline-block;font-size:12px;color:#3d3d48;background:#e5e5ec;"
        "border-radius:12px;padding:5px 11px;margin:3px}"
        ".foot{text-align:center;font-size:12px;color:#8f8f9d;line-height:1.7}.foot b{color:#5b5b66}";
    // a feather: two curves and a shaft (original drawing)
    const char* logo =
        "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<path d=\"M52 6C34 8 18 20 14 42l-2 14 4 2 4-12c14-2 26-12 32-24-6 4-12 6-18 6 8-4 14-10 18-22z\" fill=\"#0061e0\"/>"
        "<path d=\"M14 56L40 18\" stroke=\"#ffffff\" stroke-width=\"2.5\" stroke-linecap=\"round\"/></svg>";
    std::string body = std::string("<div class=\"wrap\"><div class=\"brand\">") + logo + "<span class=\"word\">Quill</span></div>" +
                       "<div class=\"tag\">A small, fast browser for the web that's made of documents.</div>" + form +
                       "<div class=\"tiles\">" + tiles + "</div><div class=\"chips\">" + chips + "</div>" +
                       "<div class=\"foot\"><b>Ctrl+L</b> address bar &nbsp; <b>Ctrl+T</b> new tab &nbsp; <b>Ctrl+Shift+T</b> reopen tab "
                       "&nbsp; <b>Alt+Left</b> back &nbsp; <b>Ctrl+H</b> history &nbsp; <b>Esc</b> stop<br>Settings: " + html_escape(default_config_path()) +
                       "</div></div>";
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Quill</title><style>" + std::string(css) + "</style></head><body>" +
           body + "</body></html>";
}

std::string error_html(const FetchResult& fr, const std::string& url) {
    std::string b;
    if (fr.blocked) {
        b += "<h1 class=\"err\">Request blocked</h1>";
        b += "<p>" + html_escape(fr.error) + "</p>";
    } else {
        b += "<h1 class=\"err\">Can't open this page</h1>";
        b += "<p>" + html_escape(fr.error.empty() ? "Unknown error" : fr.error) + "</p>";
    }
    b += "<p><span class=\"url\">" + html_escape(url) + "</span></p>";
    if (!fr.blocked) b += "<p class=\"muted\">Check the address, your connection, and the DNS setting in your config file.</p>";
    return page_html("Can't open page", b);
}

std::string http_warning_html(const std::string& url) {
    ParsedUrl pu;
    std::string host = url;
    std::string https_url;
    if (parse_url(url, pu)) {
        host = pu.host;
        ParsedUrl s = pu;
        s.scheme    = "https";
        s.port      = pu.port == 80 ? -1 : pu.port;
        https_url   = serialize_url(s);
    }
    std::string b = "<h1>This connection is not encrypted</h1>";
    b += "<p><b>" + html_escape(host) + "</b> was requested over plain HTTP. Anyone on the network path can read or change "
         "what you send and receive.</p>";
    b += "<p><span class=\"url\">" + html_escape(url) + "</span></p>";
    if (!https_url.empty()) b += "<a class=\"btn\" href=\"" + html_escape(https_url) + "\">Try the secure (https) version</a>";
    b += "<a class=\"btn\" href=\"quill:allow-http?u=" + percent_encode(url) + "\">Continue over HTTP</a>";
    b += "<p class=\"muted\">\"Continue\" is remembered for " + html_escape(host) +
         " until you quit. Add it to http_allow in the config file to make it permanent.</p>";
    return page_html("Not encrypted", b);
}

std::string plain_text_html(const std::string& url, const std::string& text) {
    return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" + html_escape(url) +
           "</title></head><body style=\"margin:12px 16px;background:#fff;color:#111\"><pre style=\"font-family:monospace;"
           "white-space:pre-wrap;\">" + html_escape(text) + "</pre></body></html>";
}

std::string unsupported_html(const std::string& url, const std::string& type, size_t bytes) {
    std::string b = "<h1>Can't display this file</h1>";
    b += "<p>This page is of type <b>" + html_escape(type.empty() ? "unknown" : type) + "</b> (" + std::to_string(bytes) +
         " bytes). Quill shows HTML, plain text and PNG/JPEG/GIF/BMP/WebP/SVG images and never downloads files on its own.</p>";
    b += "<p><span class=\"url\">" + html_escape(url) + "</span></p>";
    return page_html("Unsupported content", b);
}

std::string cert_warning_html(const FetchResult& fr, const std::string& url) {
    std::string b = "<h1>This device's certificate can't be verified</h1>";
    b += "<p><b>" + html_escape(fr.cert_host) + "</b> is on a private network, but the certificate it presented was not "
         "issued by an authority Quill trusts (typical for routers, NAS boxes and self-hosted apps that use a self-signed certificate).</p>";
    b += "<p class=\"muted\">" + html_escape(fr.cert_error) + "</p>";
    b += "<table>";
    if (!fr.cert_subject.empty()) b += "<tr><td class=\"muted\">Subject</td><td>" + html_escape(fr.cert_subject) + "</td></tr>";
    if (!fr.cert_issuer.empty()) b += "<tr><td class=\"muted\">Issuer</td><td>" + html_escape(fr.cert_issuer) + "</td></tr>";
    if (!fr.cert_expires.empty()) b += "<tr><td class=\"muted\">Expires</td><td>" + html_escape(fr.cert_expires) + "</td></tr>";
    b += "<tr><td class=\"muted\">Key pin</td><td><span class=\"url\">" + html_escape(fr.cert_pin) + "</span></td></tr>";
    b += "</table>";
    b += "<p>If you recognise this device, you can trust <i>this exact key</i> for " + html_escape(fr.cert_host) +
         ". Quill will refuse the connection if the key ever changes. Public websites never get this option.</p>";
    b += "<a class=\"btn\" href=\"quill:trust-cert?h=" + percent_encode(fr.cert_host) + "&p=" + percent_encode(fr.cert_pin) +
         "&u=" + percent_encode(url) + "\">Trust this key for this session</a>";
    b += "<p class=\"muted\">To keep it permanently, add this line to your config file:<br><span class=\"url\">trusted_certs = " +
         html_escape(fr.cert_host) + "=" + html_escape(fr.cert_pin) + "</span></p>";
    return page_html("Certificate not verified", b);
}

// HTTP authentication: the site (a router, a NAS, ...) asks for a username and password. The realm is the site's own
// wording, so it is shown in quotes as what the site says, and never as Quill's.
std::string auth_prompt_html(const FetchResult& fr, const std::string& url, bool failed) {
    ParsedUrl   pu;
    std::string host   = parse_url(url, pu) ? display_host(pu.host) : url;
    std::string origin = url_origin(url);
    bool        plain  = pu.scheme == "http";
    std::string b = "<h1>Sign in to " + html_escape(host) + "</h1>";
    b += "<p><span class=\"url\">" + html_escape(origin) + "</span> is asking for a username and password.</p>";
    if (!fr.auth_realm.empty()) b += "<p class=\"muted\">The site says: “" + html_escape(fr.auth_realm) + "”</p>";
    if (failed) b += "<p class=\"err\"><b>That username and password were not accepted.</b></p>";
    if (plain && fr.auth_scheme == "basic")
        b += "<p class=\"err\">This connection is not encrypted: the password will be sent in a form anyone on the network "
             "path can read.</p>";
    const char* field = "width:280px;font-size:15px;padding:6px 8px;margin-top:4px";
    b += "<form method=\"post\" action=\"quill:auth\" style=\"margin:18px 0\">"
         "<div style=\"margin:8px 0\"><label>Username<br><input name=\"user\" type=\"text\" autofocus style=\"" + std::string(field) +
         "\"></label></div><div style=\"margin:8px 0\"><label>Password<br><input name=\"pass\" type=\"password\" style=\"" +
         std::string(field) + "\"></label></div><div style=\"margin-top:16px;display:flex;align-items:center\">"
         "<input type=\"submit\" value=\"Sign in\" style=\"font-size:15px;padding:7px 18px;margin-right:12px\">"
         "<a class=\"btn\" href=\"quill:auth-cancel\" style=\"margin:0;padding:6px 14px\">Cancel</a></div></form>";
    b += "<p class=\"muted\">Quill remembers what you enter for " + html_escape(origin) +
         " until you quit, and sends it only to that site's own pages.</p>";
    return page_html("Sign in", b);
}

std::string resend_html(const std::string& url) {
    std::string b = "<h1>Resend form data?</h1>";
    b += "<p>This page was the result of submitting a form. Loading it again sends the same data to the site a second time "
         "(for a purchase or a post, that could repeat the action).</p>";
    b += "<p><span class=\"url\">" + html_escape(url) + "</span></p>";
    b += "<a class=\"btn\" href=\"quill:resend\">Resend</a>";
    return page_html("Resend form data", b);
}

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

bool        framing_allowed(const std::string& xfo, const std::string& csp, const std::string& page_url, const std::string& parent_url);
std::string framing_refused_html(const std::string& url);

} // namespace

Browser::Browser(const Settings& s, std::function<void()> wake, std::shared_ptr<RequestFilter> filter) :
    settings_(s), wake_(std::move(wake)), net_owner_(std::make_shared<Network>(s, std::move(filter))), net_(*net_owner_),
    pool_(new ThreadPool(s.image_threads + 1)), doc_pool_(new ThreadPool(2)) {
    if (!s.font_dir.empty()) FontManager::instance().set_extra_dir(s.font_dir);
    last_layout_ = std::chrono::steady_clock::now();
}

Browser::Browser(Browser* parent, const std::string& name, int scrolling) :
    settings_(parent->settings_), wake_(parent->wake_), net_owner_(parent->net_owner_), net_(*net_owner_), pool_(new ThreadPool(2)), doc_pool_(new ThreadPool(1)),
    parent_(parent), frame_name_(name), no_scroll_(scrolling == 2) {
    last_layout_ = std::chrono::steady_clock::now();
}

Browser::~Browser() {
    frames_.clear(); // child frames first: they join their own workers
    iframes_.clear();
    if (loading_state_) loading_state_->cancel = true;
    if (current_) current_->state->cancel = true;
    doc_pool_.reset(); // joins workers before anything they touch is destroyed
    pool_.reset();
}

// ------------------------------------------------------------------ navigation
void Browser::navigate(const std::string& input) {
    InputResult r = resolve_input(input, settings_, net_.policy());
    if (r.url.empty()) return;
    if (r.scheme_guessed) {
        LoadSpec sp;
        sp.url            = r.url;
        sp.guessed_scheme = true;
        navigate_spec(sp);
        return;
    }
    navigate_url(r.url, false);
}

void Browser::navigate_url(const std::string& url_in, bool from_page) {
    std::string url = url_in;

    if (starts_with(to_lower(url), "quill:")) {
        // Internal commands only work from Quill's own pages (or the address bar), never from web content.
        if (from_page && !(current_ && current_->internal)) return;
        if (starts_with(url, "quill:allow-http?")) {
            std::string target = query_param(url.substr(std::strlen("quill:allow-http?")), "u");
            ParsedUrl   pu;
            if (parse_url(target, pu) && pu.scheme == "http" && !pu.host.empty()) {
                net_.allow_http_host(pu.host);
                if (idx_ >= 0) hist_[idx_].url = target;
                LoadSpec sp;
                sp.url = target;
                start_load(sp, 0);
            }
        } else if (starts_with(url, "quill:trust-cert?")) {
            std::string q      = url.substr(std::strlen("quill:trust-cert?"));
            std::string host   = query_param(q, "h"), pin = query_param(q, "p"), target = query_param(q, "u");
            ParsedUrl   pu;
            if (parse_url(target, pu) && pu.scheme == "https" && normalize_host(pu.host) == normalize_host(host)) {
                net_.trust_cert(host, pin);
                LoadSpec sp;
                sp.url = target;
                start_load(sp, 0);
            }
        } else if (url == "quill:auth-cancel") {
            if (current_ && current_->auth_retry) { // show what the site sent with its 401 instead
                LoadSpec sp       = *current_->auth_retry;
                sp.no_auth_prompt = true;
                start_load(sp, 0);
            }
        } else if (url == "quill:clear-history") {
            History::instance().clear();
            LoadSpec sp;
            sp.url = "about:history";
            if (idx_ >= 0) hist_[idx_].url = sp.url;
            start_load(sp, 0);
        } else if (url == "quill:resend") {
            if (idx_ >= 0 && hist_[idx_].post) {
                LoadSpec sp;
                sp.url          = hist_[idx_].url;
                sp.method       = "POST";
                sp.body         = hist_[idx_].body;
                sp.content_type = hist_[idx_].content_type;
                if (current_) sp.initiator = ""; // a resend has no originating page any more
                start_load(sp, 0);
            }
        }
        return;
    }

    LoadSpec sp;
    sp.url = url;
    if (from_page && current_ && !current_->internal) sp.source_page = current_->display_url;
    navigate_spec(sp);
}

void Browser::navigate_spec(const LoadSpec& sp) {
    notice_.clear();
    // A fresh blank tab has nothing worth going "back" to: the first real page replaces it.
    if (idx_ == 0 && hist_.size() == 1 && to_lower(hist_[0].url) == "about:blank") {
        hist_.clear();
        idx_ = -1;
    }
    if (idx_ >= 0) hist_[idx_].scroll = scroll_;
    if (idx_ + 1 < static_cast<int>(hist_.size())) hist_.resize(idx_ + 1);
    HistEntry e;
    e.url          = sp.url;
    e.post         = sp.method == "POST";
    e.body         = sp.body;
    e.content_type = sp.content_type;
    e.referrer        = sp.referrer;
    e.referrer_policy = sp.referrer_policy;
    e.no_referrer     = sp.no_referrer;
    hist_.push_back(e);
    idx_ = static_cast<int>(hist_.size()) - 1;
    start_load(sp, 0);
    if (parent_) parent_->frame_navigated(); // a frame navigated: the frameset page gets the history entry
}

void Browser::stop() {
    for (auto& f : frames_) f.br->stop();
    for (auto& f : iframes_) f.br->stop();
    if (!loading_) return;
    ++load_id_; // whatever the cancelled load still delivers is ignored
    if (loading_state_) loading_state_->cancel = true;
    loading_state_.reset();
    loading_ = false;
    // drop the history entry of the abandoned navigation (the page on screen is the one before it)
    if (current_ && idx_ > 0 && idx_ == static_cast<int>(hist_.size()) - 1 && hist_[static_cast<size_t>(idx_)].url != current_->display_url &&
        hist_[static_cast<size_t>(idx_)].url != current_->requested_url) {
        hist_.pop_back();
        idx_--;
    }
}

void Browser::load_entry(int idx, int restore_scroll) {
    const HistEntry& e = hist_[static_cast<size_t>(idx)];
    LoadSpec         sp;
    sp.url             = e.url;
    sp.referrer        = e.referrer;
    sp.referrer_policy = e.referrer_policy;
    sp.no_referrer     = e.no_referrer;
    if (e.post) { // never re-POST silently
        sp.method         = "POST";
        sp.body           = e.body;
        sp.content_type   = e.content_type;
        sp.confirm_resend = true;
    }
    start_load(sp, restore_scroll);
}

void Browser::reload() {
    if (idx_ < 0) return;
    load_entry(idx_, scroll_);
}

void Browser::back() {
    if (!can_back()) return;
    hist_[idx_].scroll = scroll_;
    idx_--;
    if (!go_to_entry(idx_ + 1, idx_)) load_entry(idx_, hist_[idx_].scroll);
}

void Browser::forward() {
    if (!can_forward()) return;
    hist_[idx_].scroll = scroll_;
    idx_++;
    if (!go_to_entry(idx_ - 1, idx_)) load_entry(idx_, hist_[idx_].scroll);
}

void Browser::start_load(const LoadSpec& spec, int restore_scroll) {
    ++load_id_;
    refresh_pending_ = false; // going somewhere else cancels a page's pending meta refresh
    if (loading_state_) loading_state_->cancel = true;
    auto st  = std::make_shared<PageState>();
    st->wake = wake_;
    loading_state_  = st;
    loading_        = true;
    loading_url_    = spec.url;
    restore_scroll_ = restore_scroll;
    int id = load_id_, vw = vw_, vh = vh_;
    // Its own workers: on the shared pool it waited behind every image the current page still had queued.
    doc_pool_->submit([this, st, spec, id, vw, vh]() { load_job(st, spec, id, vw, vh); });
}

// ------------------------------------------------------------------ loading (worker thread)

Browser::Page::~Page() {
    {
        std::lock_guard<std::mutex> lk(engine_mutex());
        doc.reset(); // libdom/libcss release interned strings: not safe next to another thread parsing
    }
    if (font_scope) FontManager::instance().drop_scope(font_scope); // nothing draws with them any more
}

// ------------------------------------------------------------------ loading (worker thread)
void Browser::load_job(std::shared_ptr<PageState> st, LoadSpec spec, int id, int vw, int vh) {
    const std::string& url = spec.url;
    auto page           = std::make_shared<Page>();
    page->state         = st;
    page->requested_url = url;
    page->display_url   = url;
    bool final_post     = false;
    double html_ms      = 0; // QUILL_PROF: time to fetch the document

    std::string            html;
    std::string            final_url = url;
    std::string            charset;
    std::shared_ptr<Image> prime;
    std::string            ref_policy; // the document's Referrer-Policy

    std::string lurl = to_lower(url);
    if (spec.confirm_resend) {
        html              = resend_html(url);
        page->internal    = true;
        page->display_url = url;
        final_post        = true; // keeps the history entry a POST entry
    } else if (spec.embedded && (lurl == "about:blank" || lurl == "about:srcdoc")) {
        // an <iframe> with no src (an empty page) or with its HTML inline in srcdoc (resolved against the parent page)
        html              = lurl == "about:srcdoc" ? spec.srcdoc : std::string("<!DOCTYPE html><html><body></body></html>");
        page->display_url = lurl;
        if (lurl == "about:srcdoc" && !spec.initiator.empty()) final_url = spec.initiator;
        else page->internal = true;
    } else if (!spec.embedded && (lurl == "about:history" || starts_with(lurl, "about:history?"))) {
        size_t      qm = url.find('?');
        std::string q  = qm == std::string::npos ? std::string() : query_param(url.substr(qm + 1), "q");
        html              = history_html(trim(q));
        page->internal    = true;
        page->display_url = url;
        final_url         = url;
    } else if (lurl == "about:home" || lurl == "about:blank" || lurl == "about:") {
        html           = lurl == "about:blank" ? page_html("", "") : home_html(settings_);
        page->internal = true;
        page->display_url = lurl == "about:blank" ? "about:blank" : "about:home";
        final_url         = "about:" + std::string(lurl == "about:blank" ? "blank" : "home");
    } else {
        FetchRequest rq;
        rq.url          = url;
        rq.kind         = spec.embedded ? RequestKind::Subresource : RequestKind::TopLevel; // an iframe: part of its page
        rq.type         = ResourceType::Document;
        rq.max_bytes    = settings_.max_page_bytes;
        rq.cancel       = &st->cancel;
        rq.method       = spec.method;
        rq.body         = spec.body;
        rq.content_type = spec.content_type;
        rq.initiator    = spec.initiator;
        rq.source_page  = spec.source_page;
        rq.form_submit  = spec.form_submit;
        rq.referrer        = spec.referrer;
        rq.referrer_policy = spec.referrer_policy;
        rq.no_referrer     = spec.no_referrer;
        rq.user_activated  = spec.user_activated;
        auto        html_t0 = std::chrono::steady_clock::now();
        FetchResult fr      = net_.fetch(rq);
        html_ms             = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - html_t0).count();
        if (!fr.ok && spec.guessed_scheme && spec.method == "GET" && starts_with(to_lower(url), "http://") &&
            !st->cancel.load()) {
            // Typed a bare LAN address, so http:// was only a guess. Servers like Crafty Controller, Proxmox or
            // routers speak TLS on that port and reset or reject plain http: try https:// before giving up.
            std::string e = to_lower(fr.error);
            if (e.find("recv failure") != std::string::npos || e.find("empty reply") != std::string::npos ||
                e.find("reset") != std::string::npos || e.find("bad request") != std::string::npos ||
                e.find("http/0.9") != std::string::npos || e.find("received http") != std::string::npos) {
                rq.url = "https://" + url.substr(7);
                FetchResult f2 = net_.fetch(rq);
                if (f2.ok || f2.needs_cert_confirm || f2.needs_http_confirm) fr = std::move(f2);
            }
        }
        if (st->cancel.load() || fr.cancelled) return; // superseded; nothing to publish
        final_post = fr.ok && spec.method == "POST" && fr.final_url == url;

        if (fr.needs_http_confirm) {
            html           = http_warning_html(fr.final_url);
            page->internal = true;
            final_url      = fr.final_url; // may differ from the typed URL after redirects
            page->requested_url = fr.final_url;
            page->display_url   = fr.final_url;
        } else if (fr.auth_required && !spec.embedded && !spec.no_auth_prompt) {
            std::string at = fr.final_url.empty() ? url : fr.final_url;
            if (fr.auth_failed) CredentialStore::instance().forget(url_origin(at));
            html                = auth_prompt_html(fr, at, fr.auth_failed);
            page->internal      = true;
            page->requested_url = at;
            page->display_url   = at;
            page->auth_retry    = std::make_shared<LoadSpec>(spec);
            page->auth_retry->url = at;
            page->auth_origin   = url_origin(at);
            page->auth_scheme   = fr.auth_scheme;
            page->auth_realm    = fr.auth_realm;
        } else if (fr.needs_cert_confirm) {
            html                = cert_warning_html(fr, fr.final_url.empty() ? url : fr.final_url);
            page->internal      = true;
            page->requested_url = fr.final_url;
            page->display_url   = fr.final_url;
        } else if (!fr.ok) {
            html           = error_html(fr, fr.final_url.empty() ? url : fr.final_url);
            page->internal = true;
        } else if (spec.embedded && !framing_allowed(fr.frame_options, fr.csp, fr.final_url, spec.initiator)) {
            html                = framing_refused_html(fr.final_url);
            page->internal      = true;
            page->display_url   = fr.final_url;
            page->requested_url = fr.final_url;
        } else {
            final_url         = fr.final_url;
            page->display_url = final_url;
            {
                ParsedUrl fu;
                if (parse_url(final_url, fu) && fu.scheme == "http") {
                    page->insecure = !net_.policy().is_private_host(fu.host);
                    // the policy let this http page through: reload / back / forward to it must not ask again
                    // (not for an iframe's page: the user never chose to open that one)
                    if (!spec.embedded) net_.allow_http_host(fu.host);
                }
            }
            ref_policy = fr.referrer_policy;
            const std::string& ct = fr.content_type;
            if (ct.empty() || ct == "text/html" || ct == "application/xhtml+xml") {
                html = std::move(fr.body);
                charset = fr.charset;
                if (fr.status >= 400 && html.empty()) {
                    FetchResult e2;
                    e2.error = "The server returned HTTP status " + std::to_string(fr.status);
                    html     = error_html(e2, final_url);
                    page->internal = true;
                }
            } else if (starts_with(ct, "text/") || ct == "application/json" || ct == "application/xml" ||
                       ct == "application/javascript") {
                html = plain_text_html(final_url, fr.body);
            } else if (starts_with(ct, "image/")) {
                prime = decode_image(reinterpret_cast<const unsigned char*>(fr.body.data()), fr.body.size());
                if (prime) {
                    html = "<!DOCTYPE html><html><head><title>" + html_escape(final_url) +
                           "</title></head><body style=\"margin:0;background:#2b2b2b\"><img src=\"" + html_escape(final_url) +
                           "\"></body></html>";
                } else {
                    html           = unsupported_html(final_url, ct, fr.body.size());
                    page->internal = true;
                }
            } else {
                html           = unsupported_html(final_url, ct, fr.body.size());
                page->internal = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(st->mu);
        st->url = final_url;
    }
    page->images.reset(new ImageLoader(st, &net_, pool_.get(), settings_.max_image_bytes));
    if (prime) page->images->prime(final_url, prime);

    std::string host;
    {
        ParsedUrl pu;
        if (parse_url(final_url, pu)) host = pu.host;
    }
    std::string user_css = net_.filter()->cosmetic_css(host);

    {
        std::unique_lock<std::mutex> lk(engine_mutex());
        page->doc.reset(new engine::Document());
        engine::Document& doc = *page->doc;
        doc.parse_html(html, charset);
        {
            std::string m = parse_referrer_policy(doc.meta_named("referrer"));
            if (!m.empty()) ref_policy = m;
            std::lock_guard<std::mutex> slk(st->mu);
            st->referrer_policy = ref_policy;
        }
        std::string base = final_url;
        std::string bh   = doc.base_href();
        if (!bh.empty()) base = resolve_url(final_url, bh);
        page->base_url = base;
        page->images->set_base(base);

        int  css_count = 0;
        auto css_request = [&](const std::string& href) {
            FetchRequest r;
            r.url       = strip_fragment(href);
            r.kind      = RequestKind::Subresource;
            r.type      = ResourceType::Stylesheet;
            r.max_bytes = 2u << 20;
            r.cancel    = &st->cancel;
            r.initiator = final_url;
            r.referrer_policy = ref_policy;
            return r;
        };
        // Stylesheets download in parallel (all but the first, which takes this thread's warm connection); the loader
        // below then picks the results up in document order.
        std::map<std::string, std::future<FetchResult>> css_prefetch;
        auto css_t0 = std::chrono::steady_clock::now();
        doc.load_stylesheets(
            [&](const std::string& href, std::string& css) {
                if (st->cancel.load() || ++css_count > 60 || href.empty()) return false;
                FetchResult fr;
                lk.unlock(); // network wait: let the UI thread use the engine meanwhile
                auto it = css_prefetch.find(strip_fragment(href));
                if (it != css_prefetch.end() && it->second.valid()) fr = it->second.get();
                else fr = net_.fetch(css_request(href));
                lk.lock();
                if (!fr.ok || fr.status < 200 || fr.status >= 300) return false;
                css = std::move(fr.body);
                if (css.compare(0, 3, "\xEF\xBB\xBF") == 0) css.erase(0, 3);
                return true;
            },
            base,
            [&](const std::vector<std::string>& urls) {
                for (size_t i = 1; i < urls.size() && i <= 8; i++) {
                    std::string key = strip_fragment(urls[i]);
                    if (css_prefetch.count(key)) continue;
                    FetchRequest r = css_request(urls[i]);
                    css_prefetch.emplace(key, std::async(std::launch::async, [this, r] { return net_.fetch(r); }));
                }
            });
        if (std::getenv("QUILL_PROF"))
            std::fprintf(stderr, "load: html %.0fms, %d stylesheet(s) %.0fms (fetch + parse)\n", html_ms, css_count,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - css_t0).count());
        if (!user_css.empty()) doc.add_author_css(user_css, base);
        doc.set_text_measurer(&page->measurer);
        ImageLoader* il = page->images.get();
        doc.set_image_sizer([il](const std::string& src, int& w, int& h) { return il->size(src, w, h); });
        doc.set_image_broken([il](const std::string& src) { return il->broken(src); });
        doc.set_image_request([il](const std::string& src) { il->get(src); }); // get() starts the download
        doc.set_link_visited([base](const std::string& href) { return VisitedLinks::instance().contains(resolve_url(base, href)); });
        if (!page->internal) { // visited from now on (before styling, so links to this very page come out :visited)
            VisitedLinks::instance().add(page->display_url);
            VisitedLinks::instance().add(page->requested_url); // before redirects: that's what links point at
        }
        page->visited_gen = VisitedLinks::instance().generation();
        page->base_target = doc.base_target();
        {
            double      delay = 0;
            std::string ru;
            if (!page->internal && doc.meta_refresh(delay, ru)) {
                std::string target = ru.empty() ? final_url : resolve_url(base, ru);
                std::string lt     = to_lower(target);
                if (starts_with(lt, "http://") || starts_with(lt, "https://")) {
                    page->refresh_delay = std::min(delay, 86400.0);
                    page->refresh_url   = target;
                }
            }
        }
        if (settings_.web_fonts && !page->internal) {
            page->font_scope = FontManager::instance().new_scope();
            doc.set_font_scope(page->font_scope);
        }
        page->zoom = zoom_for(final_url);
        doc.set_zoom(page->zoom);
        doc.resolve_styles(vw, vh);
        if (page->font_scope) {
            std::vector<engine::FontFaceRule> faces = doc.font_faces_in_use();
            if (!faces.empty()) {
                lk.unlock(); // network wait
                load_web_fonts(page->font_scope, faces, final_url, st);
                lk.lock();
            }
        }
        {
            engine::FrameNode fs;
            if (!page->internal && doc.frameset(fs)) page->frameset = std::make_shared<engine::FrameNode>(std::move(fs));
        }
        auto lay_t0 = std::chrono::steady_clock::now();
        doc.layout(vw, vh);
        page->layout_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lay_t0).count();
        page->laid_w = vw;
        page->laid_h = vh;
        page->title  = doc.title();
        if (page->title.empty()) page->title = page->display_url;
        page->forms = doc.forms();
        page->maps  = doc.image_maps();
        if (settings_.favicons && !page->internal && !parent_ && !spec.embedded) {
            ParsedUrl pu;
            if (parse_url(final_url, pu) && (pu.scheme == "http" || pu.scheme == "https")) {
                std::string href = doc.icon_href();
                if (!href.empty()) page->icon_url = resolve_url(base, href);
                else page->icon_url = pu.scheme + "://" + (pu.host.find(':') != std::string::npos ? "[" + pu.host + "]" : pu.host) +
                                      (pu.port > 0 ? ":" + std::to_string(pu.port) : "") + "/favicon.ico";
            }
        }
        for (auto& ci : doc.controls()) page->controls.push_back(std::make_shared<FormControl>(ci));
    }
    if (st->cancel.load()) return;

    auto lr        = std::make_shared<LoadResult>();
    lr->id         = id;
    lr->page       = page;
    lr->final_post = final_post;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = lr;
    }
    if (wake_) wake_();
}

// ------------------------------------------------------------------ web fonts
namespace {
// Sanitized fonts by URL, shared by every tab (pages of one site use the same few fonts).
struct FontMemCache {
    std::mutex mu;
    std::map<std::string, std::pair<std::shared_ptr<const std::vector<unsigned char>>, bool>> m; // url -> (data, variable)
    size_t     bytes = 0;
};
FontMemCache& font_mem() {
    static auto* c = new FontMemCache(); // leaked: worker threads may use it until exit
    return *c;
}
constexpr size_t    kFontMemCap  = 48u << 20;
constexpr size_t    kFontDiskCap = 96u << 20;
constexpr long long kFontMaxAge  = 30LL * 86400;
const char          kFontMagic[] = "QF1";

bool font_src_usable(const std::string& url, const std::string& fmt) {
    std::string f = fmt, u = to_lower(strip_fragment(url));
    size_t      q = u.find('?');
    if (q != std::string::npos) u.erase(q);
    auto ends = [&](const char* e) { size_t n = std::strlen(e); return u.size() >= n && u.compare(u.size() - n, n, e) == 0; };
    if (f == "embedded-opentype" || f == "svg" || f == "collection") return false;
    if (f.empty() && (ends(".eot") || ends(".svg") || ends(".svgz"))) return false;
    if ((f == "woff2" || f == "woff2-variations" || (f.empty() && ends(".woff2"))) && !woff2_supported()) return false;
    return true;
}

std::shared_ptr<const std::vector<unsigned char>> remember_font(const std::string& url, SanitizedFont&& sf) {
    auto  data = std::make_shared<const std::vector<unsigned char>>(std::move(sf.data));
    auto& mc   = font_mem();
    std::lock_guard<std::mutex> lk(mc.mu);
    if (mc.bytes + data->size() > kFontMemCap) { mc.m.clear(); mc.bytes = 0; }
    if (!mc.m.count(url)) {
        mc.m[url] = {data, sf.variable};
        mc.bytes += data->size();
    }
    return data;
}

// One @font-face: the first src that yields a usable font, from memory, disk or the network.
std::shared_ptr<const std::vector<unsigned char>> fetch_font(Network& net, const engine::FontFaceRule& f, const std::string& page_url,
                                                            std::atomic<bool>* cancel, bool& variable, const std::string& ref_policy) {
    for (auto& src : f.src) {
        if (cancel && cancel->load()) return nullptr;
        const std::string& url = src.first;
        if (!font_src_usable(url, src.second)) continue;
        {
            auto& mc = font_mem();
            std::lock_guard<std::mutex> lk(mc.mu);
            auto it = mc.m.find(url);
            if (it != mc.m.end()) { variable = it->second.second; return it->second.first; }
        }
        bool        is_data = to_lower(url).compare(0, 5, "data:") == 0;
        std::string cached;
        if (!is_data && DiskCache::instance().get("fonts", url, cached, kFontMaxAge) && cached.size() > 4 && cached.compare(0, 3, kFontMagic) == 0) {
            SanitizedFont sf; // cached files are our own rebuilt fonts, but checked again all the same
            if (sanitize_font(reinterpret_cast<const unsigned char*>(cached.data()) + 4, cached.size() - 4, sf)) {
                sf.variable = cached[3] == '1';
                variable    = sf.variable;
                return remember_font(url, std::move(sf));
            }
        }
        std::string body;
        if (is_data) {
            std::string mime;
            if (url.size() > (24u << 20) || !decode_data_uri(url, mime, body)) continue;
        } else {
            FetchRequest r;
            r.url       = strip_fragment(url);
            r.kind      = RequestKind::Subresource;
            r.type      = ResourceType::Font;
            r.max_bytes = 16u << 20;
            r.cancel    = cancel;
            r.initiator = page_url;
            r.referrer_policy = ref_policy;
            FetchResult fr = net.fetch(r);
            if (!fr.ok || fr.status < 200 || fr.status >= 300) continue;
            body = std::move(fr.body);
        }
        SanitizedFont sf;
        std::string   err;
        if (!sanitize_font(reinterpret_cast<const unsigned char*>(body.data()), body.size(), sf, &err)) {
            if (std::getenv("QUILL_PROF")) std::fprintf(stderr, "font %s: %s\n", url.c_str(), err.c_str());
            continue;
        }
        variable = sf.variable;
        if (!is_data) {
            std::string blob = std::string(kFontMagic) + (sf.variable ? '1' : '0');
            blob.append(reinterpret_cast<const char*>(sf.data.data()), sf.data.size());
            DiskCache::instance().put("fonts", url, blob, kFontDiskCap);
        }
        return remember_font(url, std::move(sf));
    }
    return nullptr;
}

FontManager::WebFace web_face(const engine::FontFaceRule& f, std::shared_ptr<const std::vector<unsigned char>> data, bool variable) {
    FontManager::WebFace w;
    w.family    = f.family;
    w.weight_lo = f.weight_lo;
    w.weight_hi = f.weight_hi;
    w.italic    = f.italic;
    w.variable  = variable;
    w.ranges    = f.ranges;
    w.data      = std::move(data);
    return w;
}
} // namespace

void Browser::load_web_fonts(uint64_t scope, const std::vector<engine::FontFaceRule>& faces, const std::string& page_url,
                             const std::shared_ptr<PageState>& st) {
    struct Job {
        engine::FontFaceRule rule;
        std::future<std::pair<std::shared_ptr<const std::vector<unsigned char>>, bool>> fut;
        bool optional_late = false;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (auto& f : faces) {
        Job j;
        j.rule = f;
        j.fut  = std::async(std::launch::async, [this, f, page_url, st]() {
            bool v    = false;
            std::string pol;
            {
                std::lock_guard<std::mutex> lk(st->mu);
                pol = st->referrer_policy;
            }
            auto data = fetch_font(net_, f, page_url, &st->cancel, v, pol);
            return std::make_pair(data, v);
        });
        jobs->push_back(std::move(j));
    }
    // The page waits a moment for its fonts, then shows in fallback fonts and switches when they arrive. How long
    // follows font-display: block 1 s (browsers: up to 3 s), auto 400 ms, fallback / optional 100 ms, swap ~0.
    // (auto used to wait the full second too: on a slow machine, where fetching and unpacking a WOFF2 font takes a
    // while, nearly every first visit to a site sat on a blank page for that long.)
    // optional fonts that miss that are left out for this visit (the next one finds them cached).
    auto t0 = std::chrono::steady_clock::now();
    bool late = false;
    for (auto& j : *jobs) {
        int  wait_ms  = j.rule.display == 1 ? 30 : (j.rule.display == 2 || j.rule.display == 3) ? 100 : j.rule.display == 4 ? 1000 : 400; // (30: long enough for a cached font)
        auto deadline = t0 + std::chrono::milliseconds(wait_ms);
        if (j.fut.wait_until(deadline) != std::future_status::ready) {
            late = true;
            if (j.rule.display == 3) j.optional_late = true;
            continue;
        }
        auto r = j.fut.get();
        if (r.first) FontManager::instance().add_web_face(scope, web_face(j.rule, r.first, r.second));
    }
    if (!late) return;
    pool_->submit([jobs, scope, st]() {
        bool any = false;
        for (auto& j : *jobs) {
            if (!j.fut.valid()) continue;
            auto r = j.fut.get(); // (still fetched: it lands in the disk cache for next time)
            if (j.optional_late) continue;
            if (r.first && !st->cancel.load() && FontManager::instance().add_web_face(scope, web_face(j.rule, r.first, r.second))) any = true;
        }
        if (any && !st->cancel.load()) {
            st->dirty = true; // lay out again with the font (FontManager's generation changed)
            if (st->wake) st->wake();
        }
    });
}

// ------------------------------------------------------------------ site icons
namespace {
struct IconMemCache {
    std::mutex                                                   mu;
    std::unordered_map<std::string, std::shared_ptr<const Image>> m; // null = the site has none (this session)
};
IconMemCache& icon_mem() {
    static auto* c = new IconMemCache();
    return *c;
}
constexpr size_t    kIconDiskCap = 8u << 20;
constexpr long long kIconMaxAge  = 7LL * 86400;
constexpr int       kIconPx      = 32;

extern "C" int stbi_write_png_to_func(void (*f)(void*, void*, int), void* c, int w, int h, int comp, const void* d, int stride);

void png_append(void* ctx, void* data, int n) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(n)); }

// The icon fitted into kIconPx x kIconPx (aspect kept, centred), as a plain RGBA image. Small bitmaps (the classic
// 16x16 favicon) are enlarged by whole pixels so they stay crisp; bigger ones and SVGs are resampled.
std::shared_ptr<const Image> fit_icon(const Image& src) {
    if (src.w <= 0 || src.h <= 0) return nullptr;
    auto out = std::make_shared<Image>();
    out->w   = kIconPx;
    out->h   = kIconPx;
    out->px.assign(static_cast<size_t>(kIconPx) * kIconPx, 0);
    if (!src.svg && src.w <= kIconPx && src.h <= kIconPx) {
        int k  = std::max(1, kIconPx / std::max(src.w, src.h));
        int ox = (kIconPx - src.w * k) / 2, oy = (kIconPx - src.h * k) / 2;
        for (int y = 0; y < src.h * k; y++)
            for (int x = 0; x < src.w * k; x++)
                out->px[static_cast<size_t>(y + oy) * kIconPx + (x + ox)] = src.px[static_cast<size_t>(y / k) * src.w + x / k];
        return out;
    }
    float s  = std::min(static_cast<float>(kIconPx) / src.w, static_cast<float>(kIconPx) / src.h);
    int   tw = std::max(1, static_cast<int>(std::lround(src.w * s))), th = std::max(1, static_cast<int>(std::lround(src.h * s)));
    const Image& sc = src.scaled(tw, th); // the image is still private to this thread
    int ox = (kIconPx - sc.w) / 2, oy = (kIconPx - sc.h) / 2;
    for (int y = 0; y < sc.h && y + oy < kIconPx; y++)
        for (int x = 0; x < sc.w && x + ox < kIconPx; x++)
            out->px[static_cast<size_t>(y + oy) * kIconPx + (x + ox)] = sc.px[static_cast<size_t>(y) * sc.w + x];
    return out;
}

std::string icon_png(const Image& img) {
    std::string rgba(static_cast<size_t>(img.w) * img.h * 4, '\0');
    for (size_t i = 0; i < img.px.size(); i++) {
        uint32_t p  = img.px[i];
        rgba[i * 4] = static_cast<char>((p >> 16) & 255);
        rgba[i * 4 + 1] = static_cast<char>((p >> 8) & 255);
        rgba[i * 4 + 2] = static_cast<char>(p & 255);
        rgba[i * 4 + 3] = static_cast<char>((p >> 24) & 255);
    }
    std::string out;
    stbi_write_png_to_func(png_append, &out, img.w, img.h, 4, rgba.data(), img.w * 4);
    return out;
}
} // namespace

void Browser::start_favicon(const std::string& icon_url, const std::string& page_url) {
    {
        std::lock_guard<std::mutex> lk(icon_->mu);
        if (icon_->url == icon_url) return;
        icon_->url = icon_url;
        icon_->img.reset();
        if (icon_url.empty()) return;
        auto& mc = icon_mem();
        std::lock_guard<std::mutex> lk2(mc.mu);
        auto it = mc.m.find(icon_url);
        if (it != mc.m.end()) { icon_->img = it->second; return; }
    }
    std::shared_ptr<IconSlot> slot = icon_;
    Network&                  net  = net_;
    auto                      wake = wake_;
    pool_->submit([slot, icon_url, page_url, &net, wake]() {
        std::shared_ptr<const Image> img;
        std::string                  cached;
        if (DiskCache::instance().get("icons", icon_url, cached, kIconMaxAge)) {
            if (auto d = decode_image(reinterpret_cast<const unsigned char*>(cached.data()), cached.size(), 1u << 16)) img = fit_icon(*d);
        }
        if (!img) {
            std::string body;
            bool        ok = false;
            if (to_lower(icon_url).compare(0, 5, "data:") == 0) {
                std::string mime;
                ok = icon_url.size() < (2u << 20) && decode_data_uri(icon_url, mime, body);
            } else {
                FetchRequest r;
                r.url       = strip_fragment(icon_url);
                r.kind      = RequestKind::Subresource;
                r.type      = ResourceType::Image;
                r.max_bytes = 1u << 20;
                r.initiator = page_url;
                FetchResult fr = net.fetch(r);
                ok   = fr.ok && fr.status >= 200 && fr.status < 300;
                body = std::move(fr.body);
            }
            std::shared_ptr<Image> d;
            if (ok) d = decode_image(reinterpret_cast<const unsigned char*>(body.data()), body.size(), 4u << 20);
            if (d) img = fit_icon(*d);
            if (img && to_lower(icon_url).compare(0, 5, "data:") != 0) DiskCache::instance().put("icons", icon_url, icon_png(*img), kIconDiskCap);
        }
        {
            auto& mc = icon_mem();
            std::lock_guard<std::mutex> lk(mc.mu);
            if (mc.m.size() > 2000) mc.m.clear();
            mc.m[icon_url] = img;
        }
        bool show = false;
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            if (slot->url == icon_url && img) {
                slot->img   = img;
                slot->fresh = true;
                show        = true;
            }
        }
        if (show && wake) wake();
    });
}

// ------------------------------------------------------------------ UI thread
bool Browser::poll() {
    bool                        redraw = false;
    for (auto& f : frames_)
        if (f.br->poll()) redraw = true;
    {
        std::lock_guard<std::mutex> lk(icon_->mu);
        if (icon_->fresh) { icon_->fresh = false; redraw = true; }
    }
    for (size_t i = 0; i < iframes_.size(); i++)
        if (iframes_[i].br->poll()) { // the frame's page changed: repaint the strips it is on
            invalidate_doc_rect(iframes_[i].rect);
            redraw = true;
        }
    std::shared_ptr<LoadResult> lr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lr = std::move(ready_);
    }
    if (lr && lr->id == load_id_) {
        install(lr);
        redraw = true;
    }
    if (state_pending_) push_state(); // a hover / focus change that found the engine busy
    if (hover_pending_ && current_ && current_->doc && relayout(true, false)) {
        hover_pending_ = false;
        redraw         = true;
    }
    // A page was visited (in this or another tab or frame): links to it on this page turn :visited.
    if (current_ && current_->doc && !current_->internal && current_->visited_gen != VisitedLinks::instance().generation()) {
        uint64_t gen = VisitedLinks::instance().generation();
        current_->doc->restyle_all();
        if (relayout(true, false)) {
            current_->visited_gen = gen;
            redraw                = true;
        }
    }
    if (current_ && current_->state->repaint.exchange(false) && !current_->state->dirty.load()) {
        invalidate_tiles(); // a picture arrived whose box was already the right size: paint, no layout
        redraw = true;
    }
    if (current_ && current_->state->dirty.load()) {
        auto now = std::chrono::steady_clock::now();
        // Pictures arriving one after another each want a new layout. Space them out by the time a layout takes
        // (twice it, at least 80 ms): on a slow machine a big page's layout alone can run a few hundred ms, and
        // back-to-back ones would leave no time for scrolling or drawing while images come in.
        int gap = std::max(80, static_cast<int>(2 * last_layout_ms_));
        if (now - last_layout_ >= std::chrono::milliseconds(gap)) {
            current_->state->dirty = false;
            if (relayout(false, false)) redraw = true;
            else current_->state->dirty = true; // engine busy parsing another page: try again shortly
        }
    }
    if (refresh_pending_ && current_ && std::chrono::steady_clock::now() >= refresh_due_) {
        refresh_pending_ = false;
        LoadSpec sp;
        sp.url         = unwrap_redirect(current_->refresh_url);
        sp.source_page = current_->display_url; // like following a link from this page
        sp.user_activated = false;
        set_referrer(sp);
        if (current_->refresh_delay < 1) {
            // an instant redirect page replaces itself in the history, so Back skips over it
            if (idx_ >= 0) hist_[static_cast<size_t>(idx_)].url = sp.url;
            start_load(sp, 0);
        } else {
            navigate_spec(sp);
        }
        redraw = true;
    }
    // An animated GIF on screen is due for its next frame. Cleared here and re-armed by draw(), so a tab that is
    // not being drawn (background tab) stops animating instead of waking the loop forever.
    if (anim_pending_ && std::chrono::steady_clock::now() >= anim_due_) {
        anim_pending_ = false;
        redraw        = true;
        auto now      = std::chrono::steady_clock::now();
        for (auto& kv : tiles_)
            if (kv.second.animated && now >= kv.second.anim_due) kv.second.gen = 0; // only strips with a GIF repaint
    }
    {
        auto now = std::chrono::steady_clock::now();
        if (!tiles_.empty() && now - last_draw_ > std::chrono::seconds(30)) { // tab in the background: give the memory back
            tiles_.clear();
            prerender_pending_ = false;
        } else if (!redraw && prerender_pending_ && current_ && current_->doc && idle()) {
            // idle: paint one strip around the screen ahead of time (one per loop turn keeps input responsive)
            int ty = next_prerender_tile();
            if (ty >= 0) tile(ty, current_.get(), now);
            prerender_pending_ = next_prerender_tile() >= 0;
        }
    }
    return redraw;
}

int Browser::next_wake_ms() const {
    int  wake = -1;
    auto now  = std::chrono::steady_clock::now();
    for (auto& f : frames_) {
        int w = f.br->next_wake_ms();
        if (w >= 0 && (wake < 0 || w < wake)) wake = w;
    }
    for (auto& f : iframes_) {
        int w = f.br->next_wake_ms();
        if (w >= 0 && (wake < 0 || w < wake)) wake = w;
    }
    if (current_ && current_->state->dirty.load()) {
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_layout_).count();
        wake    = static_cast<int>(std::max<long long>(1, 80 - el));
    }
    if (prerender_pending_ && idle() && wake != 0) wake = 1;
    if (refresh_pending_) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(refresh_due_ - now).count();
        int  r    = static_cast<int>(std::max<long long>(1, std::min<long long>(left, 60000)));
        if (wake < 0 || r < wake) wake = r;
    }
    if (anim_pending_) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(anim_due_ - now).count();
        int  a    = static_cast<int>(std::max<long long>(1, left));
        if (wake < 0 || a < wake) wake = a;
    }
    return wake;
}

void Browser::install(const std::shared_ptr<LoadResult>& lr) {
    anim_start_    = std::chrono::steady_clock::now();
    anim_pending_  = false;
    hover_pending_ = false;
    state_pending_ = false;
    hover_want_    = nullptr;
    active_node_   = nullptr;
    if (current_) current_->state->cancel = true;
    // The page being left is torn down on a worker once the new one is up: freeing a big DOM, its styles and boxes
    // takes as long as building them did, and the new page should not wait for it.
    std::shared_ptr<Page> old = std::move(current_);
    current_       = lr->page;
    loading_       = false;
    zoom_          = current_->zoom;
    sel_a_ = sel_b_ = engine::TextPoint();
    sel_map_.clear();
    selecting_ = false;
    if (!current_->internal && !parent_ && !is_iframe_) History::instance().add(current_->display_url, current_->title); // top-level pages only
    loading_state_.reset();
    if (idx_ >= 0) {
        hist_[idx_].url = current_->internal ? current_->requested_url : current_->display_url;
        if (!lr->final_post) { // redirected to a GET page (the usual post-redirect-get), or a plain GET
            hist_[idx_].post = false;
            hist_[idx_].body.clear();
        }
    }
    focus_.reset();
    pressed_.reset();
    close_popup();
    press_consumed_ = false;
    hover_url_.clear();
    cursor_ = "auto";
    scroll_ = 0;
    start_favicon(current_->icon_url, current_->display_url);
    iframes_.clear(); // the old page's (relayout below creates the new page's)
    hover_iframe_ = active_iframe_ = press_iframe_ = -1;
    {
        // laid out on the worker at this very size: only map the controls and measure the document
        bool resized = current_->laid_w != vw_ || current_->laid_h != vh_;
        last_layout_ms_ = current_->layout_ms;
        relayout(resized, true, resized);
    }
    focus_.reset();
    for (auto& c : current_->controls) // <input autofocus>: the first one gets the keyboard
        if (c->autofocus && c->box && c->focusable()) {
            focus_ = c;
            if (c->editable()) c->edit.cursor = c->edit.text.size();
            break;
        }
    build_frames();
    // A meta refresh to this very page with no delay would reload forever: only honour self-refreshes that wait.
    if (current_->refresh_delay >= 0 && !(current_->refresh_delay < 1 && strip_fragment(current_->refresh_url) == strip_fragment(current_->display_url))) {
        refresh_pending_ = true;
        refresh_due_     = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(current_->refresh_delay * 1000));
    }
    if (restore_scroll_ > 0) set_scroll(restore_scroll_);
    else {
        ParsedUrl pu;
        if (!current_->internal && parse_url(current_->display_url, pu) && pu.has_fragment && !pu.fragment.empty()) scroll_to_fragment(pu.fragment);
    }
    if (old && doc_pool_) doc_pool_->submit([old]() mutable { old.reset(); }); // (if the pool is stopping, the queue drops it: freed there)
}

bool Browser::relayout(bool restyle, bool blocking, bool layout) {
    if (!current_ || !current_->doc) return false;
    {
        std::unique_lock<std::mutex> lk(engine_mutex(), std::defer_lock);
        if (blocking) lk.lock();
        else if (!lk.try_lock()) return false;
        auto t0 = std::chrono::steady_clock::now();
        if (restyle) current_->doc->resolve_styles(vw_, vh_);
        auto t1 = std::chrono::steady_clock::now();
        // a :hover that only changes colours keeps the layout: repaint only
        if ((layout || restyle) && !(restyle && current_->doc->restyle_was_paint_only() && current_->laid_w == vw_ && current_->laid_h == vh_)) {
            auto tl = std::chrono::steady_clock::now();
            current_->doc->layout(vw_, vh_);
            layout_count_++;
            last_layout_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tl).count();
        }
        if (std::getenv("QUILL_PROF"))
            std::fprintf(stderr, "relayout: styles %.1fms layout %.1fms\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
        current_->laid_w = vw_;
        current_->laid_h = vh_;
    }
    const engine::Box* root = current_->doc->box_root();
    doc_h_ = root ? static_cast<int>(std::ceil(engine::scroll_extent(*root))) : 0;
    current_->by_node.clear();
    for (auto& c : current_->controls) {
        c->box = nullptr;
        current_->by_node[c->node] = c.get();
    }
    if (root) map_boxes(*root);
    last_layout_ = std::chrono::steady_clock::now();
    scroll_      = std::min(scroll_, max_scroll());
    invalidate_tiles();
    has_fixed_ = root && engine::has_fixed_boxes(*root);
    sync_iframes();
    if (sel_a_.valid()) refresh_selection(); // the fragments are new
    return true;
}

void Browser::map_boxes(const engine::Box& b) {
    if (b.node) {
        auto it = current_->by_node.find(b.node);
        if (it != current_->by_node.end() && !it->second->box) it->second->box = &b;
    }
    for (auto& c : b.children) map_boxes(*c);
    for (auto& l : b.lines)
        for (auto& f : l.frags)
            if (f.kind == engine::Frag::Kind::Atomic && f.box && f.box->parent != &b) map_boxes(*f.box);
}

void Browser::scroll_to_fragment(const std::string& frag) {
    if (!current_ || !current_->doc || frag.empty()) return;
    // percent-decode, keep it a plain id
    std::string id;
    for (size_t i = 0; i < frag.size(); i++) {
        if (frag[i] == '%' && i + 2 < frag.size() && std::isxdigit(static_cast<unsigned char>(frag[i + 1])) && std::isxdigit(static_cast<unsigned char>(frag[i + 2]))) {
            id += static_cast<char>(std::stoi(frag.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else id += frag[i];
    }
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return; // selector would not parse
    engine::Rect r;
    std::lock_guard<std::mutex> lk(engine_mutex());
    if (current_->doc->box_rect("#" + id, r)) set_scroll(static_cast<int>(r.y));
}

int Browser::max_scroll() const { return std::max(0, doc_h_ - vh_); }

void Browser::set_scroll(int y) { scroll_ = no_scroll_ ? 0 : std::max(0, std::min(y, max_scroll())); }

void Browser::set_viewport(int w, int h) { apply_viewport(std::max(100, w), std::max(50, h)); }

// A frame can be smaller than a window (a 40px banner row): no minimum size here.
void Browser::apply_viewport(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == vw_ && h == vh_) return;
    vw_ = w;
    vh_ = h;
    // the height matters too: vh units, height:100% pages, fixed boxes at the bottom, (max-height) media queries
    if (current_) relayout(true, true);
    if (!frames_.empty()) layout_frames();
}

void Browser::draw(Framebuffer& fb, const IRect& area) {
    fb.push_clip(area);
    if (!frames_.empty()) {
        // frameset: the gaps get a grey 3D look like the browsers of the time; each frame paints itself
        fb.fill_rect(area, Color(192, 192, 192));
        for (const IRect& g : frame_gaps_) {
            IRect a(area.x + g.x, area.y + g.y, g.w, g.h);
            bool  vertical = g.h > g.w; // a column divider
            if (vertical && a.w >= 2) {
                fb.fill_rect(IRect(a.x, a.y, 1, a.h), Color(240, 240, 240));
                fb.fill_rect(IRect(a.right() - 1, a.y, 1, a.h), Color(128, 128, 128));
            } else if (!vertical && a.h >= 2) {
                fb.fill_rect(IRect(a.x, a.y, a.w, 1), Color(240, 240, 240));
                fb.fill_rect(IRect(a.x, a.bottom() - 1, a.w, 1), Color(128, 128, 128));
            }
        }
        for (auto& f : frames_) {
            IRect a(area.x + f.rect.x, area.y + f.rect.y, f.rect.w, f.rect.h);
            if (!a.empty()) f.br->draw(fb, a);
        }
        anim_pending_ = false; // the frames keep their own animation clocks
        fb.pop_clip();
        return;
    }
    if (current_ && current_->doc && current_->doc->box_root()) {
        Page*              pg = current_.get();
        engine::PaintEnv   env;
        auto now   = std::chrono::steady_clock::now();
        last_draw_ = now;
        if (use_tiles()) {
            draw_tiles(fb, area, pg, now);
        } else {
            env            = make_env(pg);
            int anim_next  = -1;
            env.anim_ms      = std::chrono::duration_cast<std::chrono::milliseconds>(now - anim_start_).count();
            env.anim_next_ms = &anim_next;
            engine::paint_document_area(*pg->doc->box_root(), fb, area, 0, static_cast<float>(scroll_), env);
            anim_pending_ = anim_next >= 0;
            if (anim_pending_) anim_due_ = now + std::chrono::milliseconds(std::max(10, anim_next));
        }

        // Focus ring only for controls the page can't restyle well (checkbox/radio/select); text fields show a caret and
        // buttons/fields get the page's own :focus styling, so a ring there would override designs that set outline:none.
        if (focus_ && (focus_->kind == FormControl::Kind::Checkbox || focus_->kind == FormControl::Kind::Radio ||
                       focus_->kind == FormControl::Kind::Select)) {
            float x, y, w, h;
            if (focus_->border_box(x, y, w, h))
                fb.stroke_round_rect(IRect(area.x + static_cast<int>(std::lround(x)) - 1, area.y + static_cast<int>(std::lround(y)) - scroll_ - 1,
                                           static_cast<int>(std::lround(w)) + 2, static_cast<int>(std::lround(h)) + 2),
                                     3, 3, 3, 3, 2, Color(66, 133, 244));
        }
        IRect pb;
        int   prow = 0, prow_h = 0;
        if (popup_ && popup_geometry(pb, prow, prow_h)) {
            Font* f = FontManager::instance().get("sans-serif", 13.0f, 400, false);
            IRect abs(area.x + pb.x, area.y + pb.y, pb.w, pb.h);
            fb.fill_rect(abs, Color(118, 118, 118));
            fb.fill_rect(IRect(abs.x + 1, abs.y + 1, abs.w - 2, abs.h - 2), Color(255, 255, 255));
            fb.push_clip(abs);
            for (int r = 0; r < prow; r++) {
                int  oi  = popup_first_ + r;
                bool hot = oi == popup_hover_;
                IRect row(abs.x + 1, abs.y + 1 + r * prow_h, abs.w - 2, prow_h);
                if (hot) fb.fill_rect(row, Color(66, 133, 244));
                else if (oi == popup_->selected) fb.fill_rect(row, Color(225, 235, 252));
                if (f) f->draw(fb, popup_->options[static_cast<size_t>(oi)].label, row.x + 6, row.y + (prow_h + f->ascent - f->descent) / 2, hot ? Color(255, 255, 255) : Color(20, 20, 20));
            }
            fb.pop_clip();
        }

        if (doc_h_ > vh_ && area.h > 0 && !no_scroll_) { // overlay scrollbar
            int track = area.h;
            int thumb = std::max(24, static_cast<int>(static_cast<int64_t>(track) * vh_ / doc_h_));
            int ms    = max_scroll();
            int ty    = area.y + (ms > 0 ? static_cast<int>(static_cast<int64_t>(track - thumb) * scroll_ / ms) : 0);
            fb.fill_round_rect(IRect(area.right() - 9, ty, 7, thumb), 3, 3, 3, 3, Color(90, 90, 90, 140));
        }
    } else {
        fb.fill_rect(area, Color(255, 255, 255));
    }
    fb.pop_clip();
}

// What is under a content-area point: the control, the link (href attribute) and the page's cursor style.
namespace {
// Zoom levels (Firefox's) and the zoom chosen per site this session.
const float kZoomSteps[] = {0.3f, 0.5f, 0.67f, 0.8f, 0.9f, 1.f, 1.1f, 1.2f, 1.33f, 1.5f, 1.7f, 2.f, 2.4f, 3.f};
std::mutex& zoom_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, float>& site_zoom() {
    static std::map<std::string, float> m;
    return m;
}
std::string zoom_key(const std::string& url) { // the host; Quill's own pages share one
    ParsedUrl pu;
    if (to_lower(url).compare(0, 6, "about:") == 0 || !parse_url(url, pu) || pu.host.empty()) return "about:";
    return normalize_host(pu.host);
}
} // namespace

float Browser::zoom_for(const std::string& url) const {
    if (parent_ || is_iframe_) { // frames follow the page they are in
        const Browser* t = this;
        while (t->parent_) t = t->parent_;
        return t->zoom_.load();
    }
    std::lock_guard<std::mutex> lk(zoom_mu());
    auto it = site_zoom().find(zoom_key(url));
    return it != site_zoom().end() ? it->second : static_cast<float>(settings_.zoom) / 100.f;
}

void Browser::apply_zoom(float z) {
    if (!current_ || !current_->doc) { zoom_ = z; return; }
    if (std::fabs(current_->zoom - z) < 1e-4f) { zoom_ = z; return; }
    float old = current_->zoom;
    {
        std::lock_guard<std::mutex> lk(engine_mutex());
        current_->doc->set_zoom(z);
    }
    current_->zoom = z;
    zoom_          = z;
    int keep       = scroll_;
    relayout(true, true);
    scroll_ = std::min(max_scroll(), static_cast<int>(std::lround(keep * z / old))); // stay at the same place in the text
    for (auto& f : frames_)
        if (f.br) f.br->apply_zoom(z);
    for (auto& f : iframes_)
        if (f.br) f.br->apply_zoom(z);
    invalidate_tiles();
}

void Browser::step_zoom(int dir) {
    if (parent_) { parent_->step_zoom(dir); return; } // a frame: the whole page zooms
    float cur = zoom_.load(), next = cur;
    const int n = static_cast<int>(sizeof kZoomSteps / sizeof kZoomSteps[0]);
    if (dir > 0) {
        for (int i = 0; i < n; i++) if (kZoomSteps[i] > cur + 0.005f) { next = kZoomSteps[i]; break; }
    } else if (dir < 0) {
        for (int i = n - 1; i >= 0; i--) if (kZoomSteps[i] < cur - 0.005f) { next = kZoomSteps[i]; break; }
    } else next = static_cast<float>(settings_.zoom) / 100.f;
    std::string url = current_ ? current_->display_url : std::string();
    {
        std::lock_guard<std::mutex> lk(zoom_mu());
        if (dir == 0) site_zoom().erase(zoom_key(url));
        else site_zoom()[zoom_key(url)] = next;
    }
    apply_zoom(next);
}

void Browser::zoom_in() { step_zoom(1); }
void Browser::zoom_out() { step_zoom(-1); }
void Browser::zoom_reset() { step_zoom(0); }
void Browser::refresh_zoom() {
    if (!current_ || parent_) return;
    float z = zoom_for(current_->display_url);
    if (std::fabs(z - current_->zoom) > 1e-4f) apply_zoom(z);
}

namespace {
Browser::FilePicker& file_picker() {
    static Browser::FilePicker f;
    return f;
}
bool g_click_ctrl = false, g_click_shift = false;
int  g_click_count = 1;
} // namespace
void Browser::set_click_count(int n) { g_click_count = std::max(1, n); }

const engine::TextIndex& Browser::text_index() {
    const engine::Box* root = current_ && current_->doc ? current_->doc->box_root() : nullptr;
    uint64_t gen = current_ && current_->doc ? current_->doc->box_generation() : 0;
    if (tindex_page_ != current_.get() || tindex_layout_ != layout_count_ || tindex_scroll_ != scroll_ || tindex_bs_ != box_scroll_gen_ ||
        tindex_gen_ != gen) {
        tindex_gen_    = gen;
        tindex_page_   = current_.get();
        tindex_layout_ = layout_count_;
        tindex_scroll_ = scroll_;
        tindex_bs_     = box_scroll_gen_;
        if (root) tindex_.build(*root, static_cast<float>(scroll_), &current_->box_scroll);
        else tindex_ = engine::TextIndex();
    }
    return tindex_;
}

void Browser::refresh_selection() {
    sel_map_.clear();
    if (!sel_a_.valid() || !current_ || !current_->doc) return;
    const engine::TextIndex& ix = text_index();
    uint64_t                 gen = current_->doc->box_generation();
    if (!sel_fresh_ && (gen != sel_gen_ || !ix.contains(sel_a_.box) || !ix.contains(sel_b_.box))) {
        // the page's boxes were rebuilt (a restyle): the same page has its text boxes in the same order
        if (ix.text_boxes() == sel_boxes_ && ix.text_box(sel_ord_a_) && ix.text_box(sel_ord_b_)) {
            sel_a_.box = ix.text_box(sel_ord_a_);
            sel_b_.box = ix.text_box(sel_ord_b_);
        } else {
            sel_a_ = sel_b_ = engine::TextPoint();
            return;
        }
    }
    sel_ord_a_ = ix.ordinal(sel_a_.box);
    sel_ord_b_ = ix.ordinal(sel_b_.box);
    sel_boxes_ = ix.text_boxes();
    sel_gen_   = gen;
    sel_fresh_ = false;
    if (sel_a_ != sel_b_) sel_map_ = ix.ranges(sel_a_, sel_b_);
}

bool Browser::has_selection() const {
    if (!sel_map_.empty()) return true;
    for (auto& f : frames_)
        if (f.br && f.br->has_selection()) return true;
    for (auto& f : iframes_)
        if (f.br && f.br->has_selection()) return true;
    return false;
}

std::string Browser::selected_text() const {
    if (!sel_map_.empty() && tindex_page_ == current_.get()) return tindex_.text(sel_a_, sel_b_);
    for (auto& f : frames_)
        if (f.br && f.br->has_selection()) return f.br->selected_text();
    for (auto& f : iframes_)
        if (f.br && f.br->has_selection()) return f.br->selected_text();
    return {};
}

void Browser::select_all_text() {
    if (Browser* f = key_frame()) { f->select_all_text(); return; }
    if (!current_ || !current_->doc) return;
    const engine::TextIndex& ix = text_index();
    sel_a_     = ix.first();
    sel_b_     = ix.last();
    sel_fresh_ = true;
    refresh_selection();
    invalidate_tiles();
}

void Browser::clear_selection() {
    bool had = !sel_map_.empty();
    sel_a_ = sel_b_ = engine::TextPoint();
    sel_map_.clear();
    selecting_ = false;
    if (had) invalidate_tiles();
}
void Browser::set_file_picker(FilePicker f) { file_picker() = std::move(f); }
void Browser::set_click_modifiers(bool ctrl, bool shift) {
    g_click_ctrl  = ctrl;
    g_click_shift = shift;
}

Browser::Hit Browser::hit_at(int x, int y) const {
    Hit h;
    if (!current_ || !current_->doc || !current_->doc->box_root()) return h;
    const engine::Box* b = engine::hit_test(*current_->doc->box_root(), static_cast<float>(x), static_cast<float>(y + scroll_), static_cast<float>(scroll_), static_cast<float>(vh_), anim_clock_ms(), &current_->box_scroll);
    if (b && b->style) h.cursor = b->style->cursor;
    const engine::Box* label = nullptr;
    int                ix = -1, iy = -1; // point inside the <img> under the pointer
    for (const engine::Box* p = b; p; p = p->logical_parent()) {
        if (!h.node && p->kind == engine::Box::Kind::Element && p->node) h.node = p->node;
        if (p->kind == engine::Box::Kind::Element && p->tag == "img" && ix < 0) {
            float lx = static_cast<float>(x) - (p->x + p->border.left + p->padding.left);
            float ly = static_cast<float>(y + scroll_) - (p->y + p->border.top + p->padding.top);
            ix = std::max(0, static_cast<int>(lx));
            iy = std::max(0, static_cast<int>(ly));
            const std::string* um = p->attr("usemap");
            if (um && !h.link && !um->empty()) { // client-side image map: the <area> under the pointer is the link
                std::string name = (*um)[0] == '#' ? um->substr(1) : *um;
                const std::vector<engine::MapArea>* areas = nullptr;
                for (auto& m : current_->maps)
                    if (m.first == name) { areas = &m.second; break; }
                if (!areas)
                    for (auto& m : current_->maps)
                        if (to_lower(m.first) == to_lower(name)) { areas = &m.second; break; }
                if (areas)
                    for (auto& a : *areas)
                        if (a.contains(lx, ly)) {
                            if (!a.nohref && !a.href.empty()) {
                                h.link   = &a.href;
                                h.target = a.target.empty() ? nullptr : &a.target;
                            }
                            break;
                        }
            }
            if (!p->attr("ismap")) ix = iy = -2; // only an ismap image sends its coordinates
        }
        if (!label && p->kind == engine::Box::Kind::Element && p->tag == "label") label = p;
        if (!h.ctl && p->node) {
            auto it = current_->by_node.find(p->node);
            if (it != current_->by_node.end() && it->second->kind != FormControl::Kind::Hidden) h.ctl = it->second;
        }
        if (!h.link && p->kind == engine::Box::Kind::Element && p->tag == "a") {
            const std::string* href = p->attr("href");
            if (href && !href->empty()) {
                h.link   = href;
                h.target = p->attr("target");
                h.rel    = p->attr("rel");
                if (ix >= 0) { h.ismap_x = ix; h.ismap_y = iy; }
            }
        }
    }
    if (label && !h.ctl && !h.link) { // clicking a label activates its control
        const std::string* target = label->attr("for");
        for (auto& c : current_->controls) {
            if (c->kind == FormControl::Kind::Hidden || !c->focusable()) continue;
            if (target) {
                if (!c->id.empty() && c->id == *target) { h.label_ctl = c.get(); break; }
            } else {
                for (const engine::Box* p = c->box; p; p = p->logical_parent())
                    if (p == label) { h.label_ctl = c.get(); break; }
                if (h.label_ctl) break;
            }
        }
    }
    return h;
}

Browser::ContextInfo Browser::context_at(int x, int y) const {
    if (!frames_.empty()) {
        int i = frame_at(x, y);
        if (i < 0) return {};
        const Frame& f = frames_[static_cast<size_t>(i)];
        return f.br->context_at(x - f.rect.x, y - f.rect.y);
    }
    if (int i = iframe_at(x, y); i >= 0) {
        const InlineFrame& f = iframes_[static_cast<size_t>(i)];
        return f.br->context_at(x - f.rect.x, y + scroll_ - f.rect.y);
    }
    ContextInfo ci;
    if (!current_ || !current_->doc || !current_->doc->box_root()) return ci;
    Hit h = hit_at(x, y);
    if (h.link) ci.link = resolve_href(*h.link);
    if (h.ctl) {
        ci.control  = true;
        ci.editable = h.ctl->editable();
    }
    const engine::Box* b = engine::hit_test(*current_->doc->box_root(), static_cast<float>(x), static_cast<float>(y + scroll_), static_cast<float>(scroll_), static_cast<float>(vh_), anim_clock_ms(), &current_->box_scroll);
    for (const engine::Box* p = b; p; p = p->logical_parent()) {
        if (p->kind == engine::Box::Kind::Element && p->tag == "img") {
            const std::string* src = p->attr("src");
            if (src && !src->empty() && src->compare(0, 5, "data:") != 0) ci.image = resolve_href(*src);
            break;
        }
    }
    return ci;
}

std::string Browser::resolve_href(const std::string& href) const {
    if (!current_) return href;
    std::string r = resolve_url(current_->base_url.empty() ? current_->display_url : current_->base_url, href);
    return unwrap_redirect(r); // search engines' click-tracking links go straight to the result
}

bool Browser::mouse_move(int x, int y) {
    if (!frames_.empty()) {
        int  i = frame_at(x, y);
        bool r = false;
        if (i != hover_frame_) {
            if (hover_frame_ >= 0) r |= frames_[static_cast<size_t>(hover_frame_)].br->mouse_leave();
            hover_frame_ = i;
        }
        int t = press_frame_ >= 0 ? press_frame_ : i; // a drag stays with the frame it started in
        if (t >= 0) {
            Frame& f = frames_[static_cast<size_t>(t)];
            r |= f.br->mouse_move(x - f.rect.x, y - f.rect.y);
        }
        return r;
    }
    mouse_x_ = x;
    mouse_y_ = y;
    if (!current_ || !current_->doc) return false;
    if (drag_.on) return scroll_drag_move(y);
    if (!popup_) { // over an <iframe> (or dragging in one): its page handles the mouse
        int i = iframe_at(x, y);
        if (i != hover_iframe_) {
            bool r = hover_iframe_ >= 0 && iframes_[static_cast<size_t>(hover_iframe_)].br->mouse_leave();
            if (r) invalidate_doc_rect(iframes_[static_cast<size_t>(hover_iframe_)].rect);
            hover_iframe_ = i;
        }
        int t = press_iframe_ >= 0 ? press_iframe_ : i;
        if (t >= 0) {
            InlineFrame& f = iframes_[static_cast<size_t>(t)];
            bool         r = f.br->mouse_move(x - f.rect.x, y + scroll_ - f.rect.y);
            if (r) invalidate_doc_rect(f.rect);
            hover_url_.clear();
            hover_want_ = f.node;
            push_state();
            return r;
        }
    }
    if (selecting_ && !popup_) { // dragging out a selection (scrolls when the mouse leaves the page)
        if (y < 0) set_scroll(scroll_ - std::min(60, -y));
        else if (y > vh_) set_scroll(scroll_ + std::min(60, y - vh_));
        engine::TextPoint p = text_index().at(static_cast<float>(x), static_cast<float>(y + scroll_));
        if (p.valid() && p != sel_b_) {
            sel_b_     = p;
            sel_fresh_ = true;
            refresh_selection();
            invalidate_tiles();
        }
        cursor_ = "text";
        return true;
    }
    bool popup_redraw = false;
    if (popup_) {
        IRect pb;
        int   rows = 0, rh = 0;
        if (popup_geometry(pb, rows, rh) && pb.contains(x, y)) {
            int h = popup_first_ + (y - pb.y - 1) / rh;
            if (h != popup_hover_) {
                popup_hover_ = h;
                popup_redraw = true;
            }
        }
    }
    Hit h = hit_at(x, y);
    if (current_->doc->has_state_rules() && !popup_) {
        hover_want_ = h.node;
        push_state();
        if (hover_pending_ && relayout(true, false)) { hover_pending_ = false; popup_redraw = true; h = hit_at(x, y); }
    }
    if (h.link && !starts_with(to_lower(*h.link), "javascript:")) hover_url_ = resolve_href(*h.link);
    else hover_url_.clear();
    if (!h.ctl && h.label_ctl) cursor_ = h.cursor == 2 ? "text" : "auto";
    else if (h.ctl && h.ctl->focusable()) cursor_ = h.ctl->editable() ? "text" : "pointer";
    else if (h.cursor == 1 || (h.link && h.cursor == 0)) cursor_ = "pointer";
    else if (h.cursor == 2) cursor_ = "text";
    else if (h.cursor == 0 && !h.link) { // over the page's text: the text cursor (it can be selected)
        bool exact = false;
        text_index().at(static_cast<float>(x), static_cast<float>(y + scroll_), &exact);
        cursor_ = exact ? "text" : "auto";
    } else cursor_ = "auto";
    return popup_redraw;
}

bool Browser::mouse_down_core(int x, int y) {
    if (!current_ || !current_->doc) return false;
    press_consumed_ = false;
    down_link_.clear();
    down_node_ = nullptr;
    if (popup_) { // a dropdown is open: this click picks an option or dismisses it
        IRect pb;
        int   rows = 0, rh = 0;
        if (popup_geometry(pb, rows, rh) && pb.contains(x, y)) {
            int oi = popup_first_ + (y - pb.y - 1) / rh;
            if (oi >= 0 && oi < static_cast<int>(popup_->options.size())) popup_->select_option(oi);
        }
        close_popup();
        press_consumed_ = true;
        return true;
    }
    Hit  h   = hit_at(x, y);
    auto ctl = std::shared_ptr<FormControl>();
    FormControl* hit_ctl = h.ctl ? h.ctl : h.label_ctl;
    if (hit_ctl)
        for (auto& c : current_->controls)
            if (c.get() == hit_ctl) ctl = c;
    if (ctl && ctl->focusable()) {
        press_consumed_ = true;
        pressed_        = ctl;
        focus_          = ctl;
        if (!h.ctl) { // via a label: no caret placement, just focus (caret at the end)
            if (ctl->editable()) { ctl->edit.select_all = false; ctl->edit.cursor = ctl->edit.text.size(); }
            return true;
        }
        if (ctl->kind == FormControl::Kind::Select && ctl->listbox) { // a list box picks on press, no popup
            int oi = ctl->listbox_option_at(static_cast<float>(y + scroll_));
            if (oi >= 0) ctl->listbox_click(oi, g_click_ctrl, g_click_shift);
            pressed_.reset();
            invalidate_tiles();
            return true;
        }
        if (ctl->editable()) {
            float cx, cy, cw, ch;
            if (ctl->content_box(cx, cy, cw, ch)) ctl->set_caret_from_point(static_cast<float>(x) - cx, static_cast<float>(y + scroll_) - cy);
        }
        return true;
    }
    bool had_focus = static_cast<bool>(focus_);
    focus_.reset();
    pressed_.reset();
    if (h.link) {
        down_link_ = *h.link;
        preconnect_link(resolve_href(*h.link));
    }
    down_node_ = h.node;
    // text selection: a press off links starts one (shift extends the one there is; 2 clicks a word, 3 a block)
    bool had_sel = !sel_map_.empty();
    if (!h.link || g_click_count > 1) {
        const engine::TextIndex& ix = text_index();
        engine::TextPoint        p  = ix.at(static_cast<float>(x), static_cast<float>(y + scroll_));
        if (p.valid()) {
            if (g_click_count >= 3) {
                ix.block_at(p, sel_a_, sel_b_);
                selecting_ = false;
            } else if (g_click_count == 2) {
                ix.word_at(p, sel_a_, sel_b_);
                selecting_ = false;
                down_link_.clear(); // a double-click on a link selects its word rather than following it twice
            } else if (g_click_shift && sel_a_.valid()) {
                sel_b_     = p;
                selecting_ = true;
            } else {
                sel_a_ = sel_b_ = p;
                selecting_ = true;
            }
            sel_fresh_ = true; // just picked from the current boxes
            refresh_selection();
        } else if (had_sel) {
            clear_selection();
        }
    } else if (had_sel) {
        clear_selection();
    }
    if (had_sel || !sel_map_.empty()) invalidate_tiles();
    return had_focus || had_sel || !sel_map_.empty();
}

bool Browser::mouse_up_core(int x, int y) {
    if (!current_ || !current_->doc) return false;
    if (selecting_) {
        selecting_ = false;
        if (sel_a_ == sel_b_) sel_map_.clear(); // a click, not a drag
        if (!sel_map_.empty()) down_link_.clear();  // a drag that ended on a link doesn't follow it
    }
    if (press_consumed_) {
        press_consumed_ = false;
        Hit  h          = hit_at(x, y);
        auto pressed    = pressed_;
        pressed_.reset();
        if (pressed && (h.ctl ? h.ctl : h.label_ctl) == pressed.get()) activate_control(pressed);
        return true;
    }
    std::string down = std::move(down_link_);
    down_link_.clear();
    Hit h = hit_at(x, y);
    if (!down.empty() && h.link && *h.link == down) {
        std::string l = to_lower(down);
        if (starts_with(l, "javascript:")) return false;
        if (down[0] == '#') {
            if (down.size() == 1) set_scroll(0);
            else scroll_to_fragment(down.substr(1));
            return true;
        }
        std::string url = resolve_href(down);
        if (h.ismap_x >= 0) url = strip_fragment(url) + "?" + std::to_string(h.ismap_x) + "," + std::to_string(h.ismap_y);
        bool noref = false;
        if (h.rel) {
            std::string r = " " + to_lower(*h.rel) + " ";
            for (char& c : r) if (c == '\t' || c == '\n') c = ' ';
            noref = r.find(" noreferrer ") != std::string::npos;
        }
        follow_link(url, h.target ? *h.target : current_->base_target, noref);
        return true;
    }
    const void* dn = down_node_;
    down_node_     = nullptr;
    if (down.empty() && dn && dn == h.node) { // a click on a <details> summary opens / closes it
        bool toggled;
        {
            std::lock_guard<std::mutex> lk(engine_mutex());
            toggled = current_->doc->toggle_details(h.node);
        }
        if (toggled) {
            relayout(true, true);
            return true;
        }
    }
    return false;
}

// :hover, :focus and :active live in the document's selector state. Changing it reworks libcss / libdom data whose
// interned strings are shared by every document, so only under the engine lock; when a worker holds it (parsing a
// page, or freeing the one left behind) the change waits for poll() instead of stalling the mouse.
bool Browser::push_state() {
    if (!current_ || !current_->doc || !current_->doc->has_state_rules()) {
        state_pending_ = false;
        return false;
    }
    std::unique_lock<std::mutex> lk(engine_mutex(), std::try_to_lock);
    if (!lk.owns_lock()) {
        state_pending_ = true;
        return false;
    }
    state_pending_ = false;
    bool changed = current_->doc->set_hover(hover_want_);
    changed |= current_->doc->set_focus(focus_ ? focus_->node : nullptr);
    changed |= current_->doc->set_active(active_node_);
    if (changed) hover_pending_ = true;
    return changed;
}

// :focus and :active: keep them in step with what the UI thinks.
bool Browser::restate() {
    if (!current_ || !current_->doc || !current_->doc->has_state_rules()) return false;
    push_state();
    if (hover_pending_ && relayout(true, false)) { hover_pending_ = false; return true; }
    return false;
}

bool Browser::mouse_down(int x, int y) {
    if (!frames_.empty()) {
        int  i = frame_at(x, y);
        bool r = false;
        if (i != active_frame_ && active_frame_ >= 0) { // clicking into another frame takes the keyboard focus there
            frames_[static_cast<size_t>(active_frame_)].br->form_blur();
            r = true;
        }
        press_frame_ = i;
        if (i < 0) return r;
        active_frame_ = i;
        Frame& f = frames_[static_cast<size_t>(i)];
        return f.br->mouse_down(x - f.rect.x, y - f.rect.y) || r;
    }
    if (scroll_drag_start(x, y)) return true;
    if (!popup_) {
        int i = iframe_at(x, y);
        if (i != active_iframe_ && active_iframe_ >= 0) { // focus moves out of the frame it was in
            InlineFrame& f = iframes_[static_cast<size_t>(active_iframe_)];
            f.br->form_blur();
            invalidate_doc_rect(f.rect);
        }
        active_iframe_ = press_iframe_ = i;
        if (i >= 0) {
            bool had_focus = static_cast<bool>(focus_);
            form_blur_core();
            InlineFrame& f = iframes_[static_cast<size_t>(i)];
            bool         r = f.br->mouse_down(x - f.rect.x, y + scroll_ - f.rect.y);
            invalidate_tiles();
            return r || had_focus;
        }
    }
    bool r = mouse_down_core(x, y);
    if (current_ && current_->doc && current_->doc->has_state_rules()) {
        active_node_ = hit_at(x, y).node;
        r |= restate();
    }
    if (r) invalidate_tiles();
    return r;
}

bool Browser::mouse_up(int x, int y) {
    if (!frames_.empty()) {
        if (press_frame_ < 0 || press_frame_ >= static_cast<int>(frames_.size())) return false;
        Frame& f = frames_[static_cast<size_t>(press_frame_)];
        press_frame_ = -1;
        return f.br->mouse_up(x - f.rect.x, y - f.rect.y); // may navigate another frame: keep no references after
    }
    if (drag_.on) {
        drag_.on = false;
        return true;
    }
    if (press_iframe_ >= 0 && press_iframe_ < static_cast<int>(iframes_.size())) {
        Browser* c  = iframes_[static_cast<size_t>(press_iframe_)].br.get();
        IRect    rc = iframes_[static_cast<size_t>(press_iframe_)].rect;
        press_iframe_ = -1;
        bool r = c->mouse_up(x - rc.x, y + scroll_ - rc.y); // may navigate this page: no references kept after
        invalidate_tiles();
        return r;
    }
    press_iframe_ = -1;
    bool r = mouse_up_core(x, y);
    active_node_ = nullptr;
    r |= restate();
    if (r) invalidate_tiles(); // a click can change what the page shows (checkbox, select, focus)
    return r;
}

bool Browser::form_key(FormKey k) {
    if (Browser* f = key_frame()) {
        bool r = f->form_key(k);
        if (r) invalidate_tiles(); // an <iframe>'s page is painted into this page's strips
        return r;
    }
    bool r = form_key_core(k);
    if (r) {
        restate();
        invalidate_tiles();
    }
    return r;
}

void Browser::form_blur() {
    if (Browser* f = key_frame()) { f->form_blur(); invalidate_tiles(); return; }
    form_blur_core();
    restate();
    invalidate_tiles();
}

bool Browser::mouse_leave() {
    if (!frames_.empty()) {
        bool r = hover_frame_ >= 0 && frames_[static_cast<size_t>(hover_frame_)].br->mouse_leave();
        hover_frame_ = -1;
        return r;
    }
    mouse_x_ = mouse_y_ = -1;
    bool changed = !hover_url_.empty();
    if (hover_iframe_ >= 0) {
        InlineFrame& f = iframes_[static_cast<size_t>(hover_iframe_)];
        if (f.br->mouse_leave()) { invalidate_doc_rect(f.rect); changed = true; }
        hover_iframe_ = -1;
    }
    hover_url_.clear();
    hover_want_ = nullptr;
    push_state();
    if (hover_pending_ && relayout(true, false)) { hover_pending_ = false; changed = true; }
    cursor_ = "auto";
    return changed;
}

std::string Browser::title() const { return current_ ? current_->title : std::string(); }

std::shared_ptr<const Image> Browser::favicon() const {
    std::lock_guard<std::mutex> lk(icon_->mu);
    return icon_->img;
}

Browser::Security Browser::security() const {
    Security s;
    if (!current_) return s;
    if (current_->internal) {
        s.level = Security::Level::Internal;
        return s;
    }
    ParsedUrl pu;
    if (!parse_url(current_->display_url, pu)) return s;
    s.host = display_host(pu.host);
    bool lan = net_.policy().is_private_host(pu.host);
    if (pu.scheme == "https") s.level = lan && net_.trusts_cert(pu.host) ? Security::Level::TrustedKey : Security::Level::Secure;
    else if (pu.scheme == "http") s.level = lan ? Security::Level::LocalHttp : Security::Level::Insecure;
    s.hsts = pu.scheme == "https" && Hsts::instance().must_upgrade(pu.host);
    return s;
}

std::string Browser::url_for_bar() const {
    if (loading_) return display_url(loading_url_);
    return current_ ? display_url(current_->display_url) : std::string(); // lookalike hosts shown as xn--
}

std::string Browser::status() const {
    if (!frames_.empty()) {
        if (hover_frame_ >= 0) {
            std::string s = frames_[static_cast<size_t>(hover_frame_)].br->status();
            if (!s.empty() && !starts_with(s, "Not secure")) return s;
        }
        if (loading_) return "Loading " + loading_url_ + " ...";
        for (auto& f : frames_)
            if (f.br->loading()) return f.br->status();
    }
    if (Browser* c = hover_child()) {
        std::string st = c->status();
        if (!st.empty()) return st;
    }
    if (!hover_url_.empty()) return display_url(hover_url_);
    if (loading_) return "Loading " + display_url(loading_url_) + " ...";
    return notice_;
}

std::string Browser::hover_link() const {
    if (!frames_.empty()) return hover_frame_ >= 0 ? frames_[static_cast<size_t>(hover_frame_)].br->hover_link() : std::string();
    if (Browser* c = hover_child()) return c->hover_link();
    return hover_url_;
}

std::string Browser::cursor() const {
    if (!frames_.empty()) return hover_frame_ >= 0 ? frames_[static_cast<size_t>(hover_frame_)].br->cursor() : std::string("auto");
    if (Browser* c = hover_child()) return c->cursor();
    return current_ ? cursor_ : std::string("auto");
}

bool Browser::loading() const {
    if (loading_) return true;
    for (auto& f : frames_)
        if (f.br->loading()) return true;
    for (auto& f : iframes_)
        if (f.br->loading()) return true;
    return false;
}

bool Browser::idle() const {
    for (auto& f : frames_)
        if (!f.br->idle()) return false;
    for (auto& f : iframes_)
        if (!f.br->idle()) return false;
    if (loading_) return false;
    if (!current_) return true;
    return current_->state->pending_images.load() == 0 && !current_->state->dirty.load();
}

// ------------------------------------------------------------------ page forms
std::vector<std::shared_ptr<FormControl>> Browser::live_controls() const {
    if (!current_) return {};
    return current_->controls;
}

bool Browser::form_wants_text() const {
    if (Browser* f = key_frame()) return f->form_wants_text();
    return focus_ && focus_->editable();
}

bool Browser::form_focused() const {
    if (Browser* f = key_frame()) return f->form_focused();
    return static_cast<bool>(focus_);
}

void Browser::form_blur_core() {
    focus_.reset();
    pressed_.reset();
    close_popup();
}

void Browser::focus_control(const std::shared_ptr<FormControl>& c, bool scroll_into_view) {
    focus_ = c;
    if (!c) return;
    if (c->editable()) c->edit.select_all = true; // like tabbing into a field in other browsers
    if (scroll_into_view) {
        float x, y, w, h;
        if (c->border_box(x, y, w, h)) {
            if (y < scroll_ || y + h > scroll_ + vh_) set_scroll(static_cast<int>(y) - vh_ / 3);
        }
    }
}

void Browser::focus_next(bool back) {
    std::vector<std::shared_ptr<FormControl>> order;
    for (auto& c : live_controls()) {
        float x, y, w, h;
        if (c->focusable() && c->border_box(x, y, w, h)) order.push_back(c);
    }
    if (order.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < order.size(); i++)
        if (order[i] == focus_) cur = static_cast<int>(i);
    int n = static_cast<int>(order.size());
    int next = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    focus_control(order[static_cast<size_t>(next)], true);
}

void Browser::activate_control(const std::shared_ptr<FormControl>& c) {
    using K = FormControl::Kind;
    switch (c->kind) {
    case K::Checkbox: c->checked = !c->checked; break;
    case K::Radio:
        if (!c->checked) {
            const void* form = c->form;
            for (auto& o : live_controls())
                if (o->kind == K::Radio && o->name == c->name && !c->name.empty() && o->form == form) o->checked = false;
            c->checked = true;
        }
        break;
    case K::Submit: submit_form(c, c); break;
    case K::Reset: {
        const void* form = c->form;
        for (auto& o : live_controls())
            if (o->form == form) o->reset();
        break;
    }
    case K::File: {
        auto& pick = file_picker();
        if (!pick || c->disabled) break;
        std::vector<std::string> got = pick(c->multiple, c->accept);
        if (!got.empty()) {
            if (!c->multiple) got.resize(1);
            c->files = std::move(got);
        }
        break;
    }
    case K::Select:
        if (c->listbox) break;
        if (!c->options.empty()) {
            popup_       = c;
            popup_first_ = 0;
            popup_hover_ = c->selected;
            IRect pb;
            int   rows = 0, rh = 0;
            if (popup_geometry(pb, rows, rh) && c->selected >= rows) popup_first_ = c->selected - rows + 1;
        }
        break;
    default: break;
    }
}

bool Browser::popup_geometry(IRect& box, int& rows, int& row_h) const {
    if (!popup_) return false;
    float x, y, w, h;
    if (!popup_->border_box(x, y, w, h)) return false;
    row_h = 20;
    rows  = std::min<int>(static_cast<int>(popup_->options.size()) - popup_first_, 15);
    if (rows < 1) return false;
    Font* f   = FontManager::instance().get("sans-serif", 13.0f, 400, false);
    float mw  = w;
    if (f)
        for (auto& o : popup_->options) mw = std::max(mw, static_cast<float>(f->text_width(o.label)) + 16.0f);
    int bh = rows * row_h + 2;
    int bx = static_cast<int>(x), by = static_cast<int>(y + h) - scroll_;
    if (by + bh > vh_ && static_cast<int>(y) - scroll_ - bh >= 0) by = static_cast<int>(y) - scroll_ - bh; // flip above
    box = IRect(bx, by, static_cast<int>(mw), bh);
    return true;
}

void Browser::submit_form(const std::shared_ptr<FormControl>& ctl, const std::shared_ptr<FormControl>& button) {
    using K = FormControl::Kind;
    const void* form = ctl->form;
    if (!form || !current_) return;
    const engine::FormInfo* fi = nullptr;
    for (auto& f : current_->forms)
        if (f.node == form) fi = &f;
    if (!fi) return;

    FormFields fields;
    std::vector<FormFile> files; // <input type=file> entries, each placed before fields[at]
    for (auto& c : live_controls()) {
        if (c->form != form || c->disabled || c->name.empty()) continue;
        switch (c->kind) {
        case K::Checkbox:
        case K::Radio:
            if (c->checked) fields.emplace_back(c->name, c->submit_value());
            break;
        case K::Submit:
            if (c == button) fields.emplace_back(c->name, c->initial_value.empty() && !c->is_button_element ? "Submit" : c->initial_value);
            break;
        case K::Reset:
        case K::Button: break;
        case K::File:
            if (c->files.empty()) files.push_back({c->name, std::string(), fields.size()});
            for (auto& p : c->files) files.push_back({c->name, p, fields.size()});
            break;
        case K::Select:
            for (auto& v : c->submit_values()) fields.emplace_back(c->name, v);
            break;
        default: fields.emplace_back(c->name, c->submit_value()); break;
        }
    }

    // the sign-in prompt (Quill's own page): keep the username and password for that site and load it again
    if (current_->internal && current_->auth_retry && fi->action == "quill:auth") {
        HttpCredentials c;
        for (auto& f : fields) {
            if (f.first == "user") c.user = f.second;
            if (f.first == "pass") c.pass = f.second;
        }
        c.digest = current_->auth_scheme == "digest";
        c.realm  = current_->auth_realm;
        CredentialStore::instance().set(current_->auth_origin, c);
        LoadSpec sp = *current_->auth_retry;
        start_load(sp, 0);
        return;
    }

    std::string method   = to_lower(fi->method);
    std::string page_url = current_->internal ? std::string() : current_->display_url;
    std::string url;
    if (!fi->action.empty()) url = resolve_href(fi->action);
    else url = page_url;
    if (url.empty()) return;
    ParsedUrl pu;
    if (!parse_url(url, pu)) return;

    LoadSpec sp;
    sp.form_submit = true;
    sp.initiator   = page_url;
    set_referrer(sp);
    bool multipart = method == "post" && to_lower(fi->enctype) == "multipart/form-data";
    std::string data, ctype = "application/x-www-form-urlencoded";
    if (multipart) {
        std::string err;
        if (!form_multipart(fields, files, data, ctype, &err)) {
            notice_ = err;
            return;
        }
    } else {
        // without multipart only the file names go (what browsers do)
        FormFields all;
        size_t     fi2 = 0;
        for (size_t i = 0; i <= fields.size(); i++) {
            for (; fi2 < files.size() && files[fi2].at == i; fi2++) {
                std::string p = files[fi2].path;
                size_t      sl = p.find_last_of("/\\");
                all.emplace_back(files[fi2].name, sl == std::string::npos ? p : p.substr(sl + 1));
            }
            if (i < fields.size()) all.push_back(fields[i]);
        }
        data = form_urlencode(all);
    }
    if (method == "post") {
        sp.method       = "POST";
        sp.body         = data;
        sp.content_type = ctype;
        pu.fragment.clear();
        pu.has_fragment = false;
        sp.url          = serialize_url(pu);
    } else {
        pu.query        = data;
        pu.has_query    = !data.empty();
        pu.fragment.clear();
        pu.has_fragment = false;
        sp.url          = serialize_url(pu);
    }
    if (!page_url.empty()) sp.source_page = page_url;
    navigate_target(fi->target.empty() ? current_->base_target : fi->target, sp);
}

bool Browser::form_text(const std::string& utf8) {
    if (Browser* f = key_frame()) {
        bool r = f->form_text(utf8);
        if (r) invalidate_tiles();
        return r;
    }
    if (!focus_ || !focus_->editable()) return false;
    std::string t = utf8;
    if (focus_->maxlength > 0) {
        size_t room = focus_->maxlength > static_cast<int>(focus_->edit.text.size()) ? static_cast<size_t>(focus_->maxlength) - focus_->edit.text.size() : 0;
        if (t.size() > room && !focus_->edit.select_all) t.resize(room);
    }
    focus_->edit.insert(t);
    invalidate_tiles();
    return true;
}

std::string Browser::form_selected_text() const {
    if (Browser* f = key_frame()) return f->form_selected_text();
    if (focus_ && focus_->editable() && focus_->kind != FormControl::Kind::Password && focus_->edit.select_all) return focus_->edit.text;
    return {};
}

bool Browser::form_key_core(FormKey k) {
    using K = FormControl::Kind;
    if (!focus_) {
        if (k == FormKey::Tab || k == FormKey::ShiftTab) {
            focus_next(k == FormKey::ShiftTab);
            return static_cast<bool>(focus_);
        }
        return false;
    }
    auto c = focus_;

    if (popup_) { // dropdown open
        int n = static_cast<int>(popup_->options.size());
        switch (k) {
        case FormKey::Up: popup_hover_ = std::max(0, (popup_hover_ < 0 ? popup_->selected : popup_hover_) - 1); break;
        case FormKey::Down: popup_hover_ = std::min(n - 1, (popup_hover_ < 0 ? popup_->selected : popup_hover_) + 1); break;
        case FormKey::Enter:
        case FormKey::Space:
            if (popup_hover_ >= 0) popup_->select_option(popup_hover_);
            close_popup();
            return true;
        case FormKey::Escape:
        case FormKey::Tab:
        case FormKey::ShiftTab: close_popup(); return true;
        default: return true;
        }
        if (popup_hover_ < popup_first_) popup_first_ = popup_hover_;
        if (popup_hover_ >= popup_first_ + 15) popup_first_ = popup_hover_ - 14;
        return true;
    }

    if (k == FormKey::Tab) { focus_next(false); return true; }
    if (k == FormKey::ShiftTab) { focus_next(true); return true; }
    if (k == FormKey::Escape) { focus_.reset(); return true; }

    if (c->editable()) {
        TextEdit& e = c->edit;
        switch (k) {
        case FormKey::Backspace: e.backspace(); return true;
        case FormKey::Delete: e.del(); return true;
        case FormKey::Left: e.left(); return true;
        case FormKey::Right: e.right(); return true;
        case FormKey::Home: e.home(); return true;
        case FormKey::End: e.end(); return true;
        case FormKey::SelectAll: e.select_all = true; e.cursor = e.text.size(); return true;
        case FormKey::Up:
            if (c->kind == K::Textarea) e.up();
            else e.home(); // single-line field: jump to the start, like other browsers
            return true;
        case FormKey::Down:
            if (c->kind == K::Textarea) e.down();
            else e.end();
            return true;
        case FormKey::Enter:
            // A one-row textarea (search boxes) behaves like a text field: Enter submits.
            if (c->kind == K::Textarea && c->rows > 1) { e.insert("\n"); return true; }
            {
                std::shared_ptr<FormControl> def;
                const void* form = c->form;
                for (auto& o : live_controls())
                    if (o->kind == K::Submit && !o->disabled && o->form == form) { def = o; break; }
                submit_form(c, def);
            }
            return true;
        default: return false;
        }
    }
    switch (c->kind) {
    case K::Checkbox:
    case K::Radio:
        if (k == FormKey::Space) { activate_control(c); return true; }
        return false;
    case K::Submit:
    case K::Reset:
    case K::Button:
        if (k == FormKey::Space || k == FormKey::Enter) { activate_control(c); return true; }
        return false;
    case K::File:
        if (k == FormKey::Space || k == FormKey::Enter) { activate_control(c); return true; }
        return false;
    case K::Select: {
        int n = static_cast<int>(c->options.size());
        if (c->listbox) {
            if (k == FormKey::Space && c->multiple && c->selected >= 0) { c->listbox_click(c->selected, true, false); return true; }
            if (k == FormKey::Up && n > 0) { c->select_option(std::max(0, c->selected - 1)); return true; }
            if (k == FormKey::Down && n > 0) { c->select_option(std::min(n - 1, c->selected + 1)); return true; }
            if (k == FormKey::Home && n > 0) { c->select_option(0); return true; }
            if (k == FormKey::End && n > 0) { c->select_option(n - 1); return true; }
            return false;
        }
        if (k == FormKey::Space || k == FormKey::Enter) { activate_control(c); return true; }
        if (k == FormKey::Up && n > 0) { c->select_option(std::max(0, c->selected - 1)); return true; }
        if (k == FormKey::Down && n > 0) { c->select_option(std::min(n - 1, c->selected + 1)); return true; }
        return false;
    }
    default: return false;
    }
}
// ------------------------------------------------------------------ public entry points that frames redirect
void Browser::navigate_link(const std::string& url, const std::string& source_page) {
    if (source_page.empty()) {
        navigate_url(url, true);
        return;
    }
    if (starts_with(to_lower(url), "quill:")) return; // never from another tab
    LoadSpec sp;
    sp.url         = url;
    sp.source_page = source_page;
    navigate_spec(sp);
}

std::string Browser::link_source() const {
    if (!frames_.empty()) {
        int i = hover_frame_ >= 0 ? hover_frame_ : active_frame_;
        return i >= 0 ? frames_[static_cast<size_t>(i)].br->link_source() : std::string();
    }
    if (Browser* c = hover_child()) return c->link_source();
    return current_ && !current_->internal ? current_->display_url : std::string();
}

void Browser::scroll_by(int dy) {
    if (Browser* f = scroll_frame()) { f->scroll_by(dy); return; }
    if (Browser* c = hover_child()) { // the wheel over an <iframe> scrolls it while it can move that way
        int before = c->scroll_y();
        c->scroll_by(dy);
        if (c->scroll_y() != before) {
            invalidate_doc_rect(iframes_[static_cast<size_t>(hover_iframe_)].rect);
            return;
        }
    }
    if (current_ && current_->doc && mouse_x_ >= 0) { // the wheel over a list box with more options than rows scrolls the list
        Hit h = hit_at(mouse_x_, mouse_y_);
        if (h.ctl && h.ctl->kind == FormControl::Kind::Select && h.ctl->listbox) {
            int rows = dy > 0 ? std::max(1, dy / 40) : std::min(-1, dy / 40);
            if (h.ctl->listbox_scroll(rows)) { invalidate_tiles(); return; }
        }
        if (scroll_box(0, static_cast<float>(dy))) return;
    }
    set_scroll(scroll_ + dy);
}

void Browser::scroll_by_x(int dx) {
    if (Browser* f = scroll_frame()) { f->scroll_by_x(dx); return; }
    if (Browser* c = hover_child()) { c->scroll_by_x(dx); invalidate_doc_rect(iframes_[static_cast<size_t>(hover_iframe_)].rect); return; }
    if (current_ && current_->doc && mouse_x_ >= 0) scroll_box(static_cast<float>(dx), 0);
}

bool Browser::scroll_box(float dx, float dy) {
    if (!current_ || !current_->doc || !current_->doc->box_root() || mouse_x_ < 0) return false;
    const engine::Box* b = engine::hit_test(*current_->doc->box_root(), static_cast<float>(mouse_x_), static_cast<float>(mouse_y_ + scroll_),
                                            static_cast<float>(scroll_), static_cast<float>(vh_), anim_clock_ms(), &current_->box_scroll);
    for (const engine::Box* p = b; p; p = p->parent) {
        if (!p->scrollable() || !p->node) continue;
        float ox, oy;
        engine::box_scroll_of(*p, &current_->box_scroll, ox, oy);
        float nx = std::max(0.f, std::min(p->scroll_max_x, ox + dx)), ny = std::max(0.f, std::min(p->scroll_max_y, oy + dy));
        if (nx == ox && ny == oy) continue; // at its end: the next one out scrolls instead
        current_->box_scroll[p->node] = {nx, ny};
        box_scroll_gen_++;
        invalidate_tiles();
        return true;
    }
    return false;
}

// A press on a scroll bar: the page's (the strip along the right edge) or a scroll container's.
bool Browser::scroll_drag_start(int x, int y) {
    drag_.on = false;
    if (!current_ || !current_->doc || !current_->doc->box_root() || popup_) return false;
    if (x >= vw_ - 10 && doc_h_ > vh_ && !no_scroll_) {
        int track = vh_, thumb = std::max(24, static_cast<int>(static_cast<int64_t>(vh_) * vh_ / doc_h_));
        int ms    = max_scroll();
        int ty    = ms > 0 ? static_cast<int>(static_cast<int64_t>(track - thumb) * scroll_ / ms) : 0;
        drag_.per_px = track > thumb ? static_cast<float>(ms) / static_cast<float>(track - thumb) : 0;
        if (y < ty || y >= ty + thumb) { // on the track: the thumb jumps there
            set_scroll(static_cast<int>((y - thumb / 2) * drag_.per_px));
            invalidate_tiles();
        }
        drag_.on = true;
        drag_.node = nullptr;
        drag_.start_y = y;
        drag_.start_scroll = static_cast<float>(scroll_);
        return true;
    }
    const engine::Box* b = engine::hit_test(*current_->doc->box_root(), static_cast<float>(x), static_cast<float>(y + scroll_), static_cast<float>(scroll_),
                                            static_cast<float>(vh_), anim_clock_ms(), &current_->box_scroll);
    for (const engine::Box* p = b; p; p = p->parent) {
        if (!p->node || p->scroll_max_y <= 0) continue;
        float right = p->x + p->w - p->border.right, top = p->y + p->border.top, h = p->h - p->border.top - p->border.bottom;
        float dy = static_cast<float>(y + scroll_);
        if (static_cast<float>(x) < right - 12 || static_cast<float>(x) >= right || dy < top || dy >= top + h) return false;
        float ox, oy;
        engine::box_scroll_of(*p, &current_->box_scroll, ox, oy);
        float all = h + p->scroll_max_y, th = std::max(12.f, h * h / all);
        drag_.per_px = h > th ? p->scroll_max_y / (h - th) : 0;
        float ty = top + (h - th) * (oy / p->scroll_max_y);
        if (dy < ty || dy >= ty + th) {
            oy = std::max(0.f, std::min(p->scroll_max_y, (dy - top - th / 2) * drag_.per_px));
            current_->box_scroll[p->node] = {ox, oy};
            box_scroll_gen_++;
            invalidate_tiles();
        }
        drag_.on = true;
        drag_.node = p->node;
        drag_.start_y = y;
        drag_.start_scroll = oy;
        return true;
    }
    return false;
}

bool Browser::scroll_drag_move(int y) {
    if (!drag_.on || !current_) return false;
    float v = drag_.start_scroll + static_cast<float>(y - drag_.start_y) * drag_.per_px;
    if (!drag_.node) {
        set_scroll(static_cast<int>(std::lround(v)));
        return true;
    }
    auto it = current_->box_scroll.find(drag_.node);
    float ox = it == current_->box_scroll.end() ? 0.f : it->second.first;
    current_->box_scroll[drag_.node] = {ox, std::max(0.f, v)}; // clamped when used
    box_scroll_gen_++;
    invalidate_tiles();
    return true;
}
void Browser::scroll_to(int y) {
    if (Browser* f = scroll_frame()) f->scroll_to(y);
    else set_scroll(y);
}
int Browser::scroll_y() const {
    if (Browser* f = scroll_frame()) return f->scroll_y();
    return scroll_;
}
int Browser::content_height() const {
    if (Browser* f = scroll_frame()) return f->content_height();
    return doc_h_;
}
int Browser::viewport_h() const {
    if (Browser* f = scroll_frame()) return f->viewport_h();
    return vh_;
}

// ------------------------------------------------------------------ frames
namespace {
// One rows= / cols= list ("150,*,2*,20%") resolved to pixel sizes that add up to `total`, the way the browsers of the
// time did it: fixed pixels first, then percentages, then the rest shared by the '*' weights; with no '*' the
// sizes are stretched or shrunk to fill.
std::vector<int> frame_sizes(const std::string& spec, int total) {
    struct It { int kind; double v; }; // 0 px, 1 percent, 2 relative weight
    std::vector<It> items;
    size_t pos = 0;
    while (pos <= spec.size() && items.size() < 100) {
        size_t      comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        std::string t = trim(spec.substr(pos, comma - pos));
        pos           = comma + 1;
        if (t.empty()) { if (comma >= spec.size()) break; continue; }
        char*  end = nullptr;
        double v   = std::strtod(t.c_str(), &end);
        bool   num = end != t.c_str();
        if (t.back() == '*') items.push_back({2, num && v > 0 ? v : 1.0});
        else if (t.back() == '%' && num) items.push_back({1, std::max(0.0, v)});
        else if (num) items.push_back({0, std::max(0.0, v)});
        else items.push_back({2, 1.0});
    }
    if (items.empty()) items.push_back({2, 1.0});
    total = std::max(0, total);
    std::vector<double> sz(items.size(), 0.0);
    double used = 0, weights = 0;
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == 0) sz[i] = items[i].v;
        else if (items[i].kind == 1) sz[i] = total * items[i].v / 100.0;
        else weights += items[i].v;
        if (items[i].kind != 2) used += sz[i];
    }
    if (weights > 0) {
        double rest = total - used;
        if (rest < 0) { // fixed parts overflow: shrink them, the '*' parts get nothing
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].kind != 2 && used > 0) sz[i] = sz[i] * total / used;
            rest = 0;
        }
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].kind == 2) sz[i] = rest * items[i].v / weights;
    } else if (used > 0) {
        for (auto& v : sz) v = v * total / used;
    }
    std::vector<int> out(items.size());
    double acc = 0;
    int    prev = 0;
    for (size_t i = 0; i < items.size(); i++) { // round so the parts add up exactly
        acc += sz[i];
        int edge = i + 1 == items.size() ? total : static_cast<int>(std::lround(acc));
        out[i]   = std::max(0, edge - prev);
        prev     = edge;
    }
    return out;
}
constexpr int kFrameBorder = 4; // default frame border / spacing in px
constexpr int kMaxFrameDepth = 4; // framesets inside frames inside framesets ... (also stops a frameset loading itself forever)
} // namespace

void Browser::walk_frames(const engine::FrameNode& n, IRect r, int border, bool show_border, size_t& idx, bool create) {
    if (!n.is_set) {
        if (create) {
            Frame f;
            f.name = n.name;
            f.src  = n.src;
            f.br.reset(new Browser(this, n.name, n.scrolling));
            frames_.push_back(std::move(f));
        }
        if (idx < frames_.size()) frames_[idx].rect = r;
        idx++;
        return;
    }
    int  bw   = n.border >= 0 ? n.border : border;
    bool show = show_border && n.frameborder;
    int  gap  = show ? bw : 0;
    std::vector<int> rows0 = frame_sizes(n.rows, 0), cols0 = frame_sizes(n.cols, 0); // just the counts
    int  nrows = static_cast<int>(rows0.size()), ncols = static_cast<int>(cols0.size());
    std::vector<int> hs = frame_sizes(n.rows, r.h - gap * (nrows - 1));
    std::vector<int> ws = frame_sizes(n.cols, r.w - gap * (ncols - 1));
    size_t k = 0;
    int    y = r.y;
    for (int row = 0; row < nrows; row++) {
        int x = r.x, h = hs[static_cast<size_t>(row)];
        for (int col = 0; col < ncols; col++) {
            int w = ws[static_cast<size_t>(col)];
            if (k < n.children.size()) walk_frames(n.children[k], IRect(x, y, w, h), bw, show, idx, create);
            k++;
            if (col + 1 < ncols && gap > 0) frame_gaps_.push_back(IRect(x + w, y, gap, h));
            x += w + gap;
        }
        if (row + 1 < nrows && gap > 0) frame_gaps_.push_back(IRect(r.x, y + h, r.w, gap));
        y += h + gap;
    }
}

void Browser::layout_frames() {
    if (!current_ || !current_->frameset) return;
    frame_gaps_.clear();
    size_t idx = 0;
    walk_frames(*current_->frameset, IRect(0, 0, vw_, vh_), kFrameBorder, true, idx, false);
    for (auto& f : frames_) f.br->apply_viewport(std::max(1, f.rect.w), std::max(1, f.rect.h));
}

void Browser::build_frames() {
    frames_.clear();
    frame_gaps_.clear();
    hover_frame_ = active_frame_ = press_frame_ = -1;
    if (!current_ || !current_->frameset) return;
    int depth = 0;
    for (Browser* b = parent_; b; b = b->parent_) depth++;
    if (depth >= kMaxFrameDepth) return;
    size_t idx = 0;
    walk_frames(*current_->frameset, IRect(0, 0, vw_, vh_), kFrameBorder, true, idx, true);
    if (frames_.empty()) return;
    layout_frames();

    // URLs: what the frames showed when we were last on this history entry (back / reload), else their src
    std::vector<std::string> urls;
    bool restore = idx_ >= 0 && hist_[static_cast<size_t>(idx_)].frames.size() == frames_.size();
    for (size_t i = 0; i < frames_.size(); i++) {
        std::string u = restore ? hist_[static_cast<size_t>(idx_)].frames[i]
                                : frames_[i].src.empty() ? std::string("about:blank") : resolve_href(frames_[i].src);
        urls.push_back(u);
    }
    for (size_t i = 0; i < frames_.size(); i++) frames_[i].br->load_frame(urls[i], current_->display_url);
    if (idx_ >= 0) hist_[static_cast<size_t>(idx_)].frames = urls;

    long best = -1; // keys and the wheel go to the biggest frame (the "main" one) until something is clicked
    for (size_t i = 0; i < frames_.size(); i++) {
        long a = static_cast<long>(frames_[i].rect.w) * frames_[i].rect.h;
        if (a > best) { best = a; active_frame_ = static_cast<int>(i); }
    }
}

void Browser::load_frame(const std::string& url, const std::string& source_page) {
    hist_.clear();
    HistEntry e;
    e.url = url;
    hist_.push_back(e);
    idx_ = 0;
    LoadSpec sp;
    sp.url         = url;
    sp.source_page = source_page;
    start_load(sp, 0);
}

std::string Browser::frame_url() const {
    if (loading_) return loading_url_;
    if (!current_) return {};
    return current_->internal ? current_->requested_url : current_->display_url;
}

void Browser::frame_navigated() {
    if (frames_.empty() || idx_ < 0) return;
    std::vector<std::string> urls;
    for (auto& f : frames_) urls.push_back(f.br->frame_url());
    HistEntry& cur = hist_[static_cast<size_t>(idx_)];
    if (urls == cur.frames) return;
    if (parent_) { // a frameset inside a frame: only the outermost page keeps history
        cur.frames = urls;
        return;
    }
    cur.scroll = scroll_;
    HistEntry e = cur;
    e.frames    = urls;
    e.scroll    = 0;
    if (idx_ + 1 < static_cast<int>(hist_.size())) hist_.resize(static_cast<size_t>(idx_) + 1);
    hist_.push_back(e);
    idx_ = static_cast<int>(hist_.size()) - 1;
}

bool Browser::go_to_entry(int from, int to) {
    if (frames_.empty() || !current_ || from < 0 || to < 0) return false;
    const HistEntry& a = hist_[static_cast<size_t>(from)];
    const HistEntry& b = hist_[static_cast<size_t>(to)];
    if (a.url != b.url || b.frames.size() != frames_.size()) return false;
    for (size_t i = 0; i < frames_.size(); i++)
        if (frames_[i].br->frame_url() != b.frames[i]) frames_[i].br->load_frame(b.frames[i], current_->display_url);
    return true;
}

int Browser::frame_at(int x, int y) const {
    for (size_t i = 0; i < frames_.size(); i++)
        if (frames_[i].rect.contains(x, y)) return static_cast<int>(i);
    return -1;
}

Browser* Browser::scroll_frame() const {
    if (frames_.empty()) return nullptr;
    int i = hover_frame_ >= 0 ? hover_frame_ : active_frame_ >= 0 ? active_frame_ : 0;
    return frames_[static_cast<size_t>(i)].br.get();
}

Browser* Browser::key_frame() const {
    if (frames_.empty()) {
        if (active_iframe_ >= 0 && active_iframe_ < static_cast<int>(iframes_.size())) return iframes_[static_cast<size_t>(active_iframe_)].br.get();
        return nullptr;
    }
    if (active_frame_ < 0) return nullptr;
    return frames_[static_cast<size_t>(active_frame_)].br.get();
}

Browser* Browser::find_frame(const std::string& name) {
    for (auto& f : frames_) {
        if (!name.empty() && f.name == name) return f.br.get();
        if (Browser* b = f.br->find_frame(name)) return b;
    }
    for (auto& f : iframes_) {
        if (!name.empty() && f.br->frame_name_ == name) return f.br.get();
        if (Browser* b = f.br->find_frame(name)) return b;
    }
    return nullptr;
}

Browser* Browser::resolve_target(const std::string& target) {
    std::string t = to_lower(trim(target));
    if (t.empty() || t == "_self") return this;
    if (t == "_top") return top();
    if (t == "_parent") return parent_ ? parent_ : this;
    if (t == "_blank" || t == "_new") return nullptr; // a new window: a new tab
    if (Browser* f = top()->find_frame(trim(target))) return f; // frame names are case-sensitive
    return nullptr; // a window name no frame has: browsers open a new window with that name
}

// Loads `sp` where `target` says: a frame, this page, or (target=_blank, unknown names) a new tab.
void Browser::navigate_target(const std::string& target, const LoadSpec& sp) {
    if (Browser* b = resolve_target(target)) {
        b->navigate_spec(sp);
        return;
    }
    Browser* t = top();
    if (!t->open_tab_) { // nobody to open tabs (headless): the whole tab navigates
        t->navigate_spec(sp);
        return;
    }
    NewTab r;
    r.url          = sp.url;
    r.method       = sp.method;
    r.body         = sp.body;
    r.content_type = sp.content_type;
    r.source_page  = sp.source_page;
    r.initiator    = sp.initiator;
    r.form_submit  = sp.form_submit;
    r.referrer        = sp.referrer;
    r.referrer_policy = sp.referrer_policy;
    r.no_referrer     = sp.no_referrer;
    t->open_tab_(r);
}

void Browser::open_request(const NewTab& r) {
    if (starts_with(to_lower(r.url), "quill:")) return;
    LoadSpec sp;
    sp.url          = r.url;
    sp.method       = r.method == "POST" ? "POST" : "GET";
    sp.body         = r.body;
    sp.content_type = r.content_type;
    sp.source_page  = r.source_page;
    sp.initiator    = r.initiator;
    sp.form_submit  = r.form_submit;
    sp.referrer        = r.referrer;
    sp.referrer_policy = r.referrer_policy;
    sp.no_referrer     = r.no_referrer;
    navigate_spec(sp);
}

void Browser::follow_link(const std::string& url, const std::string& target, bool no_referrer) {
    if (starts_with(to_lower(url), "quill:")) { // Quill's own pages: never targeted
        navigate_url(url, true);
        return;
    }
    LoadSpec sp;
    sp.url = url;
    if (current_ && !current_->internal) sp.source_page = current_->display_url;
    set_referrer(sp);
    sp.no_referrer = no_referrer;
    navigate_target(target, sp);
}

// Pressing a link to another host starts the DNS lookup and connection while the button is still down (a click
// takes 100 ms or so), as browsers do. Only http(s), only to hosts this page isn't already talking to, one at a time.
void Browser::preconnect_link(const std::string& url) {
    static std::atomic<int> in_flight{0};
    if (!settings_.preconnect || !current_ || in_flight.load() > 0) return;
    ParsedUrl to, from;
    if (!parse_url(url, to) || (to.scheme != "https" && to.scheme != "http")) return;
    if (parse_url(current_->display_url, from) && normalize_host(from.host) == normalize_host(to.host)) return;
    auto net = net_owner_;
    in_flight++;
    std::thread([net, url] {
        net->preconnect(url);
        in_flight--;
    }).detach();
}

void Browser::set_referrer(LoadSpec& sp) const {
    if (!current_ || current_->internal) return; // Quill's own pages never refer
    sp.referrer = current_->display_url;
    if (current_->state) {
        std::lock_guard<std::mutex> lk(current_->state->mu);
        sp.referrer_policy = current_->state->referrer_policy;
    }
}

// ------------------------------------------------------------------ inline frames
namespace {
constexpr size_t kMaxIframes = 16; // per page; the rest stay empty boxes (ad-heavy pages embed dozens)

struct FoundFrame {
    const void* node;
    std::string src, srcdoc, name;
    int         scrolling = 0; // 2 = scrolling="no"
    bool        has_srcdoc = false;
    IRect       rect;
};

void find_iframes(const engine::Box& b, std::vector<FoundFrame>& out) {
    if (b.skipped) return;
    if (b.replaced == engine::ReplacedKind::Iframe && b.node && b.style && b.style->visibility == engine::Visibility::Visible) {
        float cx = b.x + b.border.left + b.padding.left, cy = b.y + b.border.top + b.padding.top;
        float cw = b.w - b.border.left - b.border.right - b.padding.left - b.padding.right;
        float ch = b.h - b.border.top - b.border.bottom - b.padding.top - b.padding.bottom;
        if (cw >= 1 && ch >= 1) {
            FoundFrame f;
            f.node = b.node;
            if (const std::string* v = b.attr("src")) f.src = trim(*v);
            if (const std::string* v = b.attr("srcdoc")) { f.srcdoc = *v; f.has_srcdoc = true; }
            if (const std::string* v = b.attr("name")) f.name = *v;
            if (const std::string* v = b.attr("scrolling")) f.scrolling = to_lower(trim(*v)) == "no" ? 2 : 0;
            f.rect = IRect(static_cast<int>(std::lround(cx)), static_cast<int>(std::lround(cy)), static_cast<int>(std::lround(cw)),
                           static_cast<int>(std::lround(ch)));
            out.push_back(std::move(f));
        }
        return;
    }
    for (auto& c : b.children) find_iframes(*c, out);
    for (auto& l : b.lines)
        for (auto& fr : l.frags)
            if (fr.kind == engine::Frag::Kind::Atomic && fr.box && fr.box->parent != &b) find_iframes(*fr.box, out);
}

// May a page with these response headers be shown inside a frame of `parent_url`? (X-Frame-Options, CSP
// frame-ancestors: sites use them so another page can't wrap them in a frame and trick clicks onto them.)
bool framing_allowed(const std::string& xfo, const std::string& csp, const std::string& page_url, const std::string& parent_url) {
    ParsedUrl pg, pa;
    bool      parsed = parse_url(page_url, pg) && parse_url(parent_url, pa);
    auto      same_origin = [&] {
        return parsed && pg.scheme == pa.scheme && normalize_host(pg.host) == normalize_host(pa.host) && pg.port == pa.port;
    };
    std::string csp_l = to_lower(csp);
    size_t      fa    = csp_l.find("frame-ancestors");
    if (fa != std::string::npos) { // takes precedence over X-Frame-Options
        size_t      end  = csp_l.find_first_of(";,", fa);
        std::string list = trim(csp_l.substr(fa + 15, end == std::string::npos ? std::string::npos : end - fa - 15));
        if (list.find("'none'") != std::string::npos || list.empty()) return false;
        std::istringstream in(list);
        std::string        src;
        std::string        ph = parsed ? normalize_host(pa.host) : std::string();
        while (in >> src) {
            if (src == "*") return true;
            if (src == "'self'") { if (same_origin()) return true; continue; }
            if (src == "https:" || src == "http:") { if (parsed && pa.scheme + ":" == src) return true; continue; }
            std::string h = src;
            size_t      sch = h.find("://");
            if (sch != std::string::npos) h = h.substr(sch + 3);
            size_t slash = h.find('/');
            if (slash != std::string::npos) h.erase(slash);
            size_t colon = h.rfind(':');
            if (colon != std::string::npos && h.find(']') == std::string::npos) h.erase(colon);
            if (h.compare(0, 2, "*.") == 0) {
                std::string d = h.substr(1); // ".example.com"
                if (ph.size() > d.size() && ph.compare(ph.size() - d.size(), d.size(), d) == 0) return true;
            } else if (!h.empty() && h == ph) {
                return true;
            }
        }
        return false;
    }
    std::string x = to_lower(trim(xfo));
    if (x.empty()) return true;
    if (x == "deny") return false;
    if (x == "sameorigin") return same_origin();
    return false; // allow-from and anything unknown: treated as deny, as browsers do now
}

std::string framing_refused_html(const std::string& url) {
    return "<!DOCTYPE html><html><head><title>" + html_escape(url) +
           "</title><style>body{margin:0;font:13px sans-serif;color:#5b5b66;background:#f0f0f4;display:flex;align-items:center;"
           "justify-content:center;height:100vh;text-align:center}div{padding:12px}a{color:#0061e0}</style></head><body><div>"
           "This page doesn't allow being shown inside another page.<br><a href=\"" + html_escape(url) +
           "\" target=\"_blank\">Open it in a new tab</a></div></body></html>";
}
} // namespace

int Browser::iframe_at(int x, int y) const {
    for (size_t i = 0; i < iframes_.size(); i++)
        if (iframes_[i].rect.contains(x, y + scroll_)) return static_cast<int>(i);
    return -1;
}

void Browser::invalidate_doc_rect(const IRect& r) {
    if (tiles_.empty()) return;
    int t0 = std::max(0, r.y) / kTileH, t1 = std::max(0, r.bottom()) / kTileH;
    for (auto& kv : tiles_)
        if (kv.first >= t0 && kv.first <= t1) kv.second.gen = 0;
}

void Browser::sync_iframes() {
    std::vector<FoundFrame> found;
    int depth = 0;
    for (Browser* b = parent_; b; b = b->parent_) depth++;
    const engine::Box* root = current_ && current_->doc ? current_->doc->box_root() : nullptr;
    if (root && depth < kMaxFrameDepth && !current_->internal) find_iframes(*root, found);
    if (found.size() > kMaxIframes) found.resize(kMaxIframes);

    std::vector<InlineFrame> next;
    bool                     same = found.size() == iframes_.size();
    for (size_t i = 0; i < found.size(); i++) {
        FoundFrame& f = found[i];
        InlineFrame fr;
        for (auto& old : iframes_)
            if (old.node == f.node && old.br) { fr = std::move(old); break; }
        if (!same || i >= iframes_.size() || fr.node != f.node) same = false;
        fr.rect = f.rect;
        if (!fr.br) {
            fr.node = f.node;
            fr.br.reset(new Browser(this, f.name, f.scrolling));
            fr.br->is_iframe_                 = true;
            fr.br->settings_.render_cache_mb = 0; // painted into this page's strips: no cache of its own
            fr.br->apply_viewport(f.rect.w, f.rect.h);
            LoadSpec sp;
            sp.embedded  = true;
            sp.initiator = current_->display_url;
            set_referrer(sp);
            if (f.has_srcdoc) {
                sp.url    = "about:srcdoc";
                sp.srcdoc = f.srcdoc;
            } else {
                std::string u = f.src.empty() ? std::string("about:blank") : resolve_href(f.src);
                std::string l = to_lower(u);
                if (!(starts_with(l, "http://") || starts_with(l, "https://") || l == "about:blank")) u = "about:blank";
                sp.url = u;
            }
            fr.src = sp.url;
            fr.br->hist_.clear();
            HistEntry e;
            e.url = sp.url;
            fr.br->hist_.push_back(e);
            fr.br->idx_ = 0;
            fr.br->start_load(sp, 0);
        } else {
            fr.br->apply_viewport(f.rect.w, f.rect.h);
        }
        next.push_back(std::move(fr));
    }
    iframes_ = std::move(next); // frames no longer on the page are destroyed here (their loads are cancelled)
    if (!same) hover_iframe_ = active_iframe_ = press_iframe_ = -1;
}

// ------------------------------------------------------------------ rendered-page cache
size_t Browser::tile_budget() const {
    size_t bytes = static_cast<size_t>(settings_.render_cache_mb) * 1024 * 1024;
    if (parent_) bytes /= 4; // a frame shares the window with its siblings
    size_t per = static_cast<size_t>(std::max(1, vw_)) * kTileH * sizeof(uint32_t);
    return std::max<size_t>(4, bytes / per);
}

engine::PaintEnv Browser::make_env(Page* pg) {
    engine::PaintEnv env;
    env.zoom         = pg->zoom;
    env.selection    = pg == current_.get() && !sel_map_.empty() ? &sel_map_ : nullptr;
    env.box_scroll   = &pg->box_scroll;
    env.image        = [pg](const std::string& src) { return pg->images->get(src); };
    env.image_broken = [pg](const std::string& src) { return pg->images->broken(src); };
    env.control      = [this, pg](const engine::Box& b, Framebuffer& f, int ox, int oy) {
        auto it = pg->by_node.find(b.node);
        if (it == pg->by_node.end() || it->second->box != &b) return false;
        return it->second->paint(f, ox, oy, focus_.get() == it->second);
    };
    env.checked = [pg](const engine::Box& b) {
        auto it = pg->by_node.find(b.node);
        return it == pg->by_node.end() ? -1 : (it->second->checked ? 1 : 0);
    };
    env.frame = [this](const engine::Box& b, Framebuffer& f, const IRect& r) {
        for (auto& fr : iframes_)
            if (fr.node == b.node) {
                fr.br->draw(f, r);
                return;
            }
    };
    return env;
}

Browser::Tile& Browser::tile(int ty, Page* pg, std::chrono::steady_clock::time_point now) {
    Tile& t = tiles_[ty];
    if (t.fb && t.gen == paint_gen_ && t.fb->w == vw_) return t;
    if (!t.fb) t.fb.reset(new Framebuffer());
    if (t.fb->w != vw_ || t.fb->h != kTileH) t.fb->resize(vw_, kTileH);
    engine::PaintEnv env = make_env(pg);
    env.skip_fixed       = has_fixed_;
    int anim_next        = -1;
    env.anim_ms          = std::chrono::duration_cast<std::chrono::milliseconds>(now - anim_start_).count();
    env.anim_next_ms     = &anim_next;
    engine::paint_document_area(*pg->doc->box_root(), *t.fb, IRect(0, 0, vw_, kTileH), 0, static_cast<float>(ty) * kTileH, env);
    t.gen      = paint_gen_;
    t.animated = anim_next >= 0;
    if (t.animated) t.anim_due = now + std::chrono::milliseconds(std::max(10, anim_next));
    return t;
}

void Browser::draw_tiles(Framebuffer& fb, const IRect& area, Page* pg,
                         std::chrono::steady_clock::time_point now) {
    if (tile_w_ != vw_ || tile_page_ != pg) { // new page or new width: the strips are useless
        tiles_.clear();
        tile_w_    = vw_;
        tile_page_ = pg;
    }
    IRect dst = area.intersect(fb.clip()).intersect(IRect(0, 0, fb.w, fb.h));
    bool  anim = false;
    std::chrono::steady_clock::time_point due{};
    int top = scroll_, bottom = scroll_ + area.h;
    for (int ty = std::max(0, top) / kTileH; ty * kTileH < bottom; ++ty) {
        Tile& t = tile(ty, pg, now);
        if (t.animated) {
            if (!anim || t.anim_due < due) due = t.anim_due;
            anim = true;
        }
        // copy the strip's visible rows (a memcpy per row: this is what scrolling costs now)
        for (int sy = 0; sy < kTileH; ++sy) {
            int y = area.y + ty * kTileH + sy - scroll_;
            if (y < dst.y || y >= dst.bottom()) continue;
            int x0 = dst.x, x1 = std::min(dst.right(), area.x + t.fb->w);
            if (x1 <= x0) continue;
            std::copy_n(&t.fb->px[static_cast<size_t>(sy) * t.fb->w + (x0 - area.x)], x1 - x0, &fb.px[static_cast<size_t>(y) * fb.w + x0]);
        }
    }
    if (has_fixed_) { // position:fixed boxes stay put while the page scrolls: painted live on top of the strips
        engine::PaintEnv env = make_env(pg);
        int anim_next        = -1;
        env.anim_ms          = std::chrono::duration_cast<std::chrono::milliseconds>(now - anim_start_).count();
        env.anim_next_ms     = &anim_next;
        engine::paint_fixed_layer(*pg->doc->box_root(), fb, area, 0, static_cast<float>(scroll_), env);
        if (anim_next >= 0) {
            auto d = now + std::chrono::milliseconds(std::max(10, anim_next));
            if (!anim || d < due) due = d;
            anim = true;
        }
    }
    anim_pending_ = anim;
    anim_due_     = due;
    evict_tiles();
    prerender_pending_ = settings_.prerender && next_prerender_tile() >= 0;
}

int Browser::next_prerender_tile() const {
    if (!current_ || !current_->doc) return -1;
    int    first = std::max(0, scroll_) / kTileH, last = std::max(first, (scroll_ + vh_ - 1) / kTileH);
    int    max_t = std::max(0, (std::max(doc_h_, vh_) - 1) / kTileH);
    size_t room  = tile_budget();
    size_t want  = static_cast<size_t>(last - first + 1);
    // nearest first, below before above (people mostly scroll down), never more than fits in the budget
    for (int d = 1; want < room && (last + d <= max_t || first - d >= 0); ++d) {
        for (int ty : {last + d, first - d}) {
            if (ty < 0 || ty > max_t) continue;
            if (++want > room) return -1;
            auto it = tiles_.find(ty);
            if (it == tiles_.end() || (it->second.gen != paint_gen_ && !it->second.animated)) return ty;
        }
    }
    return -1;
}

void Browser::evict_tiles() {
    size_t budget = tile_budget();
    if (tiles_.size() <= budget) return;
    int center = (scroll_ + vh_ / 2) / kTileH;
    std::vector<int> keys;
    for (auto& kv : tiles_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end(), [center](int a, int b) { return std::abs(a - center) > std::abs(b - center); });
    for (int k : keys) {
        if (tiles_.size() <= budget) break;
        tiles_.erase(k);
    }
}

} // namespace quill
