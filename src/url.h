// URL parsing and resolution (RFC 3986 subset) plus a few string helpers.
#pragma once
#include <string>

namespace quill {

struct ParsedUrl {
    std::string scheme;    // lowercase, without ':'
    std::string userinfo;  // "user:pass" if present
    std::string host;      // lowercase, IPv6 without brackets
    int         port = -1; // -1 = not given
    std::string path;      // starts with '/' for hierarchical URLs
    std::string query;     // without '?'
    std::string fragment;  // without '#'
    bool has_authority = false;
    bool has_query     = false;
    bool has_fragment  = false;
};

// Parses an absolute URL (must start with "scheme:"). Returns false otherwise.
bool parse_url(const std::string& s, ParsedUrl& out);
std::string serialize_url(const ParsedUrl& u);

// Resolves `ref` against `base` (RFC 3986 section 5.2). Absolute refs are
// normalised (dot segments removed) and returned.
std::string resolve_url(const std::string& base, const std::string& ref);
std::string strip_fragment(const std::string& url);

std::string percent_encode(const std::string& s);                       // query component, ' ' -> '+'
std::string percent_decode(const std::string& s, bool plus_as_space);
std::string html_escape(const std::string& s);
std::string trim(const std::string& s);
std::string to_lower(std::string s);

// Value of a query parameter ("a=1&b=2"), percent-decoded. Empty if missing.
std::string query_param(const std::string& query, const std::string& key);

// Search engines wrap result links in click-tracking redirects (DuckDuckGo's /l/?uddg=<url>, Google's /url?q=<url>)
// that only forward with JavaScript or a meta refresh. Returns the real destination, or `url` unchanged.
std::string unwrap_redirect(const std::string& url);

// RFC 3492 punycode of one label ("bücher" -> "bcher-kva"); ASCII labels come back unchanged.
std::string punycode_label(const std::string& utf8_label);
// A host as Quill shows it: every label with non-ASCII characters in its xn-- form. Lookalike letters from other
// scripts (Cyrillic "а" in "аpple.com") then can't pass for a different site; the xn-- form is also what the
// certificate and DNS actually use.
std::string display_host(const std::string& host);
// `url` with its host in display form (the address bar, the status line, the security indicator).
std::string display_url(const std::string& url);

} // namespace quill
