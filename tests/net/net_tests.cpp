// Network tests against a small HTTP/1.1 server on 127.0.0.1 (real libcurl, real sockets): connection reuse across
// threads, the per-host connection limit, the HTTP cache's revalidation, HTTP authentication.
#include "net.h"
#include "httpcache.h"
#include "settings.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(c)) { g_fails++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

// ---- the server: keep-alive HTTP/1.1, one thread per connection
struct Server {
    int                      fd = -1, port = 0;
    std::atomic<int>         connections{0}, open_now{0}, max_open{0}, requests{0};
    std::mutex               mu;
    std::vector<std::string> seen; // request heads, in order
    std::atomic<bool>        stop{false};

    bool start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 64) != 0) return false;
        socklen_t l = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        std::thread([this] {
            for (;;) {
                int c = accept(fd, nullptr, nullptr);
                if (c < 0) return;
                connections++;
                int o = ++open_now;
                for (int m = max_open.load(); o > m && !max_open.compare_exchange_weak(m, o);) {}
                std::thread([this, c] { serve(c); open_now--; }).detach();
            }
        }).detach();
        return true;
    }
    static std::string header(const std::string& head, const std::string& name) {
        std::string l = head, n = name + ":";
        for (auto& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        size_t p = l.find("\r\n" + n);
        if (p == std::string::npos) return {};
        size_t v = p + 2 + n.size(), e = head.find("\r\n", v);
        std::string r = head.substr(v, e - v);
        while (!r.empty() && r[0] == ' ') r.erase(0, 1);
        return r;
    }
    void reply(int c, int status, const std::string& extra, const std::string& body) {
        const char* reason = status == 200 ? "OK" : status == 304 ? "Not Modified" : status == 401 ? "Unauthorized" : "Not Found";
        std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Length: " + std::to_string(body.size()) +
                        "\r\nContent-Type: text/plain\r\n" + extra + "\r\n" + body;
        send(c, r.data(), r.size(), MSG_NOSIGNAL);
    }
    void serve(int c) {
        std::string buf;
        char        tmp[4096];
        for (;;) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                ssize_t n = recv(c, tmp, sizeof tmp, 0);
                if (n <= 0) { close(c); return; }
                buf.append(tmp, static_cast<size_t>(n));
            }
            std::string head = buf.substr(0, end + 2);
            buf.erase(0, end + 4);
            requests++;
            {
                std::lock_guard<std::mutex> lk(mu);
                seen.push_back(head);
            }
            std::string path = head.substr(head.find(' ') + 1);
            path             = path.substr(0, path.find(' '));
            if (path == "/slow") {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                reply(c, 200, "", "slow");
            } else if (path == "/secret" || path == "/secret.css") {
                // Basic test:hunter2
                if (header(head, "Authorization") == "Basic dGVzdDpodW50ZXIy") reply(c, 200, "", "the secret");
                else reply(c, 401, "WWW-Authenticate: Basic realm=\"Test \\\"Realm\\\"\", charset=\"UTF-8\"\r\n", "denied");
            } else if (path == "/style.css") { // cacheable for a moment, then revalidated
                if (header(head, "If-None-Match") == "\"v1\"") reply(c, 304, "ETag: \"v1\"\r\nCache-Control: max-age=1\r\n", "");
                else reply(c, 200, "ETag: \"v1\"\r\nCache-Control: max-age=1\r\nContent-Type: text/css\r\n", "body{color:red}");
            } else {
                reply(c, 200, "", "hello " + path);
            }
        }
    }
    int count_heads_with(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (auto& h : seen) n += h.find(needle) != std::string::npos;
        return n;
    }
};

static FetchResult get(Network& net, const std::string& url, RequestKind kind = RequestKind::TopLevel, ResourceType type = ResourceType::Document,
                       const std::string& initiator = "") {
    FetchRequest r;
    r.url       = url;
    r.kind      = kind;
    r.type      = type;
    r.initiator = initiator;
    return net.fetch(r);
}

int main() {
    Server srv;
    if (!srv.start()) { std::printf("can't listen\n"); return 1; }
    std::string base = "http://127.0.0.1:" + std::to_string(srv.port);
    Settings    st;
    st.doh_url = "";
    st.http_cache = true;
    Network net(st, nullptr);

    // 1. one connection, reused by requests from different threads
    for (int t = 0; t < 3; t++) {
        std::thread([&] {
            for (int i = 0; i < 3; i++) {
                FetchResult r = get(net, base + "/a" + std::to_string(i));
                CHECK(r.ok && r.status == 200 && r.body == "hello /a" + std::to_string(i));
            }
        }).join();
    }
    CHECK(srv.connections.load() == 1);

    // 2. many at once: they all finish, over at most 6 connections (HTTP/1.1 can't share one)
    {
        std::vector<std::thread> ts;
        std::atomic<int>         ok{0};
        auto                     t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 12; i++) ts.emplace_back([&] { if (get(net, base + "/slow").body == "slow") ok++; });
        for (auto& t : ts) t.join();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok.load() == 12);
        CHECK(srv.max_open.load() <= 6 + 1); // (+1: the first test's idle connection)
        CHECK(secs < 1.5);                     // in parallel: 12 x 300 ms in two rounds, not twelve
        std::printf("12 slow requests: %.2f s, %d connections at most\n", secs, srv.max_open.load());
    }

    // 3. HTTP authentication
    {
        FetchResult r = get(net, base + "/secret");
        CHECK(r.status == 401 && r.auth_required && !r.auth_failed && r.auth_scheme == "basic");
        CHECK(r.auth_realm == "Test \"Realm\"");
        HttpCredentials bad;
        bad.user = "test";
        bad.pass = "wrong";
        CredentialStore::instance().set(url_origin(base), bad);
        r = get(net, base + "/secret");
        CHECK(r.status == 401 && r.auth_required && r.auth_failed);
        HttpCredentials good;
        good.user = "test";
        good.pass = "hunter2";
        CredentialStore::instance().set(url_origin(base), good);
        r = get(net, base + "/secret");
        CHECK(r.ok && r.status == 200 && r.body == "the secret" && !r.auth_required);
        // its own pages' requests get them too...
        r = get(net, base + "/secret.css", RequestKind::Subresource, ResourceType::Stylesheet, base + "/page");
        CHECK(r.status == 200);
        // ...another site's page's don't
        int before = srv.count_heads_with("Authorization:");
        r = get(net, base + "/secret.css", RequestKind::Subresource, ResourceType::Stylesheet, "http://localhost:1/evil");
        CHECK(r.status == 401);
        CHECK(srv.count_heads_with("Authorization:") == before);
        CredentialStore::instance().clear();
    }

    // 4. the HTTP cache: fresh -> no request; stale -> revalidated with a 304
    {
        int         n0 = srv.requests.load();
        std::string page = base + "/index.html";
        FetchResult r = get(net, base + "/style.css", RequestKind::Subresource, ResourceType::Stylesheet, page);
        CHECK(r.ok && r.body == "body{color:red}" && srv.requests.load() == n0 + 1);
        r = get(net, base + "/style.css", RequestKind::Subresource, ResourceType::Stylesheet, page);
        CHECK(r.ok && r.body == "body{color:red}" && srv.requests.load() == n0 + 1); // from the cache
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        r = get(net, base + "/style.css", RequestKind::Subresource, ResourceType::Stylesheet, page);
        CHECK(r.ok && r.status == 200 && r.body == "body{color:red}" && srv.requests.load() == n0 + 2);
        CHECK(srv.count_heads_with("If-None-Match: \"v1\"") == 1);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
