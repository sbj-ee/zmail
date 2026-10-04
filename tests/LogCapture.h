#pragma once

#include <QLoggingCategory>
#include <QMutex>
#include <QStringList>

// Captures every qDebug/qInfo/qWarning (all zmail.* categories enabled at
// debug level) so tests can assert that no secret ever reaches a log.
class LogCapture
{
public:
    LogCapture()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("zmail.*=true\nqt.network.*=true"));
        s_self = this;
        m_prev = qInstallMessageHandler(&LogCapture::handler);
    }
    ~LogCapture()
    {
        qInstallMessageHandler(m_prev);
        s_self = nullptr;
    }
    QStringList lines() const
    {
        QMutexLocker l(&m_mutex);
        return m_lines;
    }
    QString all() const { return lines().join(QLatin1Char('\n')); }

private:
    static void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
    {
        if (s_self) {
            QMutexLocker l(&s_self->m_mutex);
            s_self->m_lines.append(QStringLiteral("%1: %2").arg(QString::fromLatin1(ctx.category ? ctx.category : "default"), msg));
        }
        if (s_self && s_self->m_prev) {
            s_self->m_prev(type, ctx, msg);
        }
    }
    static inline LogCapture *s_self = nullptr;
    QtMessageHandler m_prev = nullptr;
    mutable QMutex m_mutex;
    QStringList m_lines;
};
