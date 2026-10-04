#pragma once

#include "core/ClientConfig.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

class QTcpServer;
class QTcpSocket;

namespace zmail::test {

// A tiny in-process stand-in for accounts.google.com, oauth2.googleapis.com
// and gmail.googleapis.com, served over plain HTTP on 127.0.0.1. Tests (and
// the screenshot tool) point zmail at it so nothing ever talks to Google.
class MockGoogle : public QObject
{
    Q_OBJECT

public:
    struct Message
    {
        QString id;
        QString threadId;
        QStringList labels;
        QString from;     // "Name <addr>"
        QString to;
        QString subject;
        QDateTime date;
        QString snippet;
        QString text;
        QString html;
        QStringList attachments;
        qint64 size = 4000;
        QString cc;
        QString replyTo;
        QString messageIdHeader;   // "" = generated "<id@mock.example>"
        QString references;
        QString inReplyTo;
        QByteArray raw;            // as uploaded (sent messages / drafts)
        QList<QByteArray> attachmentData; // served by attachments.get (default: generated)
    };
    struct Label
    {
        QString id, name, type, color;
    };
    struct Fault
    {
        QString pathPrefix;
        int status = 500;
        int remaining = 1;
        int retryAfter = -1;
    };

    static constexpr const char *kClientId = "mock-client-id.apps.example.test";
    static constexpr const char *kClientSecret = "mock-client-secret-NEVER-LOG";
    static constexpr const char *kRefreshToken = "mock-refresh-token-NEVER-LOG";
    static constexpr const char *kAccessPrefix = "mock-access-token-NEVER-LOG-";

    explicit MockGoogle(QObject *parent = nullptr);
    ~MockGoogle() override;

    bool listen();
    QUrl baseUrl() const;
    QUrl authUri() const { return baseUrl().resolved(QUrl(QStringLiteral("/o/oauth2/v2/auth"))); }
    QUrl tokenUri() const { return baseUrl().resolved(QUrl(QStringLiteral("/token"))); }
    QUrl revokeUri() const { return baseUrl().resolved(QUrl(QStringLiteral("/revoke"))); }
    QUrl apiBase() const { return baseUrl().resolved(QUrl(QStringLiteral("/gmail/v1/users/me"))); }
    ClientConfig clientConfig() const;

    // Account
    QString email = QStringLiteral("demo.user@example.com");
    int accessTokenLifetime = 3600;
    bool grantGmailScope = true;

    // Data
    void addLabel(const Label &l) { m_labels.append(l); }
    QString addMessage(Message m, bool recordHistory = true);
    void deleteMessage(const QString &id);
    void setMessageLabels(const QString &id, const QStringList &add, const QStringList &remove);
    void seedSystemLabels();
    // Fictional demo mailbox (sample data + more) for screenshots.
    void seedDemo(int extraMessages = 40);
    qint64 historyId() const { return m_historyId; }
    // Forget history older than now: the next history.list from an old id returns 404.
    void expireHistory() { m_minHistoryId = m_historyId; }
    int historyPageSize = 100;
    const QMap<QString, Message> &messages() const { return m_messages; }

    // Sending (0.3.0)
    QString displayName = QStringLiteral("Demo User");
    struct Draft
    {
        QString id;
        QString messageId;
    };
    const QMap<QString, Draft> &drafts() const { return m_drafts; }
    QByteArray lastRaw;            // last message received by send / drafts
    QString lastSendPath;          // "simple" or "resumable"
    int sendCalls = 0;
    int uploadSessions = 0;
    int uploadPuts = 0;
    int statusQueries = 0;         // Content-Range: bytes */N
    qint64 failUploadAfterBytes = -1; // next resumable PUT stores this many bytes, then 503
    static constexpr qint64 kUploadMaxBytes = 36'700'160; // Gmail discovery mediaUpload.maxSize
    QString lastUploadId;

    // Failure injection
    void addFault(const Fault &f) { m_faults.append(f); }
    void revokeRefreshToken() { m_refreshRevoked = true; }
    bool refreshRevoked() const { return m_refreshRevoked; }

    // Observations
    QStringList requests;      // "GET /gmail/v1/users/me/labels"
    int count(const QString &prefix) const;
    QList<QByteArray> requestBodies;
    QString lastVerifierChecked;
    bool pkceVerified = false;
    QStringList modifyCalls;   // "id:-UNREAD"
    QStringList trashCalls;    // message ids sent to users.messages.trash
    int tokensIssued = 0;

private:
    struct HistoryRecord
    {
        qint64 id;
        QString type; // messagesAdded / messagesDeleted / labelsAdded / labelsRemoved
        QString messageId;
        QStringList labels;
    };
    struct Pending
    {
        QByteArray challenge;
        QString redirect;
        QString scope;
    };

    void onReadyRead(QTcpSocket *s);
    void handle(QTcpSocket *s, const QByteArray &method, const QUrl &url, const QHash<QByteArray, QByteArray> &headers,
                const QByteArray &body);
    void reply(QTcpSocket *s, int status, const QByteArray &body, const QByteArray &contentType = "application/json",
               const QList<QPair<QByteArray, QByteArray>> &extra = {});
    void replyJson(QTcpSocket *s, int status, const QJsonObject &o);
    bool authorized(const QHash<QByteArray, QByteArray> &headers) const;
    QJsonObject messageJson(const Message &m, const QString &format) const;
    QString issueAccessToken();
    QString idToken() const;

    QTcpServer *m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<Label> m_labels;
    QMap<QString, Message> m_messages;
    QList<HistoryRecord> m_history;
    qint64 m_historyId = 1000;
    qint64 m_minHistoryId = 0;
    int m_nextId = 1;
    QHash<QString, Pending> m_codes;
    QStringList m_validAccess;
    bool m_refreshRevoked = false;
    QList<Fault> m_faults;

    struct Upload
    {
        QString kind;     // send / draftCreate / draftUpdate
        QString draftId;
        QJsonObject meta;
        qint64 expected = 0;
        QByteArray data;
        bool done = false;
        QJsonObject result;
    };
    QHash<QString, Upload> m_uploads;
    QMap<QString, Draft> m_drafts;
    int m_nextDraft = 1;
    void handleUpload(QTcpSocket *s, const QByteArray &method, const QString &rest, const QUrlQuery &q,
                      const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
    // Stores an uploaded RFC 822 message; returns the API response object.
    QJsonObject acceptSend(const QByteArray &raw, const QString &threadId, int *status);
    QJsonObject acceptDraft(const QByteArray &raw, const QString &threadId, const QString &draftId, int *status);
    Message messageFromRaw(const QByteArray &raw, const QString &threadId) const;
};

} // namespace zmail::test
