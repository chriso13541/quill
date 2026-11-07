// HTTP(S) fetching on top of libcurl.
//
//  - Redirects are followed manually so the policy and the ad/tracker filter
//    apply to every hop.
//  - DNS-over-HTTPS (CURLOPT_DOH_URL) is used for public hosts; private names
//    (single-label, .lan, IP literals, ...) always use the system resolver.
//  - A socket-open hook refuses connections from public names to private IPs
//    (DNS rebinding protection) unless the host is on the private list.
//  - Cookies come from the shared first-party session jar (cookies.h); TLS 1.2+.
//  - Referer follows the page's Referrer-Policy, capped by the `referrer` setting (default: a page's full address
//    only to its own origin, just the origin to other https sites, nothing on https -> http).
//  - Requests carry the headers the browser in the User-Agent sends (Accept, Sec-Fetch-*, Upgrade-Insecure-Requests).
//  - Certificate verification is always on. The one exception is a certificate the user
//    explicitly trusted for a *private* host: it is pinned by public key (certpin.h).
#pragma once
#include "filter.h"
#include "policy.h"
#include "settings.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace quill {

struct FetchRequest {
    std::string        url;
    RequestKind        kind = RequestKind::Subresource;
    ResourceType       type = ResourceType::Other;
    std::string        initiator;          // page URL that caused this request ("" for typed navigation)
    size_t             max_bytes = 16u << 20;
    std::atomic<bool>* cancel    = nullptr; // set to true to abort an in-flight request
    std::string        method    = "GET";   // "GET" or "POST"
    std::string        body;                // POST body
    std::string        content_type;        // POST Content-Type
    bool               form_submit = false; // caused by submitting a form on `initiator`
    std::string        source_page;          // top-level: page whose link was followed (only relaxes the http warning)
    // The page this request comes from, for Referer and Sec-Fetch-Site ("" = typed / none; a subresource with no
    // referrer uses `initiator`), and that page's Referrer-Policy ("" = the default).
    std::string        referrer;
    std::string        referrer_policy;
    bool               no_referrer = false;  // <a rel=noreferrer> and friends: never send one
    bool               user_activated = true; // top-level: the user clicked or typed (Sec-Fetch-User)
};

// The Referer value for a request from page `from` to `to` under `policy` (a Referrer-Policy token, "" = default),
// capped by `mode` (the `referrer` setting: "strict" (default), "origin", "off" or "full"). "" = send none.
std::string referrer_value(const std::string& from, const std::string& to, const std::string& policy, const std::string& mode);
// The last policy token in a Referrer-Policy header / <meta name=referrer> that Quill knows ("" if none).
std::string parse_referrer_policy(const std::string& v);
// The page a request comes from (for Referer / Sec-Fetch-Site): its referrer, else (subresources, forms) its initiator.
std::string request_referrer_source(const FetchRequest& req);
// Accept, Accept-Language, Referer, Upgrade-Insecure-Requests and Sec-Fetch-* for one hop to `pu`. `site` carries the
// Sec-Fetch-Site across redirect hops (start with nullptr): it only ever gets less related.
std::vector<std::string> request_headers(const FetchRequest& req, const std::string& from, const ParsedUrl& pu, const std::string& mode,
                                         const char*& site);

struct FetchResult {
    bool        ok      = false; // got an HTTP response (any status)
    int         status  = 0;
    std::string final_url;
    std::string content_type;    // lowercase, without parameters
    std::string charset;         // from Content-Type, may be empty
    std::string body;
    std::string error;           // human readable, set when !ok
    bool        blocked            = false; // refused by policy or filter
    bool        needs_http_confirm = false; // plain-http top-level navigation awaiting user consent
    bool        cancelled          = false;
    // Set when a private host presented a certificate that does not verify (self-signed, wrong name, ...)
    // and the user has not trusted it yet. The browser shows these and offers to trust the key.
    bool        needs_cert_confirm = false;
    std::string cert_host, cert_pin, cert_subject, cert_issuer, cert_expires, cert_error;
    // Framing rules of the final response (whether it may be shown inside another site's <iframe>)
    std::string frame_options; // X-Frame-Options
    std::string csp;           // Content-Security-Policy (only frame-ancestors is used)
    std::string referrer_policy; // Referrer-Policy (parsed: one token, "" if none)
    // HTTP authentication (401 with a Basic / Digest WWW-Authenticate): the site asks for a username and password.
    // auth_failed: Quill sent the ones it had and they were refused.
    bool        auth_required = false, auth_failed = false;
    std::string auth_scheme, auth_realm; // "basic" / "digest"; the realm as the site words it (untrusted text)
};

// Usernames and passwords given for HTTP authentication, per origin (scheme://host:port), for the session only
// (never written to disk). They are sent to that origin only: for pages opened there and for the requests those
// pages make to their own origin, never for another site's page that embeds or links to it.
struct HttpCredentials {
    std::string user, pass;
    bool        digest = false;
    std::string realm;
};
class CredentialStore {
public:
    static CredentialStore& instance();
    void set(const std::string& origin, const HttpCredentials& c);
    bool get(const std::string& origin, HttpCredentials& out) const;
    void forget(const std::string& origin);
    void clear();

private:
    mutable std::mutex                     mu_;
    std::map<std::string, HttpCredentials> map_;
};
// "https://host[:port]" of a URL ("" if it has none). The port is left out when it is the scheme's default.
std::string url_origin(const std::string& url);
// The scheme Quill can answer ("digest" preferred over "basic"; "" if neither is offered) and its realm.
bool parse_www_authenticate(const std::string& header, std::string& scheme, std::string& realm);
// Whether stored credentials for `url`'s origin may go with this request.
bool may_send_credentials(const FetchRequest& req, const std::string& url);

class Network {
public:
    Network(const Settings& s, std::shared_ptr<RequestFilter> filter);
    ~Network();

    FetchResult fetch(const FetchRequest& req);
    // Looks the host up and opens (then drops) a connection to it, so the first real request finds the address
    // cached, the DNS-over-HTTPS connection open and a TLS session to resume. Blocks; call it from a worker thread.
    void        preconnect(const std::string& url);

    void   allow_http_host(const std::string& host); // session-only exception
    void   trust_cert(const std::string& host, const std::string& pin); // session-only; private hosts only
    Policy policy() const;
    // The user trusted this LAN host's certificate by its key (config trusted_certs or "trust" this session).
    bool   trusts_cert(const std::string& host) const;
    std::shared_ptr<RequestFilter> filter() const { return filter_; }
    const Settings&                settings() const { return settings_; }

private:
    Settings                       settings_;
    std::shared_ptr<RequestFilter> filter_;
    mutable std::mutex             mu_;
    std::set<std::string>          session_http_allow_;
    std::map<std::string, std::string> trusted_certs_; // host -> pin (config + session)
    bool                           proxy_in_use_ = false;
};

} // namespace quill
