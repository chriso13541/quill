// A stand-in for net.cpp in browser tests: serves an in-memory site instead of using libcurl, but runs every
// request (and redirect hop) through the real request policy, so http warnings / blocking behave as in Quill.
#pragma once
#include "net.h"

#include <chrono>
#include <map>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

namespace quill::fake {

struct Resource {
    std::string body, content_type = "text/html";
    std::string redirect; // non-empty: answer 302 to this URL
    int         delay_ms = 0; // simulated network latency
    std::string hsts;         // Strict-Transport-Security header to send
    std::string frame_options; // X-Frame-Options header to send
    std::string csp;           // Content-Security-Policy header to send
    std::string referrer_policy; // Referrer-Policy header to send
};

// url (without #fragment) -> resource. Fill before the browser loads anything.
std::map<std::string, Resource>& site();
// Every URL actually "fetched" (policy allowed it), in order.
std::vector<std::string> fetched();
void clear_log();
// The headers Quill computed (request_headers()) for the last request to `url` ("" = none seen), one "Name: value"
// per line, and the value of one of them ("" if absent).
std::string headers_for(const std::string& url);
std::string header_value(const std::string& url, const std::string& name);
// HTTP authentication: requests to `url` get a 401 with this WWW-Authenticate unless they carry user / pass.
struct AuthNeed {
    std::string challenge, user, pass;
};
std::map<std::string, AuthNeed>& auth_needed();
// URLs preconnect() was asked for.
std::vector<std::string> preconnected();

} // namespace quill::fake
