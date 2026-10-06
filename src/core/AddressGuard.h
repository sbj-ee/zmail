#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>

namespace zmail::net {

// Addresses an untrusted URL (a remote image in a mail) must never reach:
// the machine itself, the LAN, the tailnet and anything else that isn't the
// public internet. IPv4 0/8, 127/8, 10/8, 172.16/12, 192.168/16,
// 169.254/16, 100.64/10 (CGNAT, Tailscale), 224/4 multicast, 240/4 and the
// broadcast address; IPv6 ::, ::1, fe80::/10, fc00::/7, ff00::/8; and the
// IPv4-mapped (::ffff:a.b.c.d) and IPv4-compatible (::a.b.c.d) forms of the
// IPv4 ranges. An invalid address counts as blocked.
bool isBlockedAddress(const QHostAddress &address);

// A host string that is an IP literal in a blocked range (brackets allowed).
// Hostnames return false: check what they resolve to.
bool isBlockedLiteral(const QString &host);

// Of the addresses a name resolved to, the one to connect to: the first
// allowed IPv4 address, else the first allowed IPv6 one; null if none is
// allowed. allowLoopback is for the tests' local servers only.
QHostAddress pickAllowed(const QList<QHostAddress> &addresses, bool allowLoopback = false);

} // namespace zmail::net
