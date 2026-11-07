#include "url.h"

#include <cctype>
#include <cstdint>
#include <vector>
#include <cstdlib>

namespace quill {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool parse_url(const std::string& in, ParsedUrl& u) {
    u = ParsedUrl();
    if (in.empty() || !std::isalpha(static_cast<unsigned char>(in[0]))) return false;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isalnum(c) || c == '+' || c == '-' || c == '.') i++;
        else break;
    }
    if (i >= in.size() || in[i] != ':') return false;
    u.scheme = to_lower(in.substr(0, i));

    std::string rest = in.substr(i + 1);
    size_t h = rest.find('#');
    if (h != std::string::npos) {
        u.has_fragment = true;
        u.fragment     = rest.substr(h + 1);
        rest.erase(h);
    }
    size_t q = rest.find('?');
    if (q != std::string::npos) {
        u.has_query = true;
        u.query     = rest.substr(q + 1);
        rest.erase(q);
    }

    if (rest.compare(0, 2, "//") == 0) {
        u.has_authority = true;
        size_t      end  = rest.find('/', 2);
        std::string auth = rest.substr(2, end == std::string::npos ? std::string::npos : end - 2);
        u.path           = end == std::string::npos ? "" : rest.substr(end);

        size_t at = auth.rfind('@');
        if (at != std::string::npos) {
            u.userinfo = auth.substr(0, at);
            auth.erase(0, at + 1);
        }
        std::string hostpart = auth, portpart;
        if (!auth.empty() && auth[0] == '[') {
            size_t rb = auth.find(']');
            if (rb == std::string::npos) return false;
            hostpart          = auth.substr(1, rb - 1);
            std::string after = auth.substr(rb + 1);
            if (!after.empty()) {
                if (after[0] != ':') return false;
                portpart = after.substr(1);
            }
        } else {
            size_t c = auth.rfind(':');
            if (c != std::string::npos) {
                hostpart = auth.substr(0, c);
                portpart = auth.substr(c + 1);
            }
        }
        if (!portpart.empty()) {
            for (char c : portpart)
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            if (portpart.size() > 5) return false;
            long p = std::strtol(portpart.c_str(), nullptr, 10);
            if (p > 65535) return false;
            u.port = static_cast<int>(p);
        }
        u.host = to_lower(hostpart);
    } else {
        u.path = rest;
    }
    return true;
}

std::string serialize_url(const ParsedUrl& u) {
    std::string out = u.scheme + ":";
    if (u.has_authority) {
        out += "//";
        if (!u.userinfo.empty()) out += u.userinfo + "@";
        if (u.host.find(':') != std::string::npos) out += "[" + u.host + "]";
        else out += u.host;
        if (u.port >= 0) out += ":" + std::to_string(u.port);
    }
    out += u.path;
    if (u.has_query) out += "?" + u.query;
    if (u.has_fragment) out += "#" + u.fragment;
    return out;
}

static std::string remove_dot_segments(const std::string& path) {
    std::string in = path, out;
    auto pop_last = [&out]() {
        size_t p = out.rfind('/');
        out.erase(p == std::string::npos ? 0 : p);
    };
    while (!in.empty()) {
        if (in.compare(0, 3, "../") == 0) in.erase(0, 3);
        else if (in.compare(0, 2, "./") == 0) in.erase(0, 2);
        else if (in.compare(0, 3, "/./") == 0) in.replace(0, 3, "/");
        else if (in == "/.") in = "/";
        else if (in.compare(0, 4, "/../") == 0) {
            in.replace(0, 4, "/");
            pop_last();
        } else if (in == "/..") {
            in = "/";
            pop_last();
        } else if (in == "." || in == "..") in.clear();
        else {
            size_t start = in[0] == '/' ? 1 : 0;
            size_t nxt   = in.find('/', start);
            if (nxt == std::string::npos) {
                out += in;
                in.clear();
            } else {
                out += in.substr(0, nxt);
                in.erase(0, nxt);
            }
        }
    }
    return out;
}

std::string strip_fragment(const std::string& url) {
    size_t h = url.find('#');
    return h == std::string::npos ? url : url.substr(0, h);
}

std::string resolve_url(const std::string& base, const std::string& ref_in) {
    std::string ref = trim(ref_in);
    if (ref.empty()) return strip_fragment(base);

    ParsedUrl r;
    if (parse_url(ref, r)) {
        if (r.has_authority) {
            r.path = remove_dot_segments(r.path.empty() ? "/" : r.path);
            return serialize_url(r);
        }
        return ref; // opaque (data:, about:, mailto:, ...)
    }

    ParsedUrl b;
    if (!parse_url(base, b) || !b.has_authority) return ref;

    ParsedUrl t;
    t.scheme = b.scheme;

    if (ref.compare(0, 2, "//") == 0) {
        ParsedUrl n;
        if (!parse_url(b.scheme + ":" + ref, n)) return ref;
        n.path = remove_dot_segments(n.path.empty() ? "/" : n.path);
        return serialize_url(n);
    }

    std::string rpath = ref, rquery, rfrag;
    bool        hq = false, hf = false;
    size_t      f = rpath.find('#');
    if (f != std::string::npos) {
        hf    = true;
        rfrag = rpath.substr(f + 1);
        rpath.erase(f);
    }
    size_t q = rpath.find('?');
    if (q != std::string::npos) {
        hq     = true;
        rquery = rpath.substr(q + 1);
        rpath.erase(q);
    }

    t.has_authority = true;
    t.userinfo      = b.userinfo;
    t.host          = b.host;
    t.port          = b.port;

    if (rpath.empty()) {
        t.path      = b.path.empty() ? "/" : b.path;
        t.has_query = hq || b.has_query;
        t.query     = hq ? rquery : b.query;
    } else {
        if (rpath[0] == '/') {
            t.path = remove_dot_segments(rpath);
        } else {
            std::string merged;
            if (b.path.empty()) merged = "/" + rpath;
            else {
                size_t slash = b.path.rfind('/');
                merged       = (slash == std::string::npos ? std::string("/") : b.path.substr(0, slash + 1)) + rpath;
            }
            t.path = remove_dot_segments(merged);
        }
        t.has_query = hq;
        t.query     = rquery;
    }
    t.has_fragment = hf;
    t.fragment     = rfrag;
    return serialize_url(t);
}

