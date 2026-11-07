// Small libdom helpers shared by the engine's .cpp files. Every node returned is borrowed: the tree
// owns it, so the reference libdom hands out is dropped right away (safe while the tree is not being
// modified underneath us). Include only from .cpp files; this pulls in the C headers.
#pragma once

extern "C" {
#include <bindings/hubbub/parser.h>
#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libwapcaplet/libwapcaplet.h>
}

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace quill::engine::domutil {

// ------------------------------------------------------------------ small helpers

inline std::string lower(std::string s) {
    for (auto& c : s) // ASCII only (HTML names and keywords), without a locale call per byte
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}

inline std::string dstr(dom_string* s) {
    if (!s) return {};
    return std::string(dom_string_data(s), dom_string_byte_length(s));
}

inline std::string lwcs(lwc_string* s) { return s ? std::string(lwc_string_data(s), lwc_string_length(s)) : std::string(); }

inline lwc_string* intern(const std::string& s) {
    lwc_string* out = nullptr;
    if (lwc_intern_string(s.data(), s.size(), &out) != lwc_error_ok) return nullptr;
    return out;
}

// --- DOM navigation. Every returned node is borrowed: the tree owns it, so we drop the reference
// libdom hands out right away. Safe as long as the tree is not being modified underneath us.
inline dom_node* parent_of(dom_node* n) {
    dom_node* p = nullptr;
    if (dom_node_get_parent_node(n, &p) != DOM_NO_ERR || !p) return nullptr;
    dom_node_unref(p);
    return p;
}
inline dom_node* first_child(dom_node* n) {
    dom_node* c = nullptr;
    if (dom_node_get_first_child(n, &c) != DOM_NO_ERR || !c) return nullptr;
    dom_node_unref(c);
    return c;
}
inline dom_node* next_sibling(dom_node* n) {
    dom_node* c = nullptr;
    if (dom_node_get_next_sibling(n, &c) != DOM_NO_ERR || !c) return nullptr;
    dom_node_unref(c);
    return c;
}
inline dom_node* prev_sibling(dom_node* n) {
    dom_node* c = nullptr;
    if (dom_node_get_previous_sibling(n, &c) != DOM_NO_ERR || !c) return nullptr;
    dom_node_unref(c);
    return c;
}
inline dom_node_type type_of(dom_node* n) {
    dom_node_type t = static_cast<dom_node_type>(0);
    dom_node_get_node_type(n, &t);
    return t;
}
inline bool is_element(dom_node* n) { return n && type_of(n) == DOM_ELEMENT_NODE; }

inline dom_node* parent_element(dom_node* n) {
    dom_node* p = parent_of(n);
    return is_element(p) ? p : nullptr;
}
inline dom_node* prev_element(dom_node* n) {
    dom_node* p = prev_sibling(n);
    while (p && !is_element(p)) p = prev_sibling(p);
    return p;
}
inline dom_node* next_element(dom_node* n) {
    dom_node* p = next_sibling(n);
    while (p && !is_element(p)) p = next_sibling(p);
    return p;
}

inline std::string tag_of(dom_node* n) {
    dom_string* s = nullptr;
    if (dom_node_get_node_name(n, &s) != DOM_NO_ERR || !s) return {};
    std::string r = lower(dstr(s));
    dom_string_unref(s);
    return r;
}

inline bool get_attr(dom_node* n, const char* name, std::string& out) {
    dom_string* k = nullptr;
    if (dom_string_create(reinterpret_cast<const uint8_t*>(name), std::strlen(name), &k) != DOM_NO_ERR) return false;
    dom_string* v = nullptr;
    dom_exception e = dom_element_get_attribute(n, k, &v);
    dom_string_unref(k);
    if (e != DOM_NO_ERR || !v) return false;
    out = dstr(v);
    dom_string_unref(v);
    return true;
}

inline std::vector<std::pair<std::string, std::string>> all_attrs(dom_node* n) {
    std::vector<std::pair<std::string, std::string>> out;
    dom_namednodemap* map = nullptr;
    if (dom_node_get_attributes(n, &map) != DOM_NO_ERR || !map) return out;
    uint32_t len = 0;
    dom_namednodemap_get_length(map, &len);
    for (uint32_t i = 0; i < len; ++i) {
        dom_node* a = nullptr;
        if (dom_namednodemap_item(map, i, &a) != DOM_NO_ERR || !a) continue;
        dom_string *nm = nullptr, *val = nullptr;
        dom_attr_get_name(reinterpret_cast<dom_attr*>(a), &nm);
        dom_attr_get_value(reinterpret_cast<dom_attr*>(a), &val);
        if (nm) out.push_back({lower(dstr(nm)), val ? dstr(val) : std::string()});
        if (nm) dom_string_unref(nm);
        if (val) dom_string_unref(val);
        dom_node_unref(a);
    }
    dom_namednodemap_unref(map);
    return out;
}

inline void set_attr(dom_node* n, const char* name, const std::string& value) {
    dom_string *k = nullptr, *v = nullptr;
    if (dom_string_create(reinterpret_cast<const uint8_t*>(name), std::strlen(name), &k) != DOM_NO_ERR) return;
    if (dom_string_create(reinterpret_cast<const uint8_t*>(value.data()), value.size(), &v) == DOM_NO_ERR) {
        dom_element_set_attribute(n, k, v);
        dom_string_unref(v);
    }
    dom_string_unref(k);
}

inline void remove_attr(dom_node* n, const char* name) {
    dom_string* k = nullptr;
    if (dom_string_create(reinterpret_cast<const uint8_t*>(name), std::strlen(name), &k) != DOM_NO_ERR) return;
    dom_element_remove_attribute(n, k);
    dom_string_unref(k);
}

inline std::string text_content(dom_node* n) {
    dom_string* s = nullptr;
    if (dom_node_get_text_content(n, &s) != DOM_NO_ERR || !s) return {};
    std::string r = dstr(s);
    dom_string_unref(s);
    return r;
}

inline bool has_token(const std::string& list, const char* tok) {
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace(static_cast<unsigned char>(list[i]))) i++;
        size_t j = i;
        while (j < list.size() && !std::isspace(static_cast<unsigned char>(list[j]))) j++;
        if (lower(list.substr(i, j - i)) == tok) return true;
        i = j;
    }
    return false;
}


} // namespace quill::engine::domutil
