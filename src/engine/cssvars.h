// CSS custom properties (var()) pre-pass.
//
// libcss has no var() support, so the engine resolves custom properties in text before libcss ever
// sees a stylesheet: definitions are collected from every sheet, the cascade between them is decided
// here, and var(--x[, fallback]) is replaced by its value.
//
// Scope: a definition applies page-wide when its selector is a single compound selector that matches
// the root element (:root, html, html.cls, ...) or <body>, or is `*`. Definitions scoped to other
// elements (.card { --c: red }) are counted in `skipped_scoped` but not applied yet; var() uses whose
// variable is undefined use their fallback, or the whole declaration is dropped.
//
// This module has no libdom/libcss dependency.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace quill::engine {

struct ElementView {
    std::string tag;                                                      // lower-case local name
    std::function<bool(const std::string&, std::string&)> attr;           // attribute lookup
};

struct MediaEnv {
    int width  = 1024;
    int height = 768;
};

class CssVars {
public:
    CssVars(MediaEnv env, ElementView root, ElementView body);

    // Scans one stylesheet's text (document order matters: call once per sheet, imports first).
    void collect(const std::string& css);

    // Call after every collect(): decides which variables are "tainted" (scoped to non-root selectors, directly or
    // through other variables). Declarations using tainted variables are skipped by apply(); ElementCascade
    // resolves them per element.
    void finish();
    bool references_tainted(const std::string& value) const;
    size_t tainted_count() const { return tainted_.size(); }

    // Returns `css` with custom property declarations removed and var() substituted.
    std::string apply(const std::string& css);
    // Same for a bare declaration list (a style="" attribute).
    std::string apply_declarations(const std::string& decls);

    // @import targets found in a stylesheet (raw, unresolved).
    static std::vector<std::string> imports(const std::string& css);

    // Media query evaluation against the environment (exposed for tests).
    bool media_matches(const std::string& query) const;

    size_t defined() const { return defs_.size(); }
    size_t substituted = 0;    // var() uses replaced
    size_t unresolved = 0;     // declarations dropped because a variable was undefined
    size_t skipped_scoped = 0; // custom property rules not scoped to root/body (resolved per element by ElementCascade)
    size_t deferred = 0;       // declarations left to ElementCascade because they use a tainted variable

private:
    struct Def {
        std::string value;
        bool        important;
        int         rank;
        int         spec;
        size_t      order;
    };
    struct Memo {
        bool        ok;
        std::string val;
    };

    void        collect_rules(const std::string& s, size_t b, size_t e);
    void        add_def(const std::string& name, const std::string& value, bool important, int rank, int spec);
    int         selector_rank(const std::string& selector, int& spec) const;
    bool        subst(const std::string& in, std::string& out, int depth);
    bool        lookup(const std::string& name, std::string& out, int depth);
    std::string transform_rules(const std::string& s, size_t b, size_t e);
    std::string transform_decls(const std::string& s, size_t b, size_t e);
    bool        eval_feature(const std::string& inner) const;
    bool        media_one(const std::string& q) const;

    MediaEnv                    env_;
    ElementView                 root_, body_;
    std::map<std::string, Def>  defs_;
    std::map<std::string, Memo> memo_;
    std::set<std::string>       resolving_;
    size_t                      order_ = 0;
    std::set<std::string>       tainted_;
    std::vector<std::pair<std::string, std::string>> scoped_defs_; // (name, value) of scoped definitions
};

} // namespace quill::engine