std::string percent_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
        else if (c == ' ') out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(const std::string& s, bool plus_as_space) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else if (s[i] == '+' && plus_as_space) out += ' ';
        else out += s[i];
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string unwrap_redirect(const std::string& url) {
    ParsedUrl pu;
    if (!parse_url(url, pu) || (pu.scheme != "https" && pu.scheme != "http") || !pu.has_query) return url;
    std::string host = to_lower(pu.host);
    auto        site = [&](const char* domain) {
        std::string d = domain;
        return host == d || (host.size() > d.size() && host.compare(host.size() - d.size() - 1, d.size() + 1, "." + d) == 0);
    };
    std::string target;
    if (site("duckduckgo.com") && (pu.path == "/l/" || pu.path == "/l")) target = query_param(pu.query, "uddg");
    else if ((site("google.com") || host.rfind("www.google.", 0) == 0) && pu.path == "/url") {
        target = query_param(pu.query, "q");
        if (target.empty()) target = query_param(pu.query, "url");
    }
    ParsedUrl tu;
    if (target.empty() || !parse_url(target, tu) || (tu.scheme != "https" && tu.scheme != "http") || tu.host.empty()) return url;
    return target;
}

std::string punycode_label(const std::string& label) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < label.size();) { // decode UTF-8 (invalid bytes become U+FFFD)
        unsigned char c = static_cast<unsigned char>(label[i]);
        uint32_t      cp;
        int           n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { cps.push_back(0xFFFD); i++; continue; }
        if (i + static_cast<size_t>(n) > label.size()) { cps.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < n; k++) {
            unsigned char cc = static_cast<unsigned char>(label[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        cps.push_back(ok ? cp : 0xFFFD);
        i += ok ? static_cast<size_t>(n) : 1;
    }
    bool ascii = true;
    for (uint32_t cp : cps) if (cp >= 0x80) ascii = false;
    if (ascii) return label;
    const uint32_t base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    auto digit = [](uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); };
    auto adapt = [&](uint32_t delta, uint32_t points, bool first) {
        delta = first ? delta / damp : delta / 2;
        delta += delta / points;
        uint32_t k = 0;
        while (delta > ((base - tmin) * tmax) / 2) { delta /= base - tmin; k += base; }
        return k + (base - tmin + 1) * delta / (delta + skew);
    };
    std::string out;
    for (uint32_t cp : cps) if (cp < 0x80) out += static_cast<char>(std::tolower(static_cast<int>(cp)));
    uint32_t b = static_cast<uint32_t>(out.size()), h = b;
    if (b > 0) out += '-';
    uint32_t n = 0x80, delta = 0, bias = 72;
    while (h < cps.size()) {
        uint32_t m = 0xFFFFFFFF;
        for (uint32_t cp : cps) if (cp >= n && cp < m) m = cp;
        if ((m - n) > (0xFFFFFFFF - delta) / (h + 1)) return label; // overflow: leave it alone
        delta += (m - n) * (h + 1);
        n = m;
        for (uint32_t cp : cps) {
            if (cp < n) delta++;
            if (cp == n) {
                uint32_t q = delta;
                for (uint32_t k = base;; k += base) {
                    uint32_t t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
                    if (q < t) break;
                    out += digit(t + (q - t) % (base - t));
                    q = (q - t) / (base - t);
                }
                out += digit(q);
                bias  = adapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        n++;
    }
    return "xn--" + out;
}

std::string display_host(const std::string& host) {
    bool ascii = true;
    for (char c : host) if (static_cast<unsigned char>(c) >= 0x80) ascii = false;
    if (ascii) return host;
    std::string out;
    size_t      pos = 0;
    while (pos <= host.size()) {
        size_t dot = host.find('.', pos);
        if (dot == std::string::npos) dot = host.size();
        if (!out.empty() || pos > 0) out += '.';
        out += punycode_label(host.substr(pos, dot - pos));
        pos = dot + 1;
    }
    return out;
}

std::string display_url(const std::string& url) {
    bool ascii = true;
    for (char c : url) if (static_cast<unsigned char>(c) >= 0x80) ascii = false;
    if (ascii) return url;
    ParsedUrl pu;
    if (!parse_url(url, pu) || pu.host.empty()) return url;
    std::string h = display_host(pu.host);
    if (h == pu.host) return url;
    pu.host = h;
    return serialize_url(pu);
}

std::string query_param(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t      eq   = pair.find('=');
        std::string k    = percent_decode(eq == std::string::npos ? pair : pair.substr(0, eq), true);
        if (k == key) return eq == std::string::npos ? "" : percent_decode(pair.substr(eq + 1), true);
        pos = amp + 1;
    }
    return "";
}

} // namespace quill
