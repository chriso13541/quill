#include "fake_net.h"

#include "hsts.h"
#include "url.h"

namespace quill {
namespace fake {
namespace {
std::mutex               g_mu;
std::vector<std::string> g_log;
} // namespace
std::map<std::string, Resource>& site() {
    static std::map<std::string, Resource> s;
    return s;
}
std::vector<std::string> fetched() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_log;
}
void clear_log() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log.clear();
}
} // namespace fake

namespace fake {
std::map<std::string, std::string> g_headers;
std::vector<std::string>           g_pre;
std::string headers_for(const std::string& url) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_headers.find(url);
    return it == g_headers.end() ? std::string() : it->second;
}
std::string header_value(const std::string& url, const std::string& name) {
    std::string h = headers_for(url), want = name + ": ";
    size_t      i = 0;
    while (i < h.size()) {
        size_t e = h.find('\n', i);
        if (e == std::string::npos) e = h.size();
        if (h.compare(i, want.size(), want) == 0) return h.substr(i + want.size(), e - i - want.size());
        i = e + 1;
    }
    return {};
}
std::map<std::string, AuthNeed>& auth_needed() {
    static std::map<std::string, AuthNeed> m;
    return m;
}
std::vector<std::string> preconnected() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_pre;
}
} // namespace fake

void Network::preconnect(const std::string& url) {
    std::lock_guard<std::mutex> lk(fake::g_mu);
    fake::g_pre.push_back(url);
}

Network::Network(const Settings& s, std::shared_ptr<RequestFilter> filter) :
    settings_(s), filter_(filter ? std::move(filter) : std::make_shared<RequestFilter>()) {
    trusted_certs_ = s.trusted_certs;
}
Network::~Network() = default;

void Network::allow_http_host(const std::string& host) {
    std::lock_guard<std::mutex> lk(mu_);
    session_http_allow_.insert(normalize_host(host));
}
void Network::trust_cert(const std::string& host, const std::string& pin) {
    std::lock_guard<std::mutex> lk(mu_);
    trusted_certs_[normalize_host(host)] = pin;
}
bool Network::trusts_cert(const std::string& host) const {
    std::lock_guard<std::mutex> lk(mu_);
    return trusted_certs_.count(normalize_host(host)) != 0;
}
Policy Network::policy() const {
    Policy p;
    p.doh_url       = settings_.doh_url;
    p.http_allow    = settings_.http_allow;
    p.private_allow = settings_.private_allow;
    p.http_warn     = settings_.http_warning == "never" ? Policy::HttpWarn::Never
                    : settings_.http_warning == "always" ? Policy::HttpWarn::Always : Policy::HttpWarn::Typed;
    std::lock_guard<std::mutex> lk(mu_);
    p.http_allow.insert(session_http_allow_.begin(), session_http_allow_.end());
    return p;
}

FetchResult Network::fetch(const FetchRequest& req) {
    FetchResult res;
    Policy      pol = policy();
    std::string url = req.url, prev;
    std::string from = request_referrer_source(req);
    const char* site = nullptr;
    for (int hop = 0; hop < 10; hop++) {
        res.final_url = url;
        ParsedUrl pu;
        if (!parse_url(url, pu)) { res.error = "Invalid URL"; return res; }
        if (Hsts::instance().upgrade(pu)) { url = serialize_url(pu); res.final_url = url; } // as in net.cpp
        Decision d = evaluate_request(pol, pu, req.kind, hop == 0 ? req.initiator : prev, hop > 0, hop == 0 ? req.source_page : std::string());
        if (d.verdict == Verdict::Block) { res.blocked = true; res.error = d.reason; return res; }
        if (d.verdict == Verdict::WarnHttp) { res.needs_http_confirm = true; res.error = d.reason; return res; }
        if (req.form_submit && hop == 0 && d.allow_private_ip && !req.initiator.empty()) { // as in net.cpp
            ParsedUrl init;
            if (parse_url(req.initiator, init) && init.has_authority && !pol.is_private_host(init.host)) {
                res.blocked = true;
                res.error   = "Blocked: form to a private address";
                return res;
            }
        }
        std::string key = strip_fragment(url);
        {
            std::string hs;
            for (auto& l : request_headers(req, from, pu, settings_.referrer, site)) hs += l + "\n";
            std::lock_guard<std::mutex> lk(fake::g_mu);
            fake::g_log.push_back(key);
            fake::g_headers[key] = hs;
        }
        auto it = fake::site().find(key);
        if (it == fake::site().end()) { res.ok = true; res.status = 404; res.content_type = "text/html"; return res; }
        if (it->second.delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(it->second.delay_ms));
        if (!it->second.hsts.empty() && pu.scheme == "https") Hsts::instance().note(pu.host, it->second.hsts);
        if (!it->second.redirect.empty()) { prev = url; url = resolve_url(url, it->second.redirect); continue; }
        auto an = fake::auth_needed().find(key);
        if (an != fake::auth_needed().end()) { // as net.cpp: stored credentials go only where may_send_credentials says
            HttpCredentials c;
            bool sent = may_send_credentials(req, url) && CredentialStore::instance().get(url_origin(url), c);
            if (!sent || c.user != an->second.user || c.pass != an->second.pass) {
                res.ok     = true;
                res.status = 401;
                res.body   = "<html><body>the server's 401 page</body></html>";
                res.content_type = "text/html";
                res.final_url    = url;
                if (parse_www_authenticate(an->second.challenge, res.auth_scheme, res.auth_realm)) {
                    res.auth_required = true;
                    res.auth_failed   = sent;
                }
                return res;
            }
        }
        res.ok           = true;
        res.status       = 200;
        res.body         = it->second.body;
        res.content_type = it->second.content_type;
        res.frame_options = it->second.frame_options;
        res.csp          = it->second.csp;
        res.referrer_policy = parse_referrer_policy(it->second.referrer_policy);
        return res;
    }
    res.error = "Too many redirects";
    return res;
}

} // namespace quill
