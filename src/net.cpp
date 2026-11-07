#include "net.h"

#include "certpin.h"
#include "cookies.h"
#include "httpcache.h"
#include "hsts.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace quill {

namespace {

constexpr int kMaxRedirects = 10;

struct Ctx {
    std::string*       body        = nullptr;
    size_t             max_bytes   = 0;
    bool               too_big     = false;
    std::atomic<bool>* cancel      = nullptr;
    std::string        location;
    std::string        content_type;
    bool               check_ip         = true;
    bool               allow_private_ip = false;
    bool               ip_refused       = false;
    std::vector<std::string> set_cookies;
    std::string        hsts; // Strict-Transport-Security
    std::string        frame_options, csp; // X-Frame-Options, Content-Security-Policy (for frame-ancestors)
    std::string        referrer_policy;
    CacheHeaders       cache;
    std::string        www_authenticate;
};

size_t write_cb(char* p, size_t sz, size_t n, void* ud) {
    auto*  c   = static_cast<Ctx*>(ud);
    size_t len = sz * n;
    if (c->body->size() + len > c->max_bytes) {
        c->too_big = true;
        return 0;
    }
    c->body->append(p, len);
    return len;
}

size_t header_cb(char* b, size_t sz, size_t n, void* ud) {
    auto*       c    = static_cast<Ctx*>(ud);
    size_t      len  = sz * n;
    std::string line(b, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.compare(0, 5, "HTTP/") == 0) { // new response (redirect chain, proxy CONNECT, 100-continue)
        c->location.clear();
        c->content_type.clear();
        c->set_cookies.clear();
        c->hsts.clear();
        c->frame_options.clear();
        c->csp.clear();
        c->referrer_policy.clear();
        c->cache = CacheHeaders();
        c->www_authenticate.clear();
        return len;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return len;
    std::string name = to_lower(line.substr(0, colon));
    std::string val  = trim(line.substr(colon + 1));
    if (name == "location") c->location = val;
    else if (name == "content-type") c->content_type = val;
    else if (name == "cache-control") c->cache.cache_control += (c->cache.cache_control.empty() ? "" : ", ") + val;
    else if (name == "expires") c->cache.expires = val;
    else if (name == "date") c->cache.date = val;
    else if (name == "last-modified") c->cache.last_modified = val;
    else if (name == "etag") c->cache.etag = val;
    else if (name == "age") c->cache.age = val;
    else if (name == "vary") c->cache.vary += (c->cache.vary.empty() ? "" : ", ") + val;
    else if (name == "pragma") c->cache.pragma = val;
    else if (name == "www-authenticate") c->www_authenticate += (c->www_authenticate.empty() ? "" : ", ") + val;
    else if (name == "strict-transport-security") {
        if (c->hsts.empty()) c->hsts = val; // RFC 6797: only the first one counts
    } else if (name == "x-frame-options") {
        if (c->frame_options.empty()) c->frame_options = val;
    } else if (name == "content-security-policy") {
        if (c->csp.size() < 8192) c->csp += (c->csp.empty() ? "" : ", ") + val; // several headers all apply
    } else if (name == "referrer-policy") {
        std::string p = parse_referrer_policy(val);
        if (!p.empty()) c->referrer_policy = p; // the last one that is understood
    } else if (name == "set-cookie") {
        if (c->set_cookies.size() < 32) c->set_cookies.push_back(val);
    }
    return len;
}

int xfer_cb(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<Ctx*>(ud);
    return (c->cancel && c->cancel->load()) ? 1 : 0;
}

curl_socket_t open_cb(void* clientp, curlsocktype purpose, struct curl_sockaddr* addr) {
    auto* c = static_cast<Ctx*>(clientp);
    if (purpose == CURLSOCKTYPE_IPCXN && c->check_ip && !c->allow_private_ip) {
        unsigned char bytes[16];
        int           len = 0;
        if (addr->family == AF_INET) {
            sockaddr_in sin;
            std::memcpy(&sin, &addr->addr, sizeof(sin));
            std::memcpy(bytes, &sin.sin_addr, 4);
            len = 4;
        } else if (addr->family == AF_INET6) {
            sockaddr_in6 sin6;
            std::memcpy(&sin6, &addr->addr, sizeof(sin6));
            std::memcpy(bytes, &sin6.sin6_addr, 16);
            len = 16;
        } else {
            c->ip_refused = true;
            return CURL_SOCKET_BAD;
        }
        if (ip_is_non_public(bytes, len)) {
            c->ip_refused = true;
            return CURL_SOCKET_BAD;
        }
    }
    return socket(addr->family, addr->socktype, addr->protocol);
}

// One DNS cache and TLS session cache for every thread. The connections themselves live in the transfer loop's
// multi handle below (libcurl's shared connection pool isn't safe to use from several threads at once).
// Never freed: the thread_local easy handles below may be destroyed after static destructors run.
struct CurlShare {
    CURLSH*    sh = nullptr;
    std::mutex locks[CURL_LOCK_DATA_LAST];
    static void lock(CURL*, curl_lock_data d, curl_lock_access, void* u) { static_cast<CurlShare*>(u)->locks[d].lock(); }
    static void unlock(CURL*, curl_lock_data d, void* u) { static_cast<CurlShare*>(u)->locks[d].unlock(); }
    CurlShare() {
        sh = curl_share_init();
        if (!sh) return;
        curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, &CurlShare::lock);
        curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, &CurlShare::unlock);
        curl_share_setopt(sh, CURLSHOPT_USERDATA, this);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
};
CURLSH* shared_curl() {
    static CurlShare* s = new CurlShare(); // created on first fetch, after curl_global_init
    return s->sh;
}

struct CurlHolder {
    CURL* h = nullptr;
    ~CurlHolder() {
        if (h) curl_easy_cleanup(h);
    }
    CURL* get() {
        if (!h) h = curl_easy_init();
        else curl_easy_reset(h);
        return h;
    }
};
thread_local CurlHolder tl_curl;

// Every transfer runs on one thread, in one curl multi handle: one connection pool for every tab, frame and worker,
// so requests to a site share its connections, and over HTTP/2 run side by side on a single one (multiplexed).
// Callers still block in fetch(): their easy handle is handed to the loop and they wait for it to finish.
// (QUILL_NO_MULTI=1: each transfer on its caller's thread instead, as before; for comparison.)
class TransferLoop {
public:
    static TransferLoop& instance() {
        static TransferLoop* t = new TransferLoop(); // leaked on purpose: transfers may still finish while Quill exits
        return *t;
    }
    CURLcode perform(CURL* h) {
        if (!multi_) return curl_easy_perform(h);
        Job j;
        j.h = h;
        curl_easy_setopt(h, CURLOPT_PRIVATE, &j);
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(&j);
        }
        curl_multi_wakeup(multi_);
        std::unique_lock<std::mutex> lk(j.m);
        j.cv.wait(lk, [&] { return j.done; });
        return j.rc;
    }

private:
    struct Job {
        CURL*                   h = nullptr;
        std::mutex              m;
        std::condition_variable cv;
        bool                    done = false;
        CURLcode                rc   = CURLE_OK;
    };
    TransferLoop() {
#if LIBCURL_VERSION_NUM < 0x074400 // curl_multi_poll / curl_multi_wakeup: 7.68
        return;
#endif
        if (std::getenv("QUILL_NO_MULTI")) return;
        multi_ = curl_multi_init();
        if (!multi_) return;
        curl_multi_setopt(multi_, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
        curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, 6L); // what browsers allow per host over HTTP/1.1
        curl_multi_setopt(multi_, CURLMOPT_MAXCONNECTS, 64L);          // idle connections kept for reuse
        std::thread([this] { run(); }).detach();
    }
    void finish(Job* j, CURLcode rc) {
        std::lock_guard<std::mutex> lk(j->m);
        j->rc   = rc;
        j->done = true;
        j->cv.notify_one();
    }
    void run() {
        for (;;) {
            std::vector<Job*> add;
            {
                std::lock_guard<std::mutex> lk(mu_);
                add.swap(pending_);
            }
            for (Job* j : add)
                if (curl_multi_add_handle(multi_, j->h) != CURLM_OK) finish(j, CURLE_FAILED_INIT);
            int running = 0;
            curl_multi_perform(multi_, &running);
            int      left = 0;
            CURLMsg* msg;
            while ((msg = curl_multi_info_read(multi_, &left))) {
                if (msg->msg != CURLMSG_DONE) continue;
                CURL*    e  = msg->easy_handle;
                CURLcode rc = msg->data.result;
                char*    p  = nullptr;
                curl_easy_getinfo(e, CURLINFO_PRIVATE, &p);
                curl_multi_remove_handle(multi_, e);
                if (p) finish(reinterpret_cast<Job*>(p), rc);
            }
            curl_multi_poll(multi_, nullptr, 0, 1000, nullptr);
        }
    }
    CURLM*            multi_ = nullptr;
    std::mutex        mu_;
    std::vector<Job*> pending_;
};

