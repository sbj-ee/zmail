#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

class QNetworkAccessManager;
class QTimer;

namespace zmail {

class AuthManager;

struct ApiError
{
    int httpStatus = 0;   // 0 = network error
    QString reason;       // Google's error.status / errors[0].reason, if any
    QString message;
    bool isError = false;
    static ApiError none() { return {}; }
};

// Minimal Gmail REST v1 client: bearer auth with one retry on 401, a quota
// token bucket (6,000 units/min/user), and exponential backoff with jitter on
// 429 / 5xx / rate-limit 403s, honouring Retry-After.
class GmailClient : public QObject
{
    Q_OBJECT

public:
    using JsonCb = std::function<void(const QJsonObject &json, const ApiError &err)>;

    static QUrl defaultBaseUrl() { return QUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1/users/me")); }

    GmailClient(AuthManager *auth, QNetworkAccessManager *nam, QUrl base = defaultBaseUrl(), QObject *parent = nullptr);

    // Quota units per Google's table (developers.google.com/workspace/gmail/api/reference/quota).
    void getProfile(JsonCb cb);
    void listLabels(JsonCb cb);
    void listMessages(const QString &labelId, int maxResults, const QString &pageToken, JsonCb cb);
    void getMessageMetadata(const QString &id, JsonCb cb);
    void getMessageFull(const QString &id, JsonCb cb);
    void modifyLabels(const QString &id, const QStringList &add, const QStringList &remove, JsonCb cb);
    void listHistory(const QString &startHistoryId, const QString &pageToken, JsonCb cb);

    // Tuning (tests use tiny values).
    void setBackoffBaseMs(int ms) { m_backoffBaseMs = ms; }
    void setMaxAttempts(int n) { m_maxAttempts = n; }
    void setQuota(int unitsPerMinute, int burst);

    int requestsSent() const { return m_sent; }
    int retries() const { return m_retries; }

private:
    struct Call
    {
        QByteArray verb;
        QString path;
        QUrlQuery query;
        QByteArray body;
        int units = 5;
        int attempt = 0;
        bool reauthed = false;
        JsonCb cb;
    };
    void enqueue(Call c);
    void pump();
    void send(Call c);
    void retryLater(Call c, int delayMs);

    AuthManager *m_auth;
    QNetworkAccessManager *m_nam;
    QUrl m_base;
    int m_backoffBaseMs = 1000;
    int m_maxAttempts = 6;
    int m_sent = 0;
    int m_retries = 0;

    // Token bucket
    double m_tokens = 600;
    double m_capacity = 600;
    double m_refillPerMs = 90.0 / 1000.0; // 600 + 90/s*60 = 6,000 units per minute
    qint64 m_lastRefill = 0;
    QList<Call> m_queue;
    QTimer *m_pumpTimer = nullptr;
};

} // namespace zmail
