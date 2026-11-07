// Certificate pinning helpers: used to "trust this certificate" for a private
// (LAN) host that serves a self-signed certificate.
//
// A pin is curl's format: "sha256//" + base64(SHA-256 of the certificate's
// SubjectPublicKeyInfo). Pinning the public key (not the whole certificate)
// means the exception survives a certificate renewal that keeps the same key,
// and nothing else can pass it.
#pragma once
#include <string>

namespace quill {

std::string sha256_bytes(const std::string& data);                 // 32 raw bytes
std::string base64_encode(const std::string& raw);
bool        base64_decode(const std::string& text, std::string& raw); // ignores whitespace
std::string hex_colon(const std::string& raw);                      // "AB:CD:..."

// Extracts the DER of the first CERTIFICATE in a PEM block.
bool pem_to_der(const std::string& pem, std::string& der);
// Returns "sha256//<base64>" for the certificate's public key, or "" if it cannot be parsed.
std::string spki_pin_from_der(const std::string& der);
std::string spki_pin_from_pem(const std::string& pem);
// "sha256//..." pin syntax check.
bool is_valid_pin(const std::string& pin);

} // namespace quill
