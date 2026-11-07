#include "svg.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

namespace quill {

struct SvgDoc::Impl {
    NSVGimage* img = nullptr;
    ~Impl() {
        if (img) nsvgDelete(img);
    }
};

SvgDoc::~SvgDoc() = default;

float SvgDoc::width() const { return impl_ && impl_->img ? impl_->img->width : 0.0f; }
float SvgDoc::height() const { return impl_ && impl_->img ? impl_->img->height : 0.0f; }

bool SvgDoc::sniff(const unsigned char* d, size_t n) {
    size_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) i = 3; // UTF-8 BOM
    while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\r' || d[i] == '\n')) i++;
    if (i >= n || d[i] != '<') return false;
    size_t      end = std::min(n, i + 2048);
    std::string head(reinterpret_cast<const char*>(d) + i, end - i);
    for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head.find("<html") != std::string::npos) return false;
    return head.find("<svg") != std::string::npos;
}

std::shared_ptr<SvgDoc> SvgDoc::parse(const unsigned char* data, size_t len) {
    if (!data || len == 0 || len > 4u * 1024 * 1024) return nullptr;
    if (!sniff(data, len)) return nullptr;
    std::string buf(reinterpret_cast<const char*>(data), len); // nsvgParse edits its input and needs a NUL
    NSVGimage*  img = nsvgParse(&buf[0], "px", 96.0f);
    if (!img) return nullptr;
    if (!(img->width > 0.0f) || !(img->height > 0.0f) || !std::isfinite(img->width) || !std::isfinite(img->height) ||
        img->width > 1e6f || img->height > 1e6f) {
        nsvgDelete(img);
        return nullptr;
    }
    std::shared_ptr<SvgDoc> d(new SvgDoc());
    d->impl_.reset(new Impl());
    d->impl_->img = img;
    return d;
}

bool SvgDoc::render(int tw, int th, std::vector<uint32_t>& out) const {
    if (!impl_ || !impl_->img || tw <= 0 || th <= 0) return false;
    if (static_cast<int64_t>(tw) * th > 16'000'000) return false;
    NSVGrasterizer* r = nsvgCreateRasterizer();
    if (!r) return false;
    float s  = std::min(static_cast<float>(tw) / impl_->img->width, static_cast<float>(th) / impl_->img->height);
    float tx = (static_cast<float>(tw) - impl_->img->width * s) * 0.5f;
    float ty = (static_cast<float>(th) - impl_->img->height * s) * 0.5f;
    std::vector<unsigned char> rgba(static_cast<size_t>(tw) * th * 4, 0);
    nsvgRasterize(r, impl_->img, tx, ty, s, rgba.data(), tw, th, tw * 4);
    nsvgDeleteRasterizer(r);
    out.resize(static_cast<size_t>(tw) * th);
    for (size_t i = 0; i < out.size(); i++) {
        const unsigned char* p = &rgba[i * 4];
        out[i] = (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    }
    return true;
}

} // namespace quill