std::once_flag g_curl_once;

bool env_nonempty(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

bool g_netlog = std::getenv("QUILL_NETLOG") != nullptr;

// QUILL_NETLOG=1: one line per request on stderr, with where the time went.
void log_transfer(CURL* h, const char* method, const std::string& url, CURLcode rc, long status, size_t bytes, bool doh) {
    curl_off_t dns = 0, conn = 0, tls = 0, ttfb = 0, total = 0;
    long       ver = 0, reused = 0;
#if LIBCURL_VERSION_NUM >= 0x073d00
    curl_easy_getinfo(h, CURLINFO_NAMELOOKUP_TIME_T, &dns);
    curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T, &conn);
    curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME_T, &tls);
    curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME_T, &ttfb);
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME_T, &total);
#endif
    curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &ver);
    curl_easy_getinfo(h, CURLINFO_NUM_CONNECTS, &reused); // new connections this transfer made
    reused = reused == 0;
    char*       ip = nullptr;
    curl_easy_getinfo(h, CURLINFO_PRIMARY_IP, &ip);
    const char* v  = ver == CURL_HTTP_VERSION_2_0 ? "h2" : ver == CURL_HTTP_VERSION_1_1 ? "h1.1" : ver == CURL_HTTP_VERSION_1_0 ? "h1.0" :
