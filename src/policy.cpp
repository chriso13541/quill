#include "policy.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace quill {

std::string normalize_host(const std::string& host) {
    std::string h = to_lower(host);
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

bool parse_ip_literal(const std::string& host, unsigned char out[16], int& len) {
    if (host.find(':') != std::string::npos) {
        std::string h = host;
        size_t      pct = h.find('%'); // zone id
        if (pct != std::string::npos) h.erase(pct);
        if (inet_pton(AF_INET6, h.c_str(), out) == 1) {
            len = 16;
            return true;
        }
        return false;
    }
    if (inet_pton(AF_INET, host.c_str(), out) == 1) {
        len = 4;
        return true;
    }
    return false;
}

static bool v4_non_public(const unsigned char* b) {
    unsigned a = b[0], c = b[1];
    if (a == 0 || a == 10 || a == 127) return true;
    if (a == 100 && (c & 0xC0) == 64) return true;       // 100.64.0.0/10 CGNAT / Tailscale
    if (a == 169 && c == 254) return true;               // link-local
    if (a == 172 && (c & 0xF0) == 16) return true;       // 172.16.0.0/12
    if (a == 192 && c == 168) return true;
    if (a == 192 && c == 0 && b[2] == 0) return true;    // 192.0.0.0/24
    if (a == 198 && (c & 0xFE) == 18) return true;       // 198.18.0.0/15 benchmarking
    if (a >= 224) return true;                           // multicast + reserved + broadcast
    return false;
}

bool ip_is_non_public(const unsigned char* b, int len) {
    if (len == 4) return v4_non_public(b);
    if (len != 16) return true;

    bool first10zero = true;
    for (int i = 0; i < 10; i++)
        if (b[i] != 0) first10zero = false;

    if (first10zero && b[10] == 0xff && b[11] == 0xff) return v4_non_public(b + 12); // ::ffff:a.b.c.d
    bool first12zero = first10zero && b[10] == 0 && b[11] == 0;
    if (first12zero) return true;                       // ::, ::1, deprecated IPv4-compatible
    if ((b[0] & 0xFE) == 0xFC) return true;             // fc00::/7 unique local
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true; // fe80::/10 link-local
    if (b[0] == 0xFF) return true;                      // multicast
    if (b[0] == 0x20 && b[1] == 0x02) return v4_non_public(b + 2); // 6to4
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B) { // 64:ff9b::/96 NAT64
        bool zero = true;
        for (int i = 4; i < 12; i++)
            if (b[i] != 0) zero = false;
        if (zero) return v4_non_public(b + 12);
    }
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) return true; // documentation
    return false;
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Policy::is_private_host(const std::string& host_in) const {
    std::string host = normalize_host(host_in);
    if (host.empty()) return false;

    unsigned char ip[16];
    int           len = 0;
    if (parse_ip_literal(host, ip, len)) return ip_is_non_public(ip, len);

    for (const auto& entry : private_allow) {
        if (entry.compare(0, 2, "*.") == 0) {
            if (ends_with(host, entry.substr(1))) return true;
        } else if (host == entry) {
            return true;
        }
    }

    if (host.find('.') == std::string::npos) return true; // single-label: router, nas, localhost
    static const char* suffixes[] = {".localhost", ".local", ".lan", ".home", ".home.arpa", ".internal",
                                     ".intranet", ".corp", ".private", ".localdomain"};
    for (const char* s : suffixes)
        if (ends_with(host, s)) return true;
    return false;
}

Decision evaluate_request(const Policy& p, const ParsedUrl& t, RequestKind kind, const std::string& initiator_url,
                          bool is_redirect, const std::string& source_page) {
    Decision d;
    if (t.scheme != "http" && t.scheme != "https") {
        d.verdict = Verdict::Block;
        d.reason  = "Unsupported URL scheme \"" + t.scheme + ":\"";
        return d;
    }
    std::string host = normalize_host(t.host);
    if (!t.has_authority || host.empty()) {
        d.verdict = Verdict::Block;
        d.reason  = "URL has no host";
        return d;
    }
    if (!t.userinfo.empty()) {
        d.verdict = Verdict::Block;
        d.reason  = "URLs with embedded credentials are not supported";
        return d;
    }

    bool target_private = p.is_private_host(host);
    d.allow_private_ip  = target_private;
    d.use_doh           = !p.doh_url.empty() && !target_private;

    // Public pages (and redirects from public hosts) may not reach into the private network.
    if ((kind == RequestKind::Subresource || is_redirect) && target_private && !initiator_url.empty()) {
        ParsedUrl init;
        if (parse_url(initiator_url, init) && init.has_authority && !p.is_private_host(init.host)) {
            d.verdict = Verdict::Block;
            d.reason  = "Blocked: a page from the public internet tried to reach a private network address (" + host + ")";
            return d;
        }
    }

    if (t.scheme == "http" && !target_private && p.http_allow.count(host) == 0) {
        if (kind == RequestKind::TopLevel) {
            if (p.http_warn == Policy::HttpWarn::Never) return d;
            if (p.http_warn == Policy::HttpWarn::Typed) {
                // Already on the plain-http web (a link or form on an http page, or an http -> http redirect):
                // the user accepted plain http for where they came from, so following it on doesn't ask again.
                // https -> http (a downgrade) and typed addresses still ask.
                const std::string& from = is_redirect ? initiator_url : !source_page.empty() ? source_page : initiator_url;
                ParsedUrl          fu;
                if (!from.empty() && parse_url(from, fu) && fu.scheme == "http") return d;
            }
            d.verdict = Verdict::WarnHttp;
            d.reason  = "Plain HTTP is not encrypted";
            return d;
        }
        // Mixed content is only a concern on an https page. A plain-http page the user already chose to open
        // (old-web sites, which hotlink GIFs and counters from other http hosts) gains nothing from having its
        // plain-http images and stylesheets blocked: the page itself already travelled unencrypted.
        ParsedUrl init;
        if (!initiator_url.empty() && parse_url(initiator_url, init) && init.scheme == "http") return d;
        d.verdict = Verdict::Block;
        d.reason  = "Blocked insecure (http) subresource from " + host;
        return d;
    }
    return d;
}

} // namespace quill
