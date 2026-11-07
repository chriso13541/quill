// HTTP Strict Transport Security (RFC 6797): sites that said "only ever talk to me over HTTPS" are upgraded from
// http:// to https:// before any request leaves, so a plain-http link (or someone on the network rewriting one)
// can't downgrade them. Remembered for the session only (like cookies), plus a small built-in preload list.
#pragma once

#include "url.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace quill {

class Hsts {
public:
    static Hsts& instance();

    // A Strict-Transport-Security header from `host`, received over https with a valid certificate.
    void note(const std::string& host, const std::string& header_value);
    // Must `host` only be reached over https? (preload list, or a header seen this session)
    bool must_upgrade(const std::string& host) const;
    // If `u` is http:// to such a host, turn it into https:// (port 80 -> default). Returns true if it changed.
    bool upgrade(ParsedUrl& u) const;

    void clear_for_tests();

private:
    struct Entry {
        long long expires = 0; // unix time
        bool      subdomains = false;
    };
    mutable std::mutex                     mu_;
    std::unordered_map<std::string, Entry> hosts_;
};

} // namespace quill