#if LIBCURL_VERSION_NUM >= 0x074200
                     ver == CURL_HTTP_VERSION_3 ? "h3" :
#endif
                     "-";
    auto ms = [](curl_off_t us) { return static_cast<long long>(us / 1000); };
    std::fprintf(stderr, "[net] %s %ld %s %s  dns%s %lldms  connect %lldms  tls %lldms  first byte %lldms  total %lldms  %zu B%s ip %s%s%s\n",
                 method, status, v, url.substr(0, 160).c_str(), doh ? "(doh)" : "", ms(dns), ms(conn), ms(tls), ms(ttfb), ms(total), bytes,
                 reused ? "  (reused connection)" : "", ip ? ip : "-", rc != CURLE_OK ? "  error: " : "", rc != CURLE_OK ? curl_easy_strerror(rc) : "");
}


std::string slist_find(const struct curl_slist* l, const char* prefix) {
    size_t n = std::strlen(prefix);
    for (; l; l = l->next)
        if (l->data && std::strncmp(l->data, prefix, n) == 0) return l->data + n;
    return {};
}

// Connects to `url` WITHOUT verifying the certificate, only to read the certificate the server presents.
// Nothing is sent to the server beyond the TLS handshake and a HEAD-less (no request) connection.
bool probe_certificate(const std::string& url, const Settings& st, FetchResult& out) {
    CURL* h = curl_easy_init();
    if (!h) return false;
    std::string sink;
    Ctx         ctx;
    ctx.body      = &sink;
    ctx.max_bytes = 1;
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L); // handshake + (at most) a HEAD; the response is discarded
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CERTINFO, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, st.user_agent.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    if (!st.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, st.proxy.c_str());
    curl_easy_perform(h); // the result does not matter, only whether a certificate was captured
    struct curl_certinfo* ci = nullptr;
    bool                  ok = false;
    if (curl_easy_getinfo(h, CURLINFO_CERTINFO, &ci) == CURLE_OK && ci && ci->num_of_certs > 0) {
        std::string pem = slist_find(ci->certinfo[0], "Cert:");
        out.cert_pin     = spki_pin_from_pem(pem);
        out.cert_subject = slist_find(ci->certinfo[0], "Subject:");
        out.cert_issuer  = slist_find(ci->certinfo[0], "Issuer:");
        out.cert_expires = slist_find(ci->certinfo[0], "Expire date:");
        ok               = !out.cert_pin.empty();
    }
    curl_easy_cleanup(h);
    return ok;
}

