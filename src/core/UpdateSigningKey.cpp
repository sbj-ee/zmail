// The public key that release SHA256SUMS files are signed with (minisign).
// Not secret. Release builds refuse to install an update unless
// SHA256SUMS.minisig verifies against this key.
//
// Release key ID 32960BBA77B36F1F (minisign -G, October 2026). It is the same
// key that signs zterminal's releases (the same maintainer and the same release
// pipeline), so zmail's release workflow needs the same two repository
// secrets. To use a different key for zmail, replace the string below and the
// MINISIGN_* secrets together. See docs/RELEASING.md, "Signing key".
#include "Update.h"

namespace zmail {

namespace {
constexpr const char kUpdateSigningPublicKey[] = "RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9";

QString &testKey()
{
    static QString k;
    return k;
}
} // namespace

QString updateSigningPublicKey()
{
    if (!testKey().isEmpty()) {
        return testKey();
    }
    return QString::fromLatin1(kUpdateSigningPublicKey);
}

void setUpdateSigningPublicKeyForTests(const QString &base64)
{
    testKey() = base64;
}

} // namespace zmail
