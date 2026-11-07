// Request policy: HTTPS-first, private-network isolation, resolver choice.
//
// Every network request (top-level page, stylesheet, image, each redirect hop)
// is run through evaluate_request() before it is sent.
#pragma once
#include "url.h"

#include <set>
#include <string>

namespace quill {

enum class RequestKind { TopLevel, Subresource };

struct Policy {
    std::string           doh_url;            // empty => system resolver for everything
    bool                  doh_fallback = false;
    std::set<std::string> http_allow;         // public hosts the user allowed over plain http
    std::set<std::string> private_allow;      // names treated as private ("host" or "*.suffix")
    enum class HttpWarn { Never, Typed, Always } http_warn = HttpWarn::Typed; // see Settings::http_warning

    // True for IP literals in private/loopback/link-local ranges, single-label
    // names, .local/.lan/.home.arpa/... names, and anything in private_allow.
    bool is_private_host(const std::string& host) const;
};

enum class Verdict { Allow, Block, WarnHttp };

struct Decision {
    Verdict     verdict = Verdict::Allow;
    std::string reason;
    bool        use_doh          = false; // resolve via DNS-over-HTTPS
    bool        allow_private_ip = false; // connection to private addresses permitted
};

// initiator_url: page that caused the request ("" for user-typed navigation).
// is_redirect:   the request is a redirect hop from initiator_url.
// source_page:   top-level only: the page whose link was followed ("" if typed). It only decides whether a public
//                http page needs the warning (not when following a link from a page that is itself plain http).
Decision evaluate_request(const Policy& p, const ParsedUrl& target, RequestKind kind,
                          const std::string& initiator_url, bool is_redirect, const std::string& source_page = "");

// bytes: 4 (IPv4) or 16 (IPv6) bytes in network order. True for anything that
// is not a normal public unicast address (private, loopback, link-local,
// CGNAT, multicast, reserved, and IPv6 forms embedding such IPv4 addresses).
bool ip_is_non_public(const unsigned char* bytes, int len);

// Parses a v4/v6 literal (no brackets). Returns false for hostnames.
bool parse_ip_literal(const std::string& host, unsigned char out[16], int& len);

std::string normalize_host(const std::string& host); // lowercase, no trailing dot

} // namespace quill