// Cookies are sent to / stored from a request only when it is first-party (see cookies.h).
bool cookies_allowed(const Settings& st, const FetchRequest& req, const ParsedUrl& target, bool is_post) {
    if (!st.cookies) return false;
    std::string init_host;
    if (!req.initiator.empty()) {
        ParsedUrl iu;
        if (parse_url(req.initiator, iu)) init_host = iu.host;
    }
    if (req.kind == RequestKind::TopLevel) {
        if (!is_post || init_host.empty()) return true;
        return same_site(init_host, target.host); // a cross-site POST never carries cookies
    }
    return !init_host.empty() && same_site(init_host, target.host);
}

} // namespace

Network::Network(const Settings& s, std::shared_ptr<RequestFilter> filter) :
    settings_(s),
    filter_(filter ? std::move(filter) : std::make_shared<RequestFilter>()) {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    HttpCache::instance().set_capacity(static_cast<size_t>(s.http_cache_mb) << 20);
    trusted_certs_ = s.trusted_certs;
    proxy_in_use_ = !s.proxy.empty() || env_nonempty("https_proxy") || env_nonempty("HTTPS_PROXY") ||
                    env_nonempty("http_proxy") || env_nonempty("HTTP_PROXY") || env_nonempty("all_proxy") ||
                    env_nonempty("ALL_PROXY");
}

Network::~Network() = default;

void Network::allow_http_host(const std::string& host) {
    std::lock_guard<std::mutex> lk(mu_);
    session_http_allow_.insert(normalize_host(host));
}

bool Network::trusts_cert(const std::string& host) const {
    std::lock_guard<std::mutex> lk(mu_);
    return trusted_certs_.count(normalize_host(host)) != 0;
}

void Network::trust_cert(const std::string& host, const std::string& pin) {
    if (!is_valid_pin(pin)) return;
    if (!policy().is_private_host(host)) return; // exceptions exist for LAN devices only, never the public internet
    std::lock_guard<std::mutex> lk(mu_);
    trusted_certs_[normalize_host(host)] = pin;
}

Policy Network::policy() const {
    Policy p;
    p.doh_url         = settings_.doh_url;
    p.doh_fallback    = settings_.doh_fallback;
    p.http_allow      = settings_.http_allow;
    p.private_allow   = settings_.private_allow;
    p.http_warn       = settings_.http_warning == "never" ? Policy::HttpWarn::Never
                      : settings_.http_warning == "always" ? Policy::HttpWarn::Always : Policy::HttpWarn::Typed;
    std::lock_guard<std::mutex> lk(mu_);
    p.http_allow.insert(session_http_allow_.begin(), session_http_allow_.end());
    return p;
}

