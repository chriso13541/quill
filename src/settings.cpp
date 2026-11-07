#include "settings.h"

#include <algorithm>

#include "certpin.h"
#include "policy.h"
#include "url.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace quill {

std::string default_config_path() {
    if (const char* e = std::getenv("QUILL_CONFIG")) return e;
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\quill\\config.ini";
    return "quill.ini";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) return std::string(x) + "/quill/config.ini";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/quill/config.ini";
    return "quill.ini";
#endif
}

std::string default_cache_dir() {
#ifdef _WIN32
    if (const char* a = std::getenv("LOCALAPPDATA")) return std::string(a) + "\\quill\\cache";
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\quill\\cache";
    return "quill-cache";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/quill";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.cache/quill";
    return "";
#endif
}

static void add_list(std::set<std::string>& dst, const std::string& value) {
    std::stringstream ss(value);
    std::string       item;
    while (std::getline(ss, item, ',')) {
        item = normalize_host(trim(item));
        if (!item.empty()) dst.insert(item);
    }
}

static bool parse_bool(const std::string& v) {
    std::string l = to_lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

Settings load_settings(const std::string& path, std::string* error) {
    Settings s;
    std::ifstream f(path);
    if (!f) return s;
    std::string line;
    int         n = 0;
    while (std::getline(f, line)) {
        n++;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error && error->empty()) *error = path + ":" + std::to_string(n) + ": expected key = value";
            continue;
        }
        std::string k = to_lower(trim(line.substr(0, eq)));
        std::string v = trim(line.substr(eq + 1));
        if (k == "search_url") s.search_url = v;
        else if (k == "home") s.home_url = v;
        else if (k == "doh_url") s.doh_url = v;
        else if (k == "doh_fallback") s.doh_fallback = parse_bool(v);
        else if (k == "user_agent") s.user_agent = v;
        else if (k == "preconnect") s.preconnect = parse_bool(v);
        else if (k == "http_cache") s.http_cache = parse_bool(v);
        else if (k == "http_cache_mb") s.http_cache_mb = std::max(1, std::min(4096, std::atoi(v.c_str())));
        else if (k == "referrer" || k == "referer") {
            std::string l = to_lower(trim(v));
            if (l == "strict" || l == "origin" || l == "off" || l == "full") s.referrer = l;
        }
        else if (k == "proxy") s.proxy = v;
        else if (k == "font_dir") s.font_dir = v;
        else if (k == "http_allow") add_list(s.http_allow, v);
        else if (k == "http_warning") {
            std::string w = to_lower(v);
            if (w == "typed" || w == "always" || w == "never") s.http_warning = w;
            else if (error && error->empty()) *error = path + ":" + std::to_string(n) + ": http_warning is typed, always or never";
        }
        else if (k == "private_allow") add_list(s.private_allow, v);
        else if (k == "cookies") s.cookies = parse_bool(v);
        else if (k == "trusted_certs") {
            std::stringstream ss(v);
            std::string       item;
            while (std::getline(ss, item, ',')) {
                item         = trim(item);
                size_t e     = item.find('=');
                std::string host = e == std::string::npos ? "" : normalize_host(trim(item.substr(0, e)));
                std::string pin  = e == std::string::npos ? "" : trim(item.substr(e + 1));
                if (host.empty() || !is_valid_pin(pin)) {
                    if (error && error->empty()) *error = path + ":" + std::to_string(n) + ": trusted_certs entries look like host=sha256//BASE64";
                    continue;
                }
                s.trusted_certs[host] = pin;
            }
        }
        else if (k == "max_page_mb") s.max_page_bytes = static_cast<size_t>(std::atol(v.c_str())) << 20;
        else if (k == "max_image_mb") s.max_image_bytes = static_cast<size_t>(std::atol(v.c_str())) << 20;
        else if (k == "timeout_s") s.timeout_ms = std::atol(v.c_str()) * 1000;
        else if (k == "zoom") s.zoom = std::max(30, std::min(300, std::atoi(v.c_str())));
        else if (k == "image_threads") s.image_threads = std::atoi(v.c_str());
        else if (k == "render_cache_mb") s.render_cache_mb = std::max(0, std::min(4096, std::atoi(v.c_str())));
        else if (k == "icon_dir") s.icon_dir = v;
        else if (k == "home_links") s.home_links = v;
        else if (k == "web_fonts") s.web_fonts = parse_bool(v);
        else if (k == "favicons") s.favicons = parse_bool(v);
        else if (k == "disk_cache") s.disk_cache = parse_bool(v);
        else if (k == "cache_dir") s.cache_dir = v;
        else if (k == "history") s.history = parse_bool(v);
        else if (k == "history_days") s.history_days = std::max(1, std::min(3650, std::atoi(v.c_str())));
        else if (k == "history_file") s.history_file = v;
        else if (k == "prerender") s.prerender = to_lower(v) != "false" && v != "0" && to_lower(v) != "no";
        else if (k == "window_width") s.window_w = std::atoi(v.c_str());
        else if (k == "window_height") s.window_h = std::atoi(v.c_str());
        else if (error && error->empty()) *error = path + ":" + std::to_string(n) + ": unknown key \"" + k + "\"";
    }
    if (s.image_threads < 1) s.image_threads = 1;
    if (s.image_threads > 16) s.image_threads = 16;
    if (s.timeout_ms < 1000) s.timeout_ms = 1000;
    if (s.window_w < 320) s.window_w = 320;
    if (s.window_h < 240) s.window_h = 240;
    return s;
}

} // namespace quill
