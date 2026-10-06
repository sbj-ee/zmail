#include "AddressGuard.h"

namespace zmail::net {

namespace {
bool blockedV4(quint32 a)
{
    struct Range { quint32 net; int bits; };
    static constexpr Range ranges[] = {
        {0x00000000u, 8},  // 0.0.0.0/8 "this network"
        {0x0A000000u, 8},  // 10/8
        {0x64400000u, 10}, // 100.64/10 CGNAT, Tailscale
        {0x7F000000u, 8},  // 127/8 loopback
        {0xA9FE0000u, 16}, // 169.254/16 link-local (cloud metadata)
        {0xAC100000u, 12}, // 172.16/12
        {0xC0A80000u, 16}, // 192.168/16
        {0xE0000000u, 4},  // 224/4 multicast
        {0xF0000000u, 4},  // 240/4 reserved, incl. 255.255.255.255
    };
    for (const Range &r : ranges) {
        const quint32 mask = r.bits == 0 ? 0 : ~quint32(0) << (32 - r.bits);
        if ((a & mask) == r.net) {
            return true;
        }
    }
    return false;
}
} // namespace

bool isBlockedAddress(const QHostAddress &address)
{
    if (address.isNull()) {
        return true;
    }
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        return blockedV4(address.toIPv4Address());
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol) {
        return true;
    }
    const Q_IPV6ADDR v6 = address.toIPv6Address();
    const quint8 *b = v6.c;
    bool firstTenZero = true;
    for (int i = 0; i < 10; ++i) {
        firstTenZero = firstTenZero && b[i] == 0;
    }
    const quint32 low = (quint32(b[12]) << 24) | (quint32(b[13]) << 16) | (quint32(b[14]) << 8) | quint32(b[15]);
    if (firstTenZero && b[10] == 0xff && b[11] == 0xff) {
        return blockedV4(low); // ::ffff:a.b.c.d
    }
    if (firstTenZero && b[10] == 0 && b[11] == 0) {
        // ::, ::1 and the deprecated IPv4-compatible ::a.b.c.d (low == 0 or 1
        // fall in 0.0.0.0/8, so they're covered too).
        return blockedV4(low);
    }
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) {
        return true; // fe80::/10 link-local
    }
    if ((b[0] & 0xfe) == 0xfc) {
        return true; // fc00::/7 unique local
    }
    if (b[0] == 0xff) {
        return true; // ff00::/8 multicast
    }
    return false;
}

bool isBlockedLiteral(const QString &host)
{
    QString h = host.trimmed();
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']'))) {
        h = h.mid(1, h.size() - 2);
    }
    QHostAddress a;
    if (!a.setAddress(h)) {
        return false;
    }
    return isBlockedAddress(a);
}

QHostAddress pickAllowed(const QList<QHostAddress> &addresses, bool allowLoopback)
{
    QHostAddress pick;
    for (const QHostAddress &a : addresses) {
        if (isBlockedAddress(a) && !(allowLoopback && a.isLoopback())) {
            continue;
        }
        if (pick.isNull() || (pick.protocol() != QAbstractSocket::IPv4Protocol && a.protocol() == QAbstractSocket::IPv4Protocol)) {
            pick = a;
        }
    }
    return pick;
}

} // namespace zmail::net
