#include "httpcache.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace quill {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\"'"), b = s.find_last_not_of(" \t\"'");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// Cache-Control directive `name`: present? (its value in `val`)
bool directive(const std::string& cc, const char* name, std::string* val = nullptr) {
    std::string l = lower(cc);
    size_t      i = 0;
    while (i < l.size()) {
        size_t      j = l.find(',', i);
        if (j == std::string::npos) j = l.size();
        std::string d = l.substr(i, j - i);
        i             = j + 1;
        size_t      eq = d.find('=');
        std::string n  = trim(eq == std::string::npos ? d : d.substr(0, eq));
        if (n == name) {
            if (val) *val = eq == std::string::npos ? std::string() : trim(d.substr(eq + 1));
            return true;
        }
    }
    return false;
}
// An HTTP date (RFC 9110: IMF-fixdate; the obsolete RFC 850 and asctime forms too). -1 if unreadable.
std::time_t http_date(const std::string& s_in) {
    static const char* mon[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string s = lower(s_in);
    int         d = -1, m = -1, y = -1, hh = -1, mm = -1, ss = -1;
    // the time is the "hh:mm:ss" group; of the other numbers the first is the day and the second the year, in all
    // three formats ("06 Nov 1994 08:49:37", "06-Nov-94 08:49:37", asctime's "Nov  6 08:49:37 1994")
    std::vector<int> nums;
    for (size_t i = 0; i < s.size();) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
            int v = std::atoi(s.substr(i, j - i).c_str());
            if (j + 1 < s.size() && s[j] == ':' && hh < 0) { // hh:mm:ss
                hh = v;
                size_t k = j + 1, e = k;
                while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) e++;
                mm = std::atoi(s.substr(k, e - k).c_str());
                ss = 0;
                if (e + 1 < s.size() && s[e] == ':') {
                    size_t f = e + 1;
                    e = f;
                    while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) e++;
                    ss = std::atoi(s.substr(f, e - f).c_str());
                }
                i = e;
                continue;
            }
            nums.push_back(v);
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) j++;
            std::string w = s.substr(i, std::min<size_t>(3, j - i));
            for (int k = 0; k < 12; k++)
                if (w == mon[k] && j - i <= 9) m = k;
            i = j;
        } else i++;
    }
    if (m < 0 || hh < 0 || nums.size() < 2) return -1;
    d = nums[0];
    y = nums[1];
    if (y < 100) y += y < 70 ? 2000 : 1900;
    if (d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60 || y < 1970) return -1;
    // days from civil (Howard Hinnant's algorithm), no time zone involved
    int       yy = y - (m < 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;
    long long doy = (153LL * (m + (m > 1 ? -2 : 10)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return static_cast<std::time_t>(days * 86400 + hh * 3600 + mm * 60 + ss);
}
} // namespace

HttpCache& HttpCache::instance() {
    static HttpCache* c = new HttpCache(); // leaked on purpose: workers may use it while Quill exits
    return *c;
}

std::string HttpCache::key(const std::string& site, const std::string& url) {
    std::string u = url;
    size_t      h = u.find('#');
    if (h != std::string::npos) u.resize(h);
    return site + " " + u;
}

bool HttpCache::storable(int status, const CacheHeaders& h) {
    if (status != 200) return false;
    if (directive(h.cache_control, "no-store")) return false;
    std::string v = lower(h.vary);
    if (!v.empty()) { // Quill's requests always carry the same Accept / Accept-Language / User-Agent
        size_t i = 0;
        while (i < v.size()) {
            size_t      j = v.find(',', i);
            if (j == std::string::npos) j = v.size();
            std::string t = trim(v.substr(i, j - i));
            i             = j + 1;
            if (t.empty() || t == "accept-encoding" || t == "accept" || t == "accept-language" || t == "user-agent" || t == "origin") continue;
            return false; // Vary: * / cookie / anything else
        }
    }
    return true;
}

std::time_t HttpCache::freshness(const CacheHeaders& h, std::time_t now) {
    std::string v;
    if (directive(h.cache_control, "no-cache") || (h.cache_control.empty() && lower(h.pragma).find("no-cache") != std::string::npos)) return 0;
    long long age = std::max(0LL, std::atoll(trim(h.age).c_str()));
    if (directive(h.cache_control, "max-age", &v)) {
        long long ma = std::atoll(v.c_str());
        return ma > age ? now + static_cast<std::time_t>(std::min(ma - age, 365LL * 86400)) : 0;
    }
    std::time_t date = h.date.empty() ? -1 : http_date(h.date);
    if (date < 0) date = now;
    if (!h.expires.empty()) {
        std::time_t ex = http_date(h.expires); // an unreadable Expires ("0", "-1") means already expired
        return ex > date ? now + std::min<std::time_t>(ex - date, 365 * 86400) - static_cast<std::time_t>(age) : 0;
    }
    if (!h.last_modified.empty()) { // heuristic: a tenth of how long it has been unchanged, at most a day
        std::time_t lm = http_date(h.last_modified);
        if (lm > 0 && lm < date) return now + std::min<std::time_t>((date - lm) / 10, 86400);
    }
    return 0;
}

HttpCache::Look HttpCache::lookup(const std::string& key, std::time_t now, CachedResponse& entry) {
    std::lock_guard<std::mutex> lk(mu_);
    auto                        it = map_.find(key);
    if (it == map_.end()) return Look::Miss;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    entry = it->second.r;
    if (now < it->second.r.fresh_until) return Look::Fresh;
    if (it->second.r.etag.empty() && it->second.r.last_modified.empty()) return Look::Miss; // nothing to revalidate with
    return Look::Stale;
}

void HttpCache::store(const std::string& key, CachedResponse e) {
    if (e.body.size() > cap_ / 8 || (e.fresh_until == 0 && e.etag.empty() && e.last_modified.empty())) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto                        it = map_.find(key);
    if (it != map_.end()) {
        bytes_ -= it->second.r.body.size();
        lru_.erase(it->second.lru);
        map_.erase(it);
    }
    lru_.push_front(key);
    bytes_ += e.body.size();
    map_[key] = Slot{std::move(e), lru_.begin()};
    evict_locked();
}

bool HttpCache::refresh(const std::string& key, const CacheHeaders& h, std::time_t now, CachedResponse& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto                        it = map_.find(key);
    if (it == map_.end()) return false;
    CachedResponse& r = it->second.r;
    r.stored          = now;
    r.fresh_until     = freshness(h, now);
    if (!h.etag.empty()) r.etag = h.etag;
    if (!h.last_modified.empty()) r.last_modified = h.last_modified;
    out = r;
    return true;
}

void HttpCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
    lru_.clear();
    bytes_ = 0;
}

size_t HttpCache::bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
}

void HttpCache::set_capacity(size_t b) {
    std::lock_guard<std::mutex> lk(mu_);
    cap_ = b;
    evict_locked();
}

void HttpCache::evict_locked() {
    while (bytes_ > cap_ && !lru_.empty()) {
        auto it = map_.find(lru_.back());
        if (it != map_.end()) {
            bytes_ -= it->second.r.body.size();
            map_.erase(it);
        }
        lru_.pop_back();
    }
}

} // namespace quill
