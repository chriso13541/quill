#include "input.h"

#include <cctype>

namespace quill {

static bool scheme_prefix(const std::string& in, std::string& scheme) {
    if (in.empty() || !std::isalpha(static_cast<unsigned char>(in[0]))) return false;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isalnum(c) || c == '+' || c == '-' || c == '.') i++;
        else break;
    }
    if (i >= in.size() || in[i] != ':') return false;
    scheme = to_lower(in.substr(0, i));
    return true;
}

static bool all_digits_or_dots(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    return true;
}

InputResult resolve_input(const std::string& raw, const Settings& s, const Policy& p) {
    InputResult r;
    std::string in = trim(raw);
    if (in.empty()) return r;

    std::string scheme;
    if (scheme_prefix(in, scheme)) {
        bool has_slashes = in.compare(scheme.size(), 3, "://") == 0;
        bool known       = scheme == "about" || scheme == "quill" || scheme == "javascript" || scheme == "data" ||
                     scheme == "file" || scheme == "mailto" || scheme == "ftp" || scheme == "view-source";
        if (has_slashes || known) {
            r.url = in;
            return r;
        }
        // otherwise "localhost:8080" style: fall through to bare-host handling
    }

    bool has_space = in.find_first_of(" \t\r\n") != std::string::npos;
    if (!has_space) {
        size_t      cut      = in.find_first_of("/?#");
        std::string hostport = in.substr(0, cut == std::string::npos ? std::string::npos : cut);
        std::string rest     = cut == std::string::npos ? "" : in.substr(cut);

        std::string host = hostport, port;
        bool        bracket = !hostport.empty() && hostport[0] == '[';
        if (bracket) {
            size_t rb = hostport.find(']');
            if (rb != std::string::npos) {
                host = hostport.substr(1, rb - 1);
                if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') port = hostport.substr(rb + 2);
            }
        } else {
            size_t c = hostport.rfind(':');
            if (c != std::string::npos) {
                host = hostport.substr(0, c);
                port = hostport.substr(c + 1);
            }
        }

        bool chars_ok = !host.empty();
        for (char c : host) {
            unsigned char u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) || c == '.' || c == '-' || c == '_' || (bracket && (c == ':' || c == '%')))) chars_ok = false;
        }
        for (char c : port)
            if (!std::isdigit(static_cast<unsigned char>(c))) chars_ok = false;

        unsigned char ip[16];
        int           len = 0;
        bool          is_ip = parse_ip_literal(host, ip, len);

        bool host_like = false;
        if (chars_ok) {
            if (is_ip) host_like = true;
            else if (all_digits_or_dots(host)) host_like = false; // "3.14" is not a host
            else if (host.find('.') != std::string::npos && host.back() != '.' && host[0] != '.') host_like = true;
            else if (normalize_host(host) == "localhost") host_like = true;
            else if (!port.empty()) host_like = true;
            else if (!rest.empty() && rest[0] == '/') host_like = true; // "nas/"
        }

        if (host_like) {
            bool priv = p.is_private_host(host);
            r.url     = std::string(priv ? "http://" : "https://") + in;
            r.scheme_guessed = priv;
            return r;
        }
    }

    r.is_search = true;
    std::string enc = percent_encode(in);
    size_t      pos = s.search_url.find("%s");
    if (pos == std::string::npos) r.url = s.search_url + enc;
    else r.url = s.search_url.substr(0, pos) + enc + s.search_url.substr(pos + 2);
    return r;
}

} // namespace quill
