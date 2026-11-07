#include "hsts.h"

#include "policy.h"

#include <cctype>
#include <cstdlib>
#include <ctime>

namespace quill {

namespace {

// Whole top-level domains that are HTTPS-only by registry policy (on every browser's HSTS preload list).
const char* kPreloadTlds[] = {"app", "dev", "page", "bank", "insurance", "foo", "new", "day", "esq", "zip", "mov"};

// A few major sites on the preload list (with their subdomains). Kept short on purpose: the session store learns the
// rest from the sites' own headers.
const char* kPreloadSites[] = {"wikipedia.org", "wikimedia.org", "github.com", "paypal.com", "duckduckgo.com", "torproject.org",
                               "proton.me", "protonmail.com", "eff.org", "letsencrypt.org"};

bool is_ip_literal(const std::string& h) {
    unsigned char ip[16];
    int           len = 0;
    return parse_ip_literal(h, ip, len);
}

bool ends_with_label(const std::string& host, const std::string& domain) {
    return host == domain ||
           (host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == '.');
}

} // namespace

Hsts& Hsts::instance() {
    static Hsts h;
    return h;
}

void Hsts::note(const std::string& host_in, const std::string& value) {
    std::string host = normalize_host(host_in);
    if (host.empty() || is_ip_literal(host)) return; // RFC 6797 8.1: not for IP addresses
    long long max_age = -1;
    bool      subdomains = false;
    size_t    pos = 0;
    while (pos <= value.size()) { // directives: max-age=N; includeSubDomains; preload
        size_t      semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        std::string d = trim(value.substr(pos, semi - pos));
        pos           = semi + 1;
        std::string l = to_lower(d);
        if (l.compare(0, 7, "max-age") == 0) {
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string v = trim(l.substr(eq + 1));
            if (!v.empty() && v.front() == '"' && v.back() == '"' && v.size() >= 2) v = v.substr(1, v.size() - 2);
            if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0]))) continue;
            max_age = std::min<long long>(std::strtoll(v.c_str(), nullptr, 10), 10LL * 365 * 86400);
        } else if (l == "includesubdomains") {
            subdomains = true;
        }
    }
    if (max_age < 0) return; // max-age is required
    std::lock_guard<std::mutex> lk(mu_);
    if (max_age == 0) { // the site turns HSTS off
        hosts_.erase(host);
        return;
    }
    if (hosts_.size() > 50000) hosts_.clear();
    hosts_[host] = {static_cast<long long>(std::time(nullptr)) + max_age, subdomains};
}

bool Hsts::must_upgrade(const std::string& host_in) const {
    std::string host = normalize_host(host_in);
    if (host.empty() || is_ip_literal(host)) return false;
    size_t      dot = host.rfind('.');
    std::string tld = dot == std::string::npos ? host : host.substr(dot + 1);
    for (const char* t : kPreloadTlds)
        if (tld == t) return true;
    for (const char* s : kPreloadSites)
        if (ends_with_label(host, s)) return true;
    long long now = static_cast<long long>(std::time(nullptr));
    std::lock_guard<std::mutex> lk(mu_);
    // the host itself, or a parent that set includeSubDomains
    std::string h = host;
    bool        self = true;
    while (true) {
        auto it = hosts_.find(h);
        if (it != hosts_.end() && it->second.expires > now && (self || it->second.subdomains)) return true;
        size_t d = h.find('.');
        if (d == std::string::npos) break;
        h    = h.substr(d + 1);
        self = false;
        if (h.find('.') == std::string::npos) break; // never a bare TLD from a header
    }
    return false;
}

bool Hsts::upgrade(ParsedUrl& u) const {
    if (u.scheme != "http" || !must_upgrade(u.host)) return false;
    u.scheme = "https";
    if (u.port == 80) u.port = -1;
    return true;
}

void Hsts::clear_for_tests() {
    std::lock_guard<std::mutex> lk(mu_);
    hosts_.clear();
}

} // namespace quill
