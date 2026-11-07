#include "page.h"

#include "url.h"

namespace quill {

std::mutex& engine_mutex() {
    static std::mutex m;
    return m;
}

VisitedLinks& VisitedLinks::instance() {
    static VisitedLinks v;
    return v;
}

void VisitedLinks::add(const std::string& url) {
    if (url.empty()) return;
    std::string key = to_lower(url.substr(0, 5)) == "data:" ? url : strip_fragment(url);
    std::lock_guard<std::mutex> lk(mu_);
    if (urls_.size() < 100000 && urls_.insert(key).second) gen_++;
}

bool VisitedLinks::contains(const std::string& url) const {
    if (url.empty()) return false;
    std::string key = strip_fragment(url);
    std::lock_guard<std::mutex> lk(mu_);
    return urls_.count(key) != 0;
}

bool ImageLoader::broken(const std::string& src) const {
    std::string key = resolve(src);
    if (key.empty()) return true;
    std::lock_guard<std::mutex> lk(st_->mu);
    auto it = st_->images.find(key);
    return it != st_->images.end() && it->second.state == ImageEntry::Failed;
}

ImageLoader::ImageLoader(std::shared_ptr<PageState> st, Network* net, ThreadPool* pool, size_t max_image_bytes) :
    st_(std::move(st)), net_(net), pool_(pool), max_image_bytes_(max_image_bytes) {}

void ImageLoader::set_base(const std::string& base) {
    std::lock_guard<std::mutex> lk(mu_);
    base_ = base;
    resolved_.clear();
}

std::string ImageLoader::resolve(const std::string& src) const {
    if (src.empty()) return {};
    std::lock_guard<std::mutex> lk(mu_);
    auto                        it = resolved_.find(src);
    if (it != resolved_.end()) return it->second;
    std::string b = base_;
    if (b.empty()) {
        std::lock_guard<std::mutex> lk2(st_->mu);
        b = st_->url;
    }
    std::string r = resolve_url(b, src);
    if (to_lower(r.substr(0, 5)) != "data:") r = strip_fragment(r);
    if (resolved_.size() < 4096) resolved_[src] = r;
    return r;
}

void ImageLoader::prime(const std::string& url, std::shared_ptr<Image> img) {
    std::lock_guard<std::mutex> lk(st_->mu);
    ImageEntry&                 e = st_->images[url];
    e.state                       = img ? ImageEntry::Ready : ImageEntry::Failed;
    e.img                         = std::move(img);
}

const Image* ImageLoader::get(const std::string& src) {
    std::string key = resolve(src);
    if (key.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        auto                        it = st_->images.find(key);
        if (it != st_->images.end()) return it->second.state == ImageEntry::Ready ? it->second.img.get() : nullptr;
    }
    start(key);
    std::lock_guard<std::mutex> lk(st_->mu); // data: URIs finish synchronously
    auto                        it = st_->images.find(key);
    return it != st_->images.end() && it->second.state == ImageEntry::Ready ? it->second.img.get() : nullptr;
}

bool ImageLoader::size(const std::string& src, int& w, int& h) {
    const Image* im = get(src);
    if (!im) {
        std::string key = resolve(src);
        std::lock_guard<std::mutex> lk(st_->mu);
        if (!key.empty() && st_->layout_waiting.size() < 100000) st_->layout_waiting.insert(key);
        return false;
    }
    w = im->w;
    h = im->h;
    return true;
}

void ImageLoader::start(const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        if (st_->images.count(key)) return;
        st_->images[key] = ImageEntry();
    }
    auto st  = st_;
    auto net = net_;
    auto max = max_image_bytes_;

    if (to_lower(key.substr(0, 5)) == "data:") {
        std::string            mime, bytes;
        std::shared_ptr<Image> img;
        if (decode_data_uri(key, mime, bytes)) img = decode_image(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        std::lock_guard<std::mutex> lk(st->mu);
        ImageEntry&                 e = st->images[key];
        e.state                       = img ? ImageEntry::Ready : ImageEntry::Failed;
        e.img                         = img;
        return;
    }

    st->pending_images++;
    pool_->submit([st, net, max, key]() {
        std::shared_ptr<Image> img;
        if (!st->cancel.load()) {
            FetchRequest r;
            r.url       = key;
            {
                // Mixed content on an https page: try the image over https instead of loading it insecurely (the
                // policy blocks plain http here). Most sites serve the same file on both.
                std::lock_guard<std::mutex> lk(st->mu);
                if (to_lower(key.substr(0, 7)) == "http://" && to_lower(st->url.substr(0, 8)) == "https://") r.url = "https://" + key.substr(7);
            }
            r.kind      = RequestKind::Subresource;
            r.type      = ResourceType::Image;
            r.max_bytes = max;
            r.cancel    = &st->cancel;
            {
                std::lock_guard<std::mutex> lk(st->mu);
                r.initiator       = st->url;
                r.referrer_policy = st->referrer_policy;
            }
            FetchResult fr = net->fetch(r);
            if (fr.ok && fr.status >= 200 && fr.status < 300)
                img = decode_image(reinterpret_cast<const unsigned char*>(fr.body.data()), fr.body.size());
        }
        {
            std::lock_guard<std::mutex> lk(st->mu);
            ImageEntry&                 e = st->images[key];
            e.state                       = img ? ImageEntry::Ready : ImageEntry::Failed;
            e.img                         = img;
        }
        {
            // Only images layout had to guess the size of change the layout (or failed ones it may lay out as alt
            // text); a thumbnail with width/height, or a CSS background, just needs painting.
            std::lock_guard<std::mutex> lk(st->mu);
            if (st->layout_waiting.erase(key)) st->dirty = true;
            else if (img) st->repaint = true;
        }
        st->pending_images--;
        if (st->wake) st->wake();
    });
}

} // namespace quill
