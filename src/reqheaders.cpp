// Request headers that don't depend on libcurl: Accept, Referer (with Referrer-Policy), Fetch Metadata. Shared by
// net.cpp and the browser tests' fake network, so the tests check what Quill really sends.
#include "net.h"

#include "cookies.h"
#include "url.h"

#include <cstring>

namespace quill {

namespace {
// What Firefox (the default User-Agent) sends, less formats Quill can't show: a request that claims to be one
// browser but asks like another is one of the things bot filters look for.
const char* accept_for(ResourceType t) {
    switch (t) {
    case ResourceType::Document: return "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
    case ResourceType::Stylesheet: return "text/css,*/*;q=0.1";
#ifdef QUILL_HAVE_WEBP
    case ResourceType::Image: return "image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
#else
    case ResourceType::Image: return "image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
#endif
    case ResourceType::Font: return "application/font-woff2;q=1.0,application/font-woff;q=0.9,*/*;q=0.8";
    default: return "*/*";
    }
}

std::string origin_of(const ParsedUrl& u) {
    std::string h = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    return u.scheme + "://" + h + (u.port > 0 ? ":" + std::to_string(u.port) : "");
}
int default_port(const ParsedUrl& u) { return u.port > 0 ? u.port : (u.scheme == "https" ? 443 : 80); }
bool same_origin(const ParsedUrl& a, const ParsedUrl& b) {
    return a.scheme == b.scheme && normalize_host(a.host) == normalize_host(b.host) && default_port(a) == default_port(b);
}

// Sec-Fetch-Site of `target` for a request from `from` ("" = the user typed it: "none").
const char* fetch_site(const std::string& from, const ParsedUrl& target) {
    if (from.empty()) return "none";
    ParsedUrl f;
    if (!parse_url(from, f) || !f.has_authority) return "cross-site";
    if (same_origin(f, target)) return "same-origin";
    if (f.scheme == target.scheme && same_site(normalize_host(f.host), normalize_host(target.host))) return "same-site";
    return "cross-site";
}
int site_rank(const char* s) { return std::strcmp(s, "cross-site") == 0 ? 3 : std::strcmp(s, "same-site") == 0 ? 2 : std::strcmp(s, "same-origin") == 0 ? 1 : 0; }

} // namespace

std::string parse_referrer_policy(const std::string& v) {
    static const char* known[] = {"no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
                                  "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"};
    std::string out;
    size_t      i = 0;
    while (i <= v.size()) {
        size_t      j = v.find(',', i);
        if (j == std::string::npos) j = v.size();
        std::string t = to_lower(trim(v.substr(i, j - i)));
        i = j + 1;
        // the old names <meta name=referrer> still accepts
        if (t == "never") t = "no-referrer";
        else if (t == "always") t = "unsafe-url";
        else if (t == "default") t = "strict-origin-when-cross-origin";
        else if (t == "origin-when-crossorigin") t = "origin-when-cross-origin";
        for (const char* k : known)
            if (t == k) out = t;
    }
    return out;
}

std::string referrer_value(const std::string& from, const std::string& to, const std::string& policy_in, const std::string& mode) {
    if (from.empty() || mode == "off") return {};
    ParsedUrl f, t;
    if (!parse_url(from, f) || !parse_url(to, t) || !f.has_authority) return {};
    if (f.scheme != "http" && f.scheme != "https") return {};
    std::string policy = policy_in.empty() ? "strict-origin-when-cross-origin" : policy_in;
    // capped: a page may send less than the default, never more (no full addresses to other sites, nothing to http)
    if (mode != "full" && (policy == "unsafe-url" || policy == "no-referrer-when-downgrade" || policy == "origin-when-cross-origin"))
        policy = "strict-origin-when-cross-origin";
    bool        same      = same_origin(f, t);
    bool        downgrade = f.scheme == "https" && t.scheme != "https";
    std::string origin    = origin_of(f) + "/";
    ParsedUrl   clean     = f;
    clean.fragment.clear();
    clean.has_fragment = false;
    clean.userinfo.clear();
    std::string full = serialize_url(clean);
    std::string r;
    if (policy == "no-referrer") r = "";
    else if (policy == "no-referrer-when-downgrade") r = downgrade ? "" : full;
    else if (policy == "same-origin") r = same ? full : "";
    else if (policy == "origin") r = origin;
    else if (policy == "strict-origin") r = downgrade ? "" : origin;
    else if (policy == "origin-when-cross-origin") r = same ? full : origin;
    else if (policy == "unsafe-url") r = full;
    else r = same ? full : (downgrade ? "" : origin); // strict-origin-when-cross-origin
    if (mode == "origin" && r == full && full != origin) r = origin;
    return r;
}

std::vector<std::string> request_headers(const FetchRequest& req, const std::string& from, const ParsedUrl& pu, const std::string& mode,
                                         const char*& site) {
    std::vector<std::string> h;
    std::string              url = serialize_url(pu);
    h.push_back(std::string("Accept: ") + accept_for(req.type));
    h.push_back("Accept-Language: en-US,en;q=0.5");
    std::string ref = req.no_referrer ? std::string() : referrer_value(from, url, req.referrer_policy, mode);
    if (!ref.empty()) h.push_back("Referer: " + ref);
    bool navigate = req.type == ResourceType::Document;
    if (navigate) h.push_back("Upgrade-Insecure-Requests: 1");
    if (pu.scheme == "https") { // Fetch Metadata, as browsers send it to potentially trustworthy URLs
        const char* hs = fetch_site(from, pu); // "none" through a typed address's redirects too
        if (!site || site_rank(hs) > site_rank(site)) site = hs;
        const char* dest = navigate ? (req.kind == RequestKind::TopLevel ? "document" : "iframe")
                         : req.type == ResourceType::Stylesheet ? "style" : req.type == ResourceType::Image ? "image"
                         : req.type == ResourceType::Font ? "font" : "empty";
        const char* fmode = navigate ? "navigate" : req.type == ResourceType::Font ? "cors" : "no-cors";
        h.push_back(std::string("Sec-Fetch-Dest: ") + dest);
        h.push_back(std::string("Sec-Fetch-Mode: ") + fmode);
        h.push_back(std::string("Sec-Fetch-Site: ") + site);
        if (navigate && req.kind == RequestKind::TopLevel && req.user_activated) h.push_back("Sec-Fetch-User: ?1");
    }
    return h;
}

CredentialStore& CredentialStore::instance() {
    static CredentialStore* c = new CredentialStore(); // leaked: workers may still be fetching at exit
    return *c;
}
void CredentialStore::set(const std::string& origin, const HttpCredentials& c) {
    std::lock_guard<std::mutex> lk(mu_);
    map_[origin] = c;
}
bool CredentialStore::get(const std::string& origin, HttpCredentials& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto                        it = map_.find(origin);
    if (it == map_.end()) return false;
    out = it->second;
    return true;
}
void CredentialStore::forget(const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    map_.erase(origin);
}
void CredentialStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
}

