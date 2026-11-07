// A small in-memory cookie jar.
//
// Policy (deliberately stricter than a normal browser):
//   - session only: nothing is written to disk, everything is gone when Quill quits;
//   - first-party only: a request gets/stores cookies only when its site is the
//     site of the page being viewed (see same_site()); third-party requests
//     (images, stylesheets, trackers) neither send nor store any;
//   - a cross-site form POST never carries cookies (like SameSite=Lax);
//   - Secure cookies are only stored from, and sent over, https.
// The jar is shared by all tabs.
#pragma once
#include "url.h"

#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace quill {

// "example.co.uk" for "www.example.co.uk"; IPs and single-label names are returned unchanged.
// (A small built-in heuristic for common two-level suffixes, not the full public-suffix list.)
std::string registrable_domain(const std::string& host);
bool        same_site(const std::string& host_a, const std::string& host_b);

class CookieJar {
public:
    static CookieJar& instance();

    // Handles one Set-Cookie header value for a response from `url`.
    void store(const ParsedUrl& url, const std::string& set_cookie, time_t now = std::time(nullptr));
    // Value for the Cookie request header ("a=1; b=2"), or "".
    std::string header_for(const ParsedUrl& url, time_t now = std::time(nullptr)) const;
    void        clear();
    size_t      size() const;

private:
    struct Cookie {
        std::string name, value, domain, path;
        bool        host_only = true, secure = false;
        time_t      expires   = 0; // 0 = session
        time_t      created   = 0;
    };
    mutable std::mutex  mu_;
    std::vector<Cookie> jar_;
};

} // namespace quill