void Network::preconnect(const std::string& url) {
    ParsedUrl pu;
    if (!parse_url(url, pu) || (pu.scheme != "https" && pu.scheme != "http")) return;
    Policy   pol = policy();
    Hsts::instance().upgrade(pu);
    Decision d = evaluate_request(pol, pu, RequestKind::TopLevel, std::string(), false, std::string());
    if (d.verdict != Verdict::Allow) return;
    std::string u = serialize_url(pu);
    CURL*       h = tl_curl.get();
    if (!h) return;
    Ctx         ctx;
    std::string sink;
    ctx.body             = &sink;
    ctx.check_ip         = !proxy_in_use_;
    ctx.allow_private_ip = d.allow_private_ip;
    if (CURLSH* sh = shared_curl()) curl_easy_setopt(h, CURLOPT_SHARE, sh);
    curl_easy_setopt(h, CURLOPT_URL, u.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
#ifdef _WIN32
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    if (!settings_.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, settings_.proxy.c_str());
    if (d.use_doh && !pol.doh_url.empty()) curl_easy_setopt(h, CURLOPT_DOH_URL, pol.doh_url.c_str());
    curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, open_cb);
    curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, &ctx);
    CURLcode rc = TransferLoop::instance().perform(h);
    if (g_netlog) log_transfer(h, "PRECONNECT", u, rc, 0, 0, d.use_doh && !pol.doh_url.empty());
    curl_easy_reset(h); // drop the connect-only connection
}

