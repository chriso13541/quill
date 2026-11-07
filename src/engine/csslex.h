// Small CSS text utilities shared by the var() pre-pass and the per-element cascade: comment stripping,
// splitting a stylesheet into rules and a rule body into declarations, calc() folding.
#pragma once

#include <string>
#include <vector>

namespace quill::engine::lex {

bool        is_ws(char c);
bool        is_ident(char c);
std::string trim(const std::string& s);
std::string lower(std::string s);
bool        starts_with(const std::string& s, const char* p);
std::string strip_comments(const std::string& s);
// First char from `stops` at nesting depth 0 (outside strings), or `e`. ( and [ always nest; { only if `braces`.
size_t scan_to(const std::string& s, size_t i, size_t e, const char* stops, bool braces);

struct Rule {
    bool        at = false, has_block = false;
    std::string name, prelude;
    size_t      rb = 0, re = 0, bb = 0, be = 0; // whole rule; body (inside the braces)
};
std::vector<Rule> parse_rules(const std::string& s, size_t b, size_t e);

struct Decl {
    std::string name, value, raw;
    bool        important = false, valid = false;
};
std::vector<Decl> parse_decls(const std::string& s, size_t b, size_t e);

std::vector<std::string> split_commas(const std::string& s);
bool                     parse_len(const std::string& t, double& px);
std::string              fold_calc(const std::string& in);

} // namespace quill::engine::lex
