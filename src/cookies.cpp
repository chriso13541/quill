#include "cookies.h"

#include "policy.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace quill {

namespace {

constexpr size_t kMaxCookies       = 2000;
constexpr size_t kMaxPerDomain     = 50;
constexpr size_t kMaxCookieBytes   = 4096;

bool is_ip(const std::string& h) {
    unsigned char b[16];
    int           n;
    return parse_ip_literal(h, b, n);
}

bool domain_match(const std::string& host, const std::string& domain, bool host_only) {
    if (host == domain) return true;
    if (host_only) return false;
    return host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == '.';
}

bool path_match(const std::string& req_path, const std::string& cookie_path) {
    if (req_path == cookie_path) return true;
    if (req_path.compare(0, cookie_path.size(), cookie_path) != 0) return false;
    if (!cookie_path.empty() && cookie_path.back() == '/') return true;
    return req_path.size() > cookie_path.size() && req_path[cookie_path.size()] == '/';
}

std::string default_path(const std::string& path) {
    if (path.empty() || path[0] != '/') return "/";
    size_t slash = path.rfind('/');
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string url_path(const ParsedUrl& u) { return u.path.empty() ? "/" : u.path; }

// "Wed, 21 Oct 2026 07:28:00 GMT" and a few close variants. Returns false if unparseable.
bool parse_http_date(const std::string& s, time_t& out) {
    static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string        l = to_lower(s);
    for (auto& c : l)
        if (c == ',' || c == '-') c = ' ';
    int  day = 0, year = 0, hh = 0, mm = 0, ss = 0, mon = -1;
    bool have_time = false;
    size_t i = 0;
    while (i < l.size()) {
        while (i < l.size() && l[i] == ' ') i++;
        size_t j = i;
        while (j < l.size() && l[j] != ' ') j++;
        std::string tok = l.substr(i, j - i);
        i               = j;
        if (tok.empty()) continue;
        if (tok.find(':') != std::string::npos && !have_time) {
            if (std::sscanf(tok.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) have_time = true;
        } else if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
            int v = std::atoi(tok.c_str());
            if (tok.size() <= 2 && day == 0) day = v;
            else if (year == 0) year = v;
        } else if (tok.size() >= 3) {
            for (int m = 0; m < 12; m++)
                if (tok.compare(0, 3, months[m]) == 0 && mon < 0) mon = m;
        }
    }
    if (day < 1 || day > 31 || mon < 0 || !have_time) return false;
    if (year < 70) year += 2000;
    else if (year < 100) year += 1900;
    if (year < 1970) return false;
    // days from civil (Howard Hinnant)
    int      y   = year - (mon < 2 ? 1 : 0);
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * static_cast<unsigned>(mon + (mon > 1 ? -2 : 10)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
    out = static_cast<time_t>(days * 86400 + hh * 3600 + mm * 60 + ss);
    return true;
}

} // namespace

std::string registrable_domain(const std::string& host_in) {
    std::string host = normalize_host(host_in);
    if (host.empty() || is_ip(host) || host.find('.') == std::string::npos) return host;
    std::vector<std::string> labels;
    size_t                   s = 0;
    while (true) {
        size_t d = host.find('.', s);
        labels.push_back(host.substr(s, d == std::string::npos ? std::string::npos : d - s));
        if (d == std::string::npos) break;
        s = d + 1;
    }
    size_t n = labels.size();
    if (n < 2) return host;
    static const char* second_level[] = {"co", "com", "org", "net", "gov", "edu", "ac", "or", "ne", "go"};
    size_t             keep           = 2;
    if (n >= 3 && labels[n - 1].size() == 2) {
        for (const char* sl : second_level)
            if (labels[n - 2] == sl) keep = 3;
    }
    std::string out;
    for (size_t i = n - keep; i < n; i++) {
        if (!out.empty()) out += '.';
        out += labels[i];
    }
    return out;
}

bool same_site(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    return registrable_domain(a) == registrable_domain(b);
}

CookieJar& CookieJar::instance() {
    static CookieJar j;
    return j;
}

void CookieJar::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    jar_.clear();
}

size_t CookieJar::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return jar_.size();
}

