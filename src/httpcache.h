// An in-memory HTTP cache for subresources (stylesheets, images, fonts), following RFC 9111 as a private cache:
// Cache-Control (max-age, no-cache, no-store), Expires, Age, the usual heuristic for responses that only have a
// Last-Modified, and revalidation with If-None-Match / If-Modified-Since (a 304 refreshes the stored copy).
//
// Entries are keyed by the top-level site that loaded them as well as the URL (as browsers partition their caches),
// so one site can't tell from timing whether you visited another. Capped in size (least recently used go first);
// thread-safe; lives for the session. Documents are not cached (Quill keeps pages for back / forward itself).
#pragma once

#include <cstdint>
#include <ctime>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace quill {

struct CachedResponse {
    std::string url;          // final URL (after redirects), what the response says it is
    std::string content_type; // as FetchResult has them
    std::string charset;
    std::string body;
    std::string etag, last_modified;
    std::time_t stored = 0;   // when it was received (or last revalidated)
    std::time_t fresh_until = 0; // fresh while now < this (0 = must revalidate)
};

// What a response said about caching, from its headers.
struct CacheHeaders {
    std::string cache_control, expires, date, last_modified, etag, age, vary, pragma;
};

class HttpCache {
public:
    static HttpCache& instance();
    // The cache key: `site` is the top-level page's site (registrable domain) or "" for none.
    static std::string key(const std::string& site, const std::string& url);

    // Whether a response may be stored (status 200, nothing in Cache-Control or Vary forbidding it).
    static bool storable(int status, const CacheHeaders& h);
    // When a response received at `now` stops being fresh (0 = immediately: store only to revalidate).
    static std::time_t freshness(const CacheHeaders& h, std::time_t now);

    enum class Look { Miss, Fresh, Stale }; // Stale: send `entry`'s validators and use it on a 304
    Look lookup(const std::string& key, std::time_t now, CachedResponse& entry);
    void store(const std::string& key, CachedResponse e);
    // A 304 for `key`: the stored copy is fresh again (with the new headers' lifetime).
    bool refresh(const std::string& key, const CacheHeaders& h, std::time_t now, CachedResponse& out);
    void clear();
    size_t bytes() const;
    void set_capacity(size_t bytes);

private:
    struct Slot {
        CachedResponse                  r;
        std::list<std::string>::iterator lru;
    };
    void evict_locked();
    mutable std::mutex                    mu_;
    std::unordered_map<std::string, Slot> map_;
    std::list<std::string>                lru_; // front = most recently used
    size_t                                bytes_ = 0, cap_ = 64u << 20;
};

} // namespace quill
