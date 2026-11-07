// Turns whatever was typed in the URL bar into a URL.
#pragma once
#include "policy.h"
#include "settings.h"

#include <string>

namespace quill {

struct InputResult {
    std::string url;       // empty => nothing to do
    bool        is_search = false;
    bool        scheme_guessed = false; // bare private host: http:// was assumed, https:// is the fallback
};

// - "https://..." / "http://..."  -> used as is
// - "about:..." / "quill:..."     -> internal pages
// - "example.com/path"            -> https://example.com/path
// - "192.168.1.1", "router.lan"   -> http://... (private hosts)
// - "nas/" or "localhost:8080"    -> http://... (private hosts)
// - anything else                 -> search_url with the text substituted for %s
InputResult resolve_input(const std::string& raw, const Settings& s, const Policy& p);

} // namespace quill