FetchResult Network::fetch(const FetchRequest& req) {
    FetchResult res;
    Policy      pol = policy();

    std::string url = req.url;
    std::string prev;
    bool        post = req.method == "POST";
    std::string body = post ? req.body : std::string();
    // the page this comes from: for Referer and Sec-Fetch-Site
    std::string from = request_referrer_source(req);
    const char* site = nullptr; // Sec-Fetch-Site, the least related hop so far

    std::string          cache_key;
    HttpCache::Look      look = HttpCache::Look::Miss;
    CachedResponse       cached;
    auto                 from_cache = [](const CachedResponse& c) {
        FetchResult r;
        r.ok           = true;
        r.status       = 200;
        r.final_url    = c.url;
        r.content_type = c.content_type;
        r.charset      = c.charset;
        r.body         = c.body;
        return r;
    };
    for (int hop = 0; hop <= kMaxRedirects; hop++) {
        res.final_url = url;
        ParsedUrl pu;
        if (!parse_url(url, pu)) {
            res.error = "Invalid URL: " + url;
            return res;
        }
        if (Hsts::instance().upgrade(pu)) { // an HTTPS-only site: never send it anything over plain http
            url           = serialize_url(pu);
            res.final_url = url;
        }
        if (filter_->should_block(url, req.initiator, req.type)) {
            res.blocked = true;
            res.error   = "Blocked by content filter";
            return res;
        }
        Decision d = evaluate_request(pol, pu, req.kind, hop == 0 ? req.initiator : prev, hop > 0, hop == 0 ? req.source_page : std::string());
        if (d.verdict == Verdict::Block) {
            res.blocked = true;
            res.error   = d.reason;
            return res;
        }
        if (d.verdict == Verdict::WarnHttp) {
            res.needs_http_confirm = true;
            res.error              = d.reason;
            return res;
        }
        // A form on a public page may not submit into the private network (router CSRF and friends).
        if (req.form_submit && hop == 0 && d.allow_private_ip && !req.initiator.empty()) {
            ParsedUrl init;
            if (parse_url(req.initiator, init) && init.has_authority && !pol.is_private_host(init.host)) {
                res.blocked = true;
                res.error   = "Blocked: a form on a public web page tried to submit to a private network address (" +
                            normalize_host(pu.host) + ")";
                return res;
            }
        }

        std::string host    = normalize_host(pu.host);
        std::string pin;
        if (pu.scheme == "https" && d.allow_private_ip) {
            std::lock_guard<std::mutex> lk(mu_);
            auto                        it = trusted_certs_.find(host);
            if (it != trusted_certs_.end()) pin = it->second;
        }
        bool use_cookies = cookies_allowed(settings_, req, pu, post);

        // the HTTP cache: stylesheets, pictures and fonts, by the page's site and the address asked for
        bool cacheable = settings_.http_cache && !post && hop == 0 && req.kind == RequestKind::Subresource &&
                         (req.type == ResourceType::Stylesheet || req.type == ResourceType::Image || req.type == ResourceType::Font);
        if (cacheable) {
            ParsedUrl ip;
            std::string site = parse_url(req.initiator, ip) ? registrable_domain(normalize_host(ip.host)) : std::string();
            cache_key        = HttpCache::key(site, url);
            look             = HttpCache::instance().lookup(cache_key, std::time(nullptr), cached);
            if (look == HttpCache::Look::Fresh) {
                if (g_netlog) std::fprintf(stderr, "[net] GET (cached) %s\n", url.substr(0, 160).c_str());
                return from_cache(cached);
            }
        }

        std::string doh_url = d.use_doh ? pol.doh_url : std::string();
        bool        fresh   = false; // the retry of a request a stale pooled connection swallowed: a new connection
        for (int attempt = 0; attempt < 2; attempt++) {
            res.body.clear();
            Ctx ctx;
            ctx.body             = &res.body;
            ctx.max_bytes        = req.max_bytes;
            ctx.cancel           = req.cancel;
            ctx.check_ip         = !proxy_in_use_;
            ctx.allow_private_ip = d.allow_private_ip;

            CURL* h = tl_curl.get();
            if (!h) {
                res.error = "Could not initialise libcurl";
                return res;
            }
            if (CURLSH* sh = shared_curl()) curl_easy_setopt(h, CURLOPT_SHARE, sh); // (curl_easy_reset clears it)
            if (fresh) curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, 1L);
            char errbuf[CURL_ERROR_SIZE] = {0};
            curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
            curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
            curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
            curl_easy_setopt(h, CURLOPT_USERAGENT, settings_.user_agent.c_str());
            curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // all encodings libcurl supports
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, settings_.timeout_ms);
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
            curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L); // 10 min (curl's default, 1 min, meant a fresh DoH lookup per minute per host)
            curl_easy_setopt(h, CURLOPT_MAXCONNECTS, 32L);         // keep more idle connections: a page talks to many hosts
            curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
            curl_easy_setopt(h, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
            curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(req.max_bytes));
