#pragma once

// One way of writing a byte count everywhere in zmail: the message list,
// the status bar, the compose size meter, attachment chips and the zip
// prompt. Decimal units (1 KB = 1000 bytes), the same convention as Gmail's
// limits in Limits.h, so "25 MB" means the same thing on every screen.
//
//   0..999 B          -> "1 KB" (never "0 KB" for a real message)
//   < 1 MB            -> whole KB, rounded up: "3 KB", "97 KB", "999 KB"
//   < 1 GB            -> MB, one decimal:      "1.2 MB", "18.5 MB", "25 MB"
//   otherwise         -> GB, one decimal       (a trailing ".0" is dropped)

#include <QLocale>
#include <QString>
#include <QtGlobal>

namespace zmail {

inline QString formatSize(qint64 bytes)
{
    const QLocale en(QLocale::English);
    auto oneDecimal = [&en](double v) {
        QString s = en.toString(v, 'f', 1);
        if (s.endsWith(QLatin1String(".0"))) {
            s.chop(2);
        }
        return s;
    };
    if (bytes < 1'000'000) {
        const qint64 kb = qMax<qint64>(1, (qMax<qint64>(0, bytes) + 999) / 1000);
        if (kb < 1000) {
            return QStringLiteral("%1 KB").arg(kb);
        }
        bytes = 1'000'000; // 999,501..999,999 B would round up to "1000 KB"
    }
    if (bytes < 1'000'000'000) {
        return QStringLiteral("%1 MB").arg(oneDecimal(double(bytes) / 1e6));
    }
    return QStringLiteral("%1 GB").arg(oneDecimal(double(bytes) / 1e9));
}

// "2,957 bytes"
inline QString formatExactBytes(qint64 bytes)
{
    return QStringLiteral("%1 bytes").arg(QLocale(QLocale::English).toString(bytes));
}

} // namespace zmail
