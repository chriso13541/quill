// Browsing history: the pages visited (address, title, when), newest first, shown on about:history (Ctrl+H).
// Kept in memory and, unless turned off, in a file (`history_file`, default ~/.local/share/quill/history.tsv)
// with one visit per line, so it survives restarts and :visited links stay purple between sessions. Entries older
// than `history_days` are dropped when Quill starts. Only top-level pages are recorded (not frames, not Quill's
// own pages). Thread-safe.
#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace quill {

struct HistoryEntry {
    std::string url, title;
    std::time_t when = 0;
};

class History {
public:
    static History& instance();

    // `path` empty = memory only. Loads the file (if enabled), prunes it and feeds VisitedLinks.
    void configure(bool enabled, const std::string& path, int days);
    bool enabled() const;

    void add(const std::string& url, const std::string& title, std::time_t when = 0);
    // Newest first. `query` (case-insensitive, in the address or title) narrows it; a page visited several times
    // shows once per day (its latest visit). At most `limit` entries.
    std::vector<HistoryEntry> list(const std::string& query = std::string(), size_t limit = 500) const;
    size_t                    size() const;
    // Forgets everything (and empties the file). Visited links stay purple until Quill restarts.
    void clear();

private:
    void                      rewrite_locked();
    mutable std::mutex        mu_;
    bool                      enabled_ = false;
    std::string               path_;
    int                       days_ = 90;
    std::vector<HistoryEntry> entries_; // oldest first
};

// $XDG_DATA_HOME/quill/history.tsv or ~/.local/share/quill/history.tsv (Linux), %APPDATA%\quill\history.tsv (Windows).
std::string default_history_path();

} // namespace quill
