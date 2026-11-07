// A small on-disk cache for things worth keeping between sessions: sanitized web fonts and site icons. Entries are
// files named by the SHA-256 of their key, one folder per bucket ("fonts", "icons"), each bucket capped in size
// (oldest files go first). Nothing is ever executed or parsed from here without the same checks a download gets.
//
// Thread-safe. Disabled (every get() misses, put() does nothing) until configure() gets a directory and enabled=true.
#pragma once

#include <mutex>
#include <string>

namespace quill {

class DiskCache {
public:
    static DiskCache& instance();

    void configure(const std::string& dir, bool enabled);
    bool enabled() const;

    // The entry for `key` if present and younger than `max_age_s`.
    bool get(const std::string& bucket, const std::string& key, std::string& data, long long max_age_s);
    // Stores (replaces) an entry, then trims the bucket to `bucket_cap` bytes.
    void put(const std::string& bucket, const std::string& key, const std::string& data, size_t bucket_cap);
    // Removes everything (all buckets).
    void clear();

private:
    std::string path_for(const std::string& bucket, const std::string& key) const;
    mutable std::mutex mu_;
    std::string        dir_;
    bool               enabled_ = false;
};

} // namespace quill