std::string url_origin(const std::string& url) {
    ParsedUrl u;
    if (!parse_url(url, u) || !u.has_authority || (u.scheme != "http" && u.scheme != "https")) return {};
    ParsedUrl o;
    o.scheme = u.scheme;
    o.host   = normalize_host(u.host);
    o.port   = default_port(u) == (u.scheme == "https" ? 443 : 80) ? 0 : default_port(u);
    return origin_of(o);
}

bool parse_www_authenticate(const std::string& header, std::string& scheme, std::string& realm) {
    // challenges: `Basic realm="x", Digest realm="y", nonce="..."` (one header or several joined with ", ")
    std::string l = to_lower(header);
    scheme.clear();
    realm.clear();
    auto realm_after = [&](size_t at) {
        size_t r = l.find("realm=", at);
        if (r == std::string::npos) return std::string();
        size_t v = r + 6;
        if (v < header.size() && header[v] == '"') { // a quoted-string: \x stands for x
            std::string out;
            for (size_t i = v + 1; i < header.size() && header[i] != '"'; i++) {
                if (header[i] == '\\' && i + 1 < header.size()) i++;
                out += header[i];
            }
            return out;
        }
        size_t e = header.find_first_of(", ", v);
        return header.substr(v, e == std::string::npos ? std::string::npos : e - v);
    };
    auto starts_word = [&](const char* w) -> size_t {
        size_t n = std::strlen(w), i = 0;
        while ((i = l.find(w, i)) != std::string::npos) {
            bool before = i == 0 || l[i - 1] == ' ' || l[i - 1] == ',';
            bool after  = i + n >= l.size() || l[i + n] == ' ';
            if (before && after) return i;
            i += n;
        }
        return std::string::npos;
    };
    size_t d = starts_word("digest"), b = starts_word("basic");
    if (d != std::string::npos) { scheme = "digest"; realm = realm_after(d); }
    else if (b != std::string::npos) { scheme = "basic"; realm = realm_after(b); }
    if (realm.size() > 200) realm.resize(200);
    return !scheme.empty();
}

bool may_send_credentials(const FetchRequest& req, const std::string& url) {
    std::string o = url_origin(url);
    if (o.empty()) return false;
    if (req.kind == RequestKind::TopLevel) return true; // the user is opening that site
    // a page's own requests to its own origin (its pictures, stylesheets, frames); never another site's page's
    std::string from = req.initiator.empty() ? req.referrer : req.initiator;
    return !from.empty() && url_origin(from) == o;
}

std::string request_referrer_source(const FetchRequest& req) {
    std::string from = req.referrer.empty() && req.kind == RequestKind::Subresource ? req.initiator : req.referrer;
    if (from.empty() && req.form_submit) from = req.initiator;
    return from;
}

} // namespace quill