#ifdef _WIN32
            curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
            if (!pin.empty()) {
                // The user trusted this private host's key: skip CA validation, but only this exact key passes.
                curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(h, CURLOPT_PINNEDPUBLICKEY, pin.c_str());
            }
            if (!settings_.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, settings_.proxy.c_str());
            if (!doh_url.empty()) curl_easy_setopt(h, CURLOPT_DOH_URL, doh_url.c_str());

            struct curl_slist* hdrs = nullptr;
            for (const std::string& hl : request_headers(req, from, pu, settings_.referrer, site)) hdrs = curl_slist_append(hdrs, hl.c_str());
            if (look == HttpCache::Look::Stale) { // ask whether our copy is still good
                if (!cached.etag.empty()) hdrs = curl_slist_append(hdrs, ("If-None-Match: " + cached.etag).c_str());
                if (!cached.last_modified.empty()) hdrs = curl_slist_append(hdrs, ("If-Modified-Since: " + cached.last_modified).c_str());
            }
            if (use_cookies) {
                std::string ck = CookieJar::instance().header_for(pu);
                if (!ck.empty()) hdrs = curl_slist_append(hdrs, ("Cookie: " + ck).c_str());
            }
            if (post) {
                curl_easy_setopt(h, CURLOPT_POST, 1L);
                curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
                curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
                hdrs = curl_slist_append(hdrs, ("Content-Type: " + (req.content_type.empty() ? std::string("application/x-www-form-urlencoded") : req.content_type)).c_str());
                hdrs = curl_slist_append(hdrs, "Expect:");
                ParsedUrl init;
                if (!req.initiator.empty() && parse_url(req.initiator, init) && init.has_authority) {
                    std::string origin = init.scheme + "://" + (init.host.find(':') != std::string::npos ? "[" + init.host + "]" : init.host) +
                                         (init.port > 0 ? ":" + std::to_string(init.port) : "");
                    hdrs = curl_slist_append(hdrs, ("Origin: " + origin).c_str());
                }
            }
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
            curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfer_cb);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
            curl_easy_setopt(h, CURLOPT_OPENSOCKETFUNCTION, open_cb);
            curl_easy_setopt(h, CURLOPT_OPENSOCKETDATA, &ctx);
            if (pu.scheme == "https") curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L); // rather share a starting HTTP/2 connection than open another
            // HTTP authentication the user gave for this origin
            HttpCredentials cred;
            bool            sent_cred = may_send_credentials(req, url) && CredentialStore::instance().get(url_origin(url), cred);
            if (sent_cred) {
                curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(cred.digest ? CURLAUTH_DIGEST : CURLAUTH_BASIC));
                curl_easy_setopt(h, CURLOPT_USERNAME, cred.user.c_str());
                curl_easy_setopt(h, CURLOPT_PASSWORD, cred.pass.c_str());
            }

            CURLcode rc     = TransferLoop::instance().perform(h);
            long     status = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
            if (g_netlog) log_transfer(h, post ? "POST" : "GET", url, rc, status, res.body.size(), !doh_url.empty());
            curl_slist_free_all(hdrs);

            if (rc != CURLE_OK) {
                if (ctx.cancel && ctx.cancel->load()) {
                    res.cancelled = true;
                    res.error     = "Cancelled";
                    return res;
                }
                if (ctx.ip_refused) {
                    res.blocked = true;
                    res.error   = "Blocked: " + normalize_host(pu.host) +
                                " resolved to a private network address (possible DNS rebinding attack)";
                    return res;
                }
                if (ctx.too_big) {
                    res.error = "Response too large";
                    return res;
                }
                // A kept-alive connection the server (or a NAT box) had already dropped fails with nothing received;
                // browsers send the request again on a new connection. Only for GET: a POST may have arrived.
                long new_conns = 1;
                curl_easy_getinfo(h, CURLINFO_NUM_CONNECTS, &new_conns);
                bool stale = rc == CURLE_GOT_NOTHING || rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR || rc == CURLE_HTTP2 ||
                             rc == CURLE_HTTP2_STREAM;
                if (stale && !post && attempt == 0 && new_conns == 0 && res.body.empty() && status == 0) {
                    fresh = true;
                    if (g_netlog) std::fprintf(stderr, "[net] retrying on a new connection: %s\n", url.substr(0, 160).c_str());
                    continue;
                }
                if (rc == CURLE_COULDNT_RESOLVE_HOST && !doh_url.empty() && pol.doh_fallback && attempt == 0) {
                    doh_url.clear(); // retry once with the system resolver
                    continue;
                }
                std::string msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
                if (rc == CURLE_PEER_FAILED_VERIFICATION && pu.scheme == "https" && d.allow_private_ip && pin.empty() &&
                    req.kind == RequestKind::TopLevel) {
                    // A LAN device with a self-signed (or otherwise unverifiable) certificate. Read what it presents so the
                    // user can decide; nothing is trusted until they say so.
                    FetchResult probe;
                    std::string pu_str = "https://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) +
                                         (pu.port > 0 ? ":" + std::to_string(pu.port) : "") + "/";
                    if (probe_certificate(pu_str, settings_, probe)) {
                        res.needs_cert_confirm = true;
                        res.cert_host          = host;
                        res.cert_pin           = probe.cert_pin;
                        res.cert_subject       = probe.cert_subject;
                        res.cert_issuer        = probe.cert_issuer;
                        res.cert_expires       = probe.cert_expires;
                        res.cert_error         = msg;
                        res.error              = msg;
                        return res;
                    }
                    msg += "  (This is a private host, but Quill could not read its certificate with this TLS backend, so it cannot offer to trust it.)";
                }
                if (rc == CURLE_SSL_PINNEDPUBKEYNOTMATCH) {
                    res.blocked = true;
                    msg = "The certificate presented by " + host + " is not the one you trusted. It may have been replaced, or "
                          "someone on your network may be intercepting the connection. (" + msg + ")";
                }
                if (rc == CURLE_COULDNT_RESOLVE_HOST && !doh_url.empty())
                    msg += "  (looked up via DNS-over-HTTPS: " + doh_url + ")";
                res.error = msg;
                return res;
            }

            if (use_cookies)
                for (const auto& sc : ctx.set_cookies) CookieJar::instance().store(pu, sc);
            // HSTS only counts over a properly verified connection (not a LAN device trusted by its key)
            if (!ctx.hsts.empty() && pu.scheme == "https" && pin.empty()) Hsts::instance().note(pu.host, ctx.hsts);

            res.status = static_cast<int>(status);
            res.ok     = true;
            if (status == 304 && look == HttpCache::Look::Stale) { // still good
                CachedResponse fresh;
                if (HttpCache::instance().refresh(cache_key, ctx.cache, std::time(nullptr), fresh)) return from_cache(fresh);
            }
            if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !ctx.location.empty()) {
                prev = url;
                url  = resolve_url(url, ctx.location);
                res.ok = false; // not final yet
                if (post && status != 307 && status != 308) { // browsers turn these into a GET without a body
                    post = false;
                    body.clear();
                }
                goto next_hop;
            }

            // final response
            res.frame_options   = ctx.frame_options;
            res.csp             = ctx.csp;
            res.referrer_policy = ctx.referrer_policy;
            if (status == 401 && parse_www_authenticate(ctx.www_authenticate, res.auth_scheme, res.auth_realm)) {
                res.auth_required = true;
                res.auth_failed   = sent_cred;
            }
            {
                std::string ct = ctx.content_type;
                size_t      sc = ct.find(';');
                res.content_type = to_lower(trim(sc == std::string::npos ? ct : ct.substr(0, sc)));
                if (sc != std::string::npos) {
                    std::string params = to_lower(ct.substr(sc + 1));
                    size_t      cs     = params.find("charset=");
                    if (cs != std::string::npos) {
                        std::string v = params.substr(cs + 8);
                        size_t      e = v.find(';');
                        if (e != std::string::npos) v.erase(e);
                        v = trim(v);
                        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) v.erase(0, 1);
                        if (!v.empty() && (v.back() == '"' || v.back() == '\'')) v.pop_back();
                        res.charset = v;
                    }
                }
                res.final_url = url;
            }
            if (cacheable && HttpCache::storable(res.status, ctx.cache)) {
                CachedResponse c;
                std::time_t    now = std::time(nullptr);
                c.url           = res.final_url;
                c.content_type  = res.content_type;
                c.charset       = res.charset;
                c.body          = res.body;
                c.etag          = ctx.cache.etag;
                c.last_modified = ctx.cache.last_modified;
                c.stored        = now;
                c.fresh_until   = HttpCache::freshness(ctx.cache, now);
                HttpCache::instance().store(cache_key, std::move(c));
            }
            return res;
        }
    next_hop:;
    }
    res.ok    = false;
    res.error = "Too many redirects";
    return res;
}

} // namespace quill
