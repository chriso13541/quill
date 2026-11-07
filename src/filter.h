// Hook points for the built-in ad/tracker blocker (milestone 4).
//
// Network::fetch() asks should_block() before every request, including each
// redirect hop. Browser passes cosmetic_css() to the engine as an author
// stylesheet, so EasyList-style "##selector" rules become display:none.
// The default implementation blocks nothing.
#pragma once
#include <memory>
#include <string>

namespace quill {

enum class ResourceType { Document, Stylesheet, Image, Font, Other };

class RequestFilter {
public:
    virtual ~RequestFilter() = default;
    virtual bool        should_block(const std::string& url, const std::string& initiator_url, ResourceType type) { return false; }
    virtual std::string cosmetic_css(const std::string& host) { return {}; }
    // Lets the blocker strip tracking parameters (utm_*, fbclid, ...) before a top-level navigation.
    virtual std::string clean_url(const std::string& url) { return url; }
};

} // namespace quill
