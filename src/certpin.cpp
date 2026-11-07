#include "certpin.h"

#include <cstdint>
#include <cstring>

namespace quill {

namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void compress(uint32_t h[8], const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = hh + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

} // namespace

std::string sha256_bytes(const std::string& data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t   n    = data.size();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t i = 0;
    for (; i + 64 <= n; i += 64) compress(h, p + i);
    unsigned char tail[128] = {0};
    size_t        rem       = n - i;
    if (rem) std::memcpy(tail, p + i, rem);
    tail[rem]       = 0x80;
    size_t   tlen   = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits   = static_cast<uint64_t>(n) * 8;
    for (int k = 0; k < 8; k++) tail[tlen - 1 - k] = static_cast<unsigned char>(bits >> (8 * k));
    compress(h, tail);
    if (tlen == 128) compress(h, tail + 64);
    std::string out(32, '\0');
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = static_cast<char>(h[k] >> (24 - 8 * j));
    return out;
}

std::string base64_encode(const std::string& raw) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    size_t             i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        uint32_t v = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8) |
                     static_cast<unsigned char>(raw[i + 2]);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (i + 1 == raw.size()) {
        uint32_t v = static_cast<unsigned char>(raw[i]) << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == raw.size()) {
        uint32_t v = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::string& raw) {
    raw.clear();
    uint32_t acc = 0;
    int      bits = 0;
    for (unsigned char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        else return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

std::string hex_colon(const std::string& raw) {
    static const char* H = "0123456789ABCDEF";
    std::string        out;
    for (size_t i = 0; i < raw.size(); i++) {
        if (i) out += ':';
        unsigned char c = static_cast<unsigned char>(raw[i]);
        out += H[c >> 4];
        out += H[c & 15];
    }
    return out;
}

bool pem_to_der(const std::string& pem, std::string& der) {
    const std::string b = "-----BEGIN CERTIFICATE-----", e = "-----END CERTIFICATE-----";
    size_t            s = pem.find(b);
    if (s == std::string::npos) return false;
    s += b.size();
    size_t t = pem.find(e, s);
    if (t == std::string::npos) return false;
    return base64_decode(pem.substr(s, t - s), der) && !der.empty();
}

namespace {

// Reads one DER element header at `pos`. On success, `hdr` is the header size and `len` the content length.
bool tlv(const std::string& d, size_t pos, size_t end, unsigned char& tag, size_t& hdr, size_t& len) {
    if (pos + 2 > end) return false;
    tag = static_cast<unsigned char>(d[pos]);
    unsigned char l = static_cast<unsigned char>(d[pos + 1]);
    if (l < 0x80) {
        hdr = 2;
        len = l;
    } else {
        int nb = l & 0x7F;
        if (nb < 1 || nb > 4 || pos + 2 + static_cast<size_t>(nb) > end) return false;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | static_cast<unsigned char>(d[pos + 2 + i]);
        hdr = 2 + static_cast<size_t>(nb);
    }
    return pos + hdr + len <= end && pos + hdr + len >= pos;
}

} // namespace

std::string spki_pin_from_der(const std::string& der) {
    unsigned char tag;
    size_t        hdr, len;
    if (!tlv(der, 0, der.size(), tag, hdr, len) || tag != 0x30) return {};          // Certificate
    size_t cs = hdr, ce = hdr + len;
    if (!tlv(der, cs, ce, tag, hdr, len) || tag != 0x30) return {};                  // TBSCertificate
    size_t pos = cs + hdr, end = cs + hdr + len;
    // [0] version (optional), serial, signature, issuer, validity, subject, then SPKI
    if (pos < end && static_cast<unsigned char>(der[pos]) == 0xA0) {
        if (!tlv(der, pos, end, tag, hdr, len)) return {};
        pos += hdr + len;
    }
    for (int i = 0; i < 5; i++) { // serial, sigalg, issuer, validity, subject
        if (!tlv(der, pos, end, tag, hdr, len)) return {};
        pos += hdr + len;
    }
    if (!tlv(der, pos, end, tag, hdr, len) || tag != 0x30) return {};
    std::string spki = der.substr(pos, hdr + len);
    return "sha256//" + base64_encode(sha256_bytes(spki));
}

std::string spki_pin_from_pem(const std::string& pem) {
    std::string der;
    if (!pem_to_der(pem, der)) return {};
    return spki_pin_from_der(der);
}

bool is_valid_pin(const std::string& pin) {
    if (pin.compare(0, 8, "sha256//") != 0) return false;
    std::string raw;
    return base64_decode(pin.substr(8), raw) && raw.size() == 32;
}

} // namespace quill
