#include "diskcache.h"

#include "certpin.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace quill {

namespace {
std::string hex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string        o;
    for (unsigned char c : raw) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}
bool safe_bucket(const std::string& b) {
    if (b.empty() || b.size() > 32) return false;
    for (char c : b)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
} // namespace

DiskCache& DiskCache::instance() {
    static DiskCache* c = new DiskCache(); // leaked on purpose: used by workers until exit
    return *c;
}

void DiskCache::configure(const std::string& dir, bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    dir_     = dir;
    enabled_ = enabled && !dir.empty();
}

bool DiskCache::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

std::string DiskCache::path_for(const std::string& bucket, const std::string& key) const {
    return (fs::path(dir_) / bucket / hex(sha256_bytes(key))).string();
}

bool DiskCache::get(const std::string& bucket, const std::string& key, std::string& data, long long max_age_s) {
    std::string p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_ || !safe_bucket(bucket)) return false;
        p = path_for(bucket, key);
    }
    std::error_code ec;
    auto            mt = fs::last_write_time(p, ec);
    if (ec) return false;
    auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - mt).count();
    if (age > max_age_s || age < -86400) {
        fs::remove(p, ec);
        return false;
    }
    auto size = fs::file_size(p, ec);
    if (ec || size > (64u << 20)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    data = ss.str();
    return data.size() == size;
}

void DiskCache::put(const std::string& bucket, const std::string& key, const std::string& data, size_t bucket_cap) {
    std::string p, dir;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_ || !safe_bucket(bucket) || data.size() > bucket_cap) return;
        p   = path_for(bucket, key);
        dir = (fs::path(dir_) / bucket).string();
    }
    std::error_code ec;
    fs::path        root = fs::path(dir).parent_path();
    bool            made_root = fs::create_directories(root, ec);
    if (ec) return;
    bool made_bucket = fs::create_directories(dir, ec);
    if (ec) return;
#ifndef _WIN32
    // folders Quill creates are private to the user (never touch the permissions of one the user pointed us at)
    if (made_root) fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (made_bucket) fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#else
    (void)made_root;
    (void)made_bucket;
#endif
    std::string tmp = p + ".tmp" + std::to_string(std::hash<std::string>()(key) & 0xffff);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) { out.close(); fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, p, ec); // atomic: a reader sees the old entry or the new one, never half a file
    if (ec) { fs::remove(tmp, ec); return; }

    // trim the bucket: oldest first until it fits
    struct E { fs::path p; uintmax_t size; fs::file_time_type t; };
    std::vector<E> es;
    uintmax_t      total = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        E e{it->path(), it->file_size(e2), it->last_write_time(e2)};
        if (e2) continue;
        total += e.size;
        es.push_back(std::move(e));
    }
    if (total <= bucket_cap) return;
    std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.t < b.t; });
    for (auto& e : es) {
        if (total <= bucket_cap) break;
        if (e.p.string() == p) continue;
        fs::remove(e.p, ec);
        total -= e.size;
    }
}

void DiskCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty()) return;
    std::error_code ec;
    for (const char* b : {"fonts", "icons"}) fs::remove_all(fs::path(dir_) / b, ec);
}

} // namespace quill