void CookieJar::store(const ParsedUrl& url, const std::string& header, time_t now) {
    if (header.size() > kMaxCookieBytes) return;
    std::string host = normalize_host(url.host);
    if (host.empty()) return;

    // name=value ; attr ; attr=value ...
    std::vector<std::string> parts;
    size_t                   s = 0;
    while (true) {
        size_t sc = header.find(';', s);
        parts.push_back(trim(header.substr(s, sc == std::string::npos ? std::string::npos : sc - s)));
        if (sc == std::string::npos) break;
        s = sc + 1;
    }
    size_t eq = parts[0].find('=');
    if (eq == std::string::npos || eq == 0) return;
    Cookie c;
    c.name    = trim(parts[0].substr(0, eq));
    c.value   = trim(parts[0].substr(eq + 1));
    c.domain  = host;
    c.created = now;
    c.path    = default_path(url_path(url));

    bool   have_max_age = false, delete_it = false;
    time_t max_age_exp = 0, date_exp = 0;
    bool   have_date = false;
    for (size_t i = 1; i < parts.size(); i++) {
        size_t      e    = parts[i].find('=');
        std::string name = to_lower(trim(parts[i].substr(0, e)));
        std::string val  = e == std::string::npos ? "" : trim(parts[i].substr(e + 1));
        if (name == "domain") {
            std::string d = to_lower(val);
            if (!d.empty() && d[0] == '.') d.erase(0, 1);
            if (d.empty()) continue;
            if (d != host && (is_ip(d) || d.find('.') == std::string::npos)) return; // no TLD-wide or bogus domains
            if (!domain_match(host, d, false)) return;                              // may only widen to a parent of the host
            if (registrable_domain(d) != registrable_domain(host)) return;          // never a public suffix like co.uk
            c.domain    = d;
            c.host_only = false;
        } else if (name == "path") {
            if (!val.empty() && val[0] == '/') c.path = val;
        } else if (name == "secure") {
            c.secure = true;
        } else if (name == "max-age") {
            char* endp = nullptr;
            long  v    = std::strtol(val.c_str(), &endp, 10);
            if (endp != val.c_str()) {
                have_max_age = true;
                if (v <= 0) delete_it = true;
                else max_age_exp = now + v;
            }
        } else if (name == "expires") {
            time_t t;
            if (parse_http_date(val, t)) {
                have_date = true;
                date_exp  = t;
            }
        }
    }
    if (have_max_age) c.expires = max_age_exp;
    else if (have_date) {
        c.expires = date_exp;
        if (date_exp <= now) delete_it = true;
    }
    if (c.secure && url.scheme != "https") return; // refuse Secure cookies from plain http
    if (c.name.compare(0, 9, "__Secure-") == 0 && !(c.secure && url.scheme == "https")) return;
    if (c.name.compare(0, 7, "__Host-") == 0 && !(c.secure && c.host_only && c.path == "/")) return;

    std::lock_guard<std::mutex> lk(mu_);
    // "Leave Secure cookies alone" (RFC 6265bis 5.6): plain http may not overwrite or delete a Secure cookie of the
    // same name, or someone on the network could replace a session cookie that was set over https.
    if (url.scheme != "https")
        for (const auto& old : jar_)
            if (old.secure && old.name == c.name && (domain_match(c.domain, old.domain, false) || domain_match(old.domain, c.domain, false)))
                return;
    // replace / delete an existing cookie with the same (name, domain, path)
    for (auto it = jar_.begin(); it != jar_.end(); ++it) {
        if (it->name == c.name && it->domain == c.domain && it->path == c.path && it->host_only == c.host_only) {
            if (delete_it) jar_.erase(it);
            else {
                c.created = it->created;
                *it       = c;
            }
            return;
        }
    }
    if (delete_it) return;
    size_t same_domain = 0;
    for (auto& k : jar_)
        if (k.domain == c.domain) same_domain++;
    if (same_domain >= kMaxPerDomain) { // evict the oldest of that domain
        auto oldest = jar_.end();
        for (auto it = jar_.begin(); it != jar_.end(); ++it)
            if (it->domain == c.domain && (oldest == jar_.end() || it->created < oldest->created)) oldest = it;
        if (oldest != jar_.end()) jar_.erase(oldest);
    }
    if (jar_.size() >= kMaxCookies) jar_.erase(jar_.begin());
    jar_.push_back(std::move(c));
}

std::string CookieJar::header_for(const ParsedUrl& url, time_t now) const {
    std::string host = normalize_host(url.host);
    std::string path = url_path(url);
    bool        https = url.scheme == "https";
    std::vector<const Cookie*> hits;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : jar_) {
        if (c.expires != 0 && c.expires <= now) continue;
        if (c.secure && !https) continue;
        if (!domain_match(host, c.domain, c.host_only)) continue;
        if (!path_match(path, c.path)) continue;
        hits.push_back(&c);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Cookie* a, const Cookie* b) {
        if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
        return a->created < b->created;
    });
    std::string out;
    for (const Cookie* c : hits) {
        if (!out.empty()) out += "; ";
        out += c->name + "=" + c->value;
    }
    return out;
}

} // namespace quill
