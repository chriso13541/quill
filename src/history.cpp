#include "history.h"

#include "page.h"
#include "url.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace quill {

namespace {
constexpr size_t kMaxEntries = 50000;

std::string clean(const std::string& s, size_t max) { // one line, no tabs: the file's separators
    std::string o;
    for (char c : s) {
        if (o.size() >= max) break;
        o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    return o;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
void make_private_dir(const fs::path& dir) {
    std::error_code ec;
    bool            made = fs::create_directories(dir, ec);
#ifndef _WIN32
    if (made) fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#else
    (void)made;
#endif
}
void make_private_file(const std::string& p) {
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#else
    (void)p;
#endif
}
std::string line_of(const HistoryEntry& e) { return std::to_string(static_cast<long long>(e.when)) + "\t" + e.url + "\t" + e.title + "\n"; }
} // namespace

std::string default_history_path() {
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA")) return std::string(a) + "\\quill\\history.tsv";
    return "quill-history.tsv";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/quill/history.tsv";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.local/share/quill/history.tsv";
    return "";
#endif
}

History& History::instance() {
    static History* h = new History(); // leaked on purpose: tabs may record a visit while Quill exits
    return *h;
}

void History::configure(bool enabled, const std::string& path, int days) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = enabled;
    path_    = enabled ? path : std::string();
    days_    = std::max(1, days);
    entries_.clear();
    if (path_.empty()) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    std::time_t cutoff = std::time(nullptr) - static_cast<std::time_t>(days_) * 86400;
    std::string line;
    bool        dropped = false;
    while (std::getline(in, line)) {
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) { dropped = true; continue; }
        HistoryEntry e;
        e.when  = static_cast<std::time_t>(std::atoll(line.substr(0, a).c_str()));
        e.url   = line.substr(a + 1, b - a - 1);
        e.title = line.substr(b + 1);
        if (!e.title.empty() && e.title.back() == '\r') e.title.pop_back();
        std::string l = lower(e.url.substr(0, 8));
        if (e.when < cutoff || !(l.compare(0, 7, "http://") == 0 || l.compare(0, 8, "https://") == 0)) { dropped = true; continue; }
        entries_.push_back(std::move(e));
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const HistoryEntry& x, const HistoryEntry& y) { return x.when < y.when; });
    if (entries_.size() > kMaxEntries) {
        entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(kMaxEntries));
        dropped = true;
    }
    for (auto& e : entries_) VisitedLinks::instance().add(e.url);
    if (dropped) rewrite_locked();
}

bool History::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

void History::add(const std::string& url_in, const std::string& title_in, std::time_t when) {
    std::string l = lower(url_in.substr(0, 8));
    if (!(l.compare(0, 7, "http://") == 0 || l.compare(0, 8, "https://") == 0)) return; // web pages only
    HistoryEntry e;
    e.url   = clean(url_in, 4096);
    e.title = clean(title_in == url_in ? std::string() : title_in, 300);
    e.when  = when ? when : std::time(nullptr);
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_) return;
    // a reload (or the same page again within a minute) updates the last visit instead of adding one
    if (!entries_.empty() && entries_.back().url == e.url && e.when - entries_.back().when < 60) {
        if (!e.title.empty()) entries_.back().title = e.title;
        entries_.back().when = e.when;
        return;
    }
    entries_.push_back(e);
    if (entries_.size() > kMaxEntries + 1000) {
        entries_.erase(entries_.begin(), entries_.begin() + 1000);
        rewrite_locked();
        return;
    }
    if (path_.empty()) return;
    bool fresh = !fs::exists(path_);
    if (fresh) make_private_dir(fs::path(path_).parent_path());
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) return;
    out << line_of(e);
    out.close();
    if (fresh) make_private_file(path_);
}

std::vector<HistoryEntry> History::list(const std::string& query, size_t limit) const {
    std::string q = lower(query);
    std::vector<HistoryEntry> out;
    std::lock_guard<std::mutex> lk(mu_);
    std::unordered_set<std::string> seen; // "day url" already listed
    for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < limit; ++it) {
        if (!q.empty() && lower(it->url).find(q) == std::string::npos && lower(it->title).find(q) == std::string::npos) continue;
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &it->when);
#else
        localtime_r(&it->when, &tmv);
#endif
        long long day = (tmv.tm_year + 1900LL) * 400 + tmv.tm_yday;
        if (!seen.insert(std::to_string(day) + " " + it->url).second) continue;
        out.push_back(*it);
    }
    return out;
}

size_t History::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

void History::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    rewrite_locked();
}

void History::rewrite_locked() {
    if (path_.empty()) return;
    make_private_dir(fs::path(path_).parent_path());
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        for (auto& e : entries_) out << line_of(e);
        if (!out) return;
    }
    make_private_file(tmp);
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (ec) fs::remove(tmp, ec);
}

} // namespace quill
