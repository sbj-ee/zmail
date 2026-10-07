#pragma once

#include <QDateTime>
#include <QHash>
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
// token bucket (6,000 units/min/user), a hard in-flight concurrency cap (so a
// labels.get burst cannot stampede), pump() yields to the event loop each
// tick, and exponential backoff with jitter on 429 / 5xx / rate-limit 403s,
// honouring Retry-After. A 429 also arms a client-wide cooldown so retries
// do not keep hammering while the bucket still has tokens.
class GmailClient : public QObject
{
    Q_OBJECT

public:
    using JsonCb = std::function<void(const QJsonObject &json, const ApiError &err)>;

    // Interactive calls (opening a message, a label change, sending) go ahead
    // of queued Background ones (sync listings and metadata), so the user
    // never waits behind a bulk fetch. First in, first out within each.
    enum class Priority { Interactive, Background };

    // Low-level request, for uploads and anything needing headers/status.
    struct Request
    {
        QByteArray verb = "GET";        // GET, POST, PUT, DELETE
        QString path;                   // relative to the API base...
        bool upload = false;            // ...or to the /upload/ variant of it
        QUrl absoluteUrl;               // ...or exactly this (resumable session URIs)
        QUrlQuery query;
        QByteArray body;
        QByteArray contentType = "application/json";
        QList<QPair<QByteArray, QByteArray>> headers;
        int units = 5;
        bool retryTransient = true;     // false: caller handles failures (resumable PUT)
        int timeoutMs = 60000;
        Priority priority = Priority::Interactive;
        std::function<void(qint64 sent, qint64 total)> progress;
    };
    struct RawReply
    {
        int status = 0;
        QByteArray body;
        QJsonObject json;
        QHash<QByteArray, QByteArray> headers; // lower-cased names
        ApiError err;                          // set for anything outside 2xx
    };
    using RawCb = std::function<void(const RawReply &reply)>;
    void request(Request rq, RawCb cb);

    static QUrl defaultBaseUrl() { return QUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1/users/me")); }

    GmailClient(AuthManager *auth, QNetworkAccessManager *nam, QUrl base = defaultBaseUrl(), QObject *parent = nullptr);

    // Quota units per Google's table (developers.google.com/workspace/gmail/api/reference/quota).
    void getProfile(JsonCb cb);
    void listLabels(JsonCb cb);
    // users.labels.get — includes messagesTotal / messagesUnread
    // (labels.list omits those fields on real Gmail).
    void getLabel(const QString &id, JsonCb cb);
    void createLabel(const QString &name, const QString &backgroundColor, JsonCb cb);
    void updateLabel(const QString &id, const QString &name, const QString &backgroundColor, JsonCb cb);
    void deleteLabel(const QString &id, JsonCb cb);
    void listMessages(const QString &labelId, int maxResults, const QString &pageToken, JsonCb cb);
    // users.messages.batchModify: up to 1000 ids, up to 100 labels each way.
    void batchModifyLabels(const QStringList &ids, const QStringList &add, const QStringList &remove, JsonCb cb);
    void getMessageMetadata(const QString &id, JsonCb cb);
    void getMessageFull(const QString &id, JsonCb cb);
    void modifyLabels(const QString &id, const QStringList &add, const QStringList &remove, JsonCb cb);
    void trashMessage(const QString &id, JsonCb cb); // users.messages.trash
    void untrashMessage(const QString &id, JsonCb cb); // users.messages.untrash
    void listHistory(const QString &startHistoryId, const QString &pageToken, JsonCb cb);
    void listSendAs(JsonCb cb);
    void getAttachment(const QString &messageId, const QString &attachmentId, JsonCb cb);
    void deleteDraft(const QString &draftId, JsonCb cb);

    QUrl baseUrl() const { return m_base; }
    QUrl uploadBaseUrl() const; // https://host/upload/<base path>

    // Tuning (tests use tiny values).
    void setBackoffBaseMs(int ms) { m_backoffBaseMs = ms; }
    void setMaxAttempts(int n) { m_maxAttempts = n; }
    void setQuota(int unitsPerMinute, int burst);
    // Cap outstanding HTTP calls. The token bucket alone still allowed a
    // 600-unit burst of parallel GETs (labels.get storm → HTTP 429 livelock).
    void setMaxInFlight(int n);
    // How many requests one pump() tick may start before yielding to the
    // event loop (keeps the UI thread from busy-draining the queue).
    void setMaxSendsPerPump(int n);

    int requestsSent() const { return m_sent; }
    int retries() const { return m_retries; }
    int inFlight() const { return m_inFlight; }
    int maxInFlight() const { return m_maxInFlight; }

private:
    struct Call
    {
        Request rq;
        int attempt = 0;
        bool reauthed = false;
        RawCb cb;
    };
    void call(QByteArray verb, QString path, QUrlQuery q, QByteArray body, int units, JsonCb cb,
              Priority priority = Priority::Interactive);
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

    // Concurrency + 429 cooldown (in addition to the token bucket).
    int m_maxInFlight = 4;
    int m_maxSendsPerPump = 1;
    int m_inFlight = 0;
    qint64 m_cooldownUntilMs = 0; // pump() stays idle until nowMs() passes this
};

} // namespace zmail
