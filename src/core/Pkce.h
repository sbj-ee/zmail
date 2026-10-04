#pragma once

#include <QByteArray>

namespace zmail::pkce {

// RFC 7636: 64 random bytes -> 86-char base64url verifier (43..128 allowed).
QByteArray makeVerifier();
// S256 challenge: base64url(SHA-256(verifier)), no padding.
QByteArray challengeS256(const QByteArray &verifier);
// Random opaque state for CSRF protection.
QByteArray makeState();

} // namespace zmail::pkce
