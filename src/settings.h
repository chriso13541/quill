#pragma once
#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace quill {

struct Settings {
    std::string search_url = "https://html.duckduckgo.com/html/?q=%s"; // %s = percent-encoded query
    std::string home_url   = "about:home";
    // DNS-over-HTTPS endpoint. Use an IP-literal URL so looking up the DoH server
    // itself does not hit your system resolver. Empty = use the system resolver.
    std::string doh_url      = "https://9.9.9.9/dns-query";
    bool        doh_fallback = false; // fall back to system DNS if DoH lookup fails
    // Referer: "strict" (default; pages may send less, never more than their full address to their own origin and
    // just the origin to other https sites), "origin" (only ever the origin), "off" (never), "full" (whatever the
    // page's Referrer-Policy says).
    std::string referrer     = "strict";
    // Keep stylesheets, pictures and fonts in memory as their Cache-Control allows (and revalidate them), up to this many MB.
    bool        http_cache   = true;
    int         http_cache_mb = 64;
    // Look up and connect to the search engine at startup, and to a link's host while it is being clicked.
    bool        preconnect   = true;
    std::string user_agent   = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
    std::string proxy;        // empty = curl's environment defaults
    std::string font_dir;     // extra directory to search for fonts

    std::set<std::string> http_allow;    // hosts allowed over plain http
    // When opening a public http:// page shows the "not encrypted" warning first:
    //   "typed"  (default) typed / bookmarked addresses, and https pages sending you to http; links followed from a
    //            page that is already plain http open directly (like Firefox).
    //   "always" every public http page whose host you haven't allowed, even from links.
    //   "never"  never (http pages still show "Not secure" in the status line).
    std::string http_warning = "typed";
    std::set<std::string> private_allow; // extra names treated as private ("host" or "*.suffix")
    // Certificates you chose to trust for private hosts: host -> "sha256//<base64 of public key hash>".
    std::map<std::string, std::string> trusted_certs;
    bool cookies = true; // first-party, session-only cookie jar (see cookies.h)

    size_t max_page_bytes  = 16u << 20;
    size_t max_image_bytes = 24u << 20;
    long   timeout_ms      = 30000;
    int    zoom            = 100; // default page zoom, percent (Ctrl +/- change it per site, Ctrl+0 goes back to this)
    int    image_threads   = 4;
    // Rendered-page cache per tab, in MB: scrolling over already painted parts is a memory copy. 0 = paint every frame.
    int    render_cache_mb = 128;
    // While idle, paint the parts of the page above / below the screen into that cache ahead of time.
    bool   prerender       = true;
    // Folder of <name>.svg icons replacing the built-in ones (back, forward, reload, close, add, home, shield-check,
    // lock, lock-open, key, info, warning, search), e.g. from Material Symbols or Noto. Empty = built-in.
    std::string icon_dir;
    // Shortcut tiles on the start page: "Name|https://url, Name|https://url, ..." (empty = the built-in set).
    std::string home_links;
    // Web fonts (@font-face): downloaded, checked and rebuilt by fontsan before use. false = system fonts only.
    bool        web_fonts = true;
    // Site icons in the tab strip (<link rel=icon>, else /favicon.ico).
    bool        favicons = true;
    // Downloaded fonts and site icons are kept on disk (in cache_dir) between sessions. Note that this leaves a record
    // of which sites were visited (file names are hashed, but anyone can hash a site name and check). false = memory only.
    bool        disk_cache = true;
    std::string cache_dir; // empty = default_cache_dir()
    // Browsing history (about:history, Ctrl+H), kept in history_file between sessions; entries older than
    // history_days are dropped. false = nothing is recorded (and :visited only lasts the session).
    bool        history = true;
    int         history_days = 90;
    std::string history_file; // empty = default_history_path()
    int    window_w        = 1100;
    int    window_h        = 800;
};

// Reads "key = value" lines ('#' comments). Missing file is not an error.
// On a parse problem, *error describes it (settings parsed so far are kept).
Settings    load_settings(const std::string& path, std::string* error);
std::string default_config_path();
// $XDG_CACHE_HOME/quill or ~/.cache/quill (Linux), %LOCALAPPDATA%\quill\cache (Windows).
std::string default_cache_dir();

} // namespace quill
