#include "Pkce.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

namespace zmail::pkce {

namespace {
QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(b.data()),
                                         reinterpret_cast<quint32 *>(b.data() + (n / 4) * 4));
    for (int i = (n / 4) * 4; i < n; ++i) {
        b[i] = char(QRandomGenerator::system()->bounded(256));
    }
    return b;
}
constexpr auto kB64Url = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
} // namespace

QByteArray makeVerifier()
{
    return randomBytes(64).toBase64(kB64Url);
}

QByteArray challengeS256(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256).toBase64(kB64Url);
}

QByteArray makeState()
{
    return randomBytes(24).toBase64(kB64Url);
}

} // namespace zmail::pkce
