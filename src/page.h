// Per-page shared state and the subresource (image) loader used by the engine's layout and paint.
#pragma once
#include "image.h"
#include "net.h"
#include "threadpool.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace quill {

struct ImageEntry {
    enum State { Loading, Ready, Failed } state = Loading;
    std::shared_ptr<Image> img;
};

// Shared between the loader (main + worker threads) and the Browser.
struct PageState {
    std::atomic<bool> cancel{false};
    std::atomic<bool> dirty{false};          // an image layout was waiting for arrived (or failed): layout must be redone
    std::atomic<bool> repaint{false};        // an image arrived that layout did not depend on: only repaint
    std::atomic<int>  pending_images{0};
    std::function<void()> wake;              // nudges the UI thread; may be called from any thread

    std::mutex                                  mu;
    std::string                                 url;      // document URL (initiator for subresources)
    std::string                                 referrer_policy; // the document's (header or <meta name=referrer>)
    std::unordered_map<std::string, ImageEntry> images;
    std::unordered_set<std::string>             layout_waiting; // images layout asked about before they were there
};

// Pages visited this session (for a:visited). Shared by all tabs and frames; never written to disk, like the cookie jar.
// Thread-safe. URLs are compared without their #fragment.
class VisitedLinks {
public:
    static VisitedLinks& instance();
    void     add(const std::string& url);
    bool     contains(const std::string& url) const;
    uint64_t generation() const { return gen_.load(); } // changes whenever a new URL is added

private:
    mutable std::mutex              mu_;
    std::unordered_set<std::string> urls_;
    std::atomic<uint64_t>           gen_{0};
};

// libwapcaplet (the string interner under libdom/libcss) is not thread-safe: everything that parses,
// styles or frees an engine Document holds this lock.
std::mutex& engine_mutex();

class ImageLoader {
public:
    ImageLoader(std::shared_ptr<PageState> st, Network* net, ThreadPool* pool, size_t max_image_bytes);

    void        set_base(const std::string& base);
    std::string resolve(const std::string& src) const;
    // Decoded image if it is ready; otherwise starts the download (once) and returns null.
    const Image* get(const std::string& src);
    // Layout hook: size of the image if known (starts the download otherwise).
    bool size(const std::string& src, int& w, int& h); // (called by layout: remembers what layout is waiting for)
    // True once the image is known to be unavailable (download or decode failed, blocked, empty src).
    bool broken(const std::string& src) const;
    // Registers an already decoded image (image documents).
    void prime(const std::string& url, std::shared_ptr<Image> img);

private:
    void start(const std::string& key);

    std::shared_ptr<PageState> st_;
    Network*                   net_;
    ThreadPool*                pool_;
    size_t                     max_image_bytes_;
    mutable std::mutex         mu_;
    std::string                base_;
    mutable std::unordered_map<std::string, std::string> resolved_;
};

} // namespace quill
