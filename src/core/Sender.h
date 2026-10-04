#pragma once

#include "GmailClient.h"

#include <QObject>
#include <functional>

namespace zmail {

// Uploads a built RFC 5322 message to Gmail: users.messages.send, or
// users.drafts.create / update. Picks the transport by encoded size
// (limits::kSimpleUploadMaxBytes) and refuses anything over
// limits::kSendLimitBytes before a single byte leaves the machine.
//
//  < 5 MB : POST /messages/send  {"raw": base64url, "threadId": ...}
//           (JSON "raw" rather than uploadType=media, because a media
//            upload has nowhere to carry threadId)
//  >= 5 MB: resumable upload. POST /upload/.../messages/send?uploadType=resumable
//           with {"threadId"} metadata, then PUT the bytes to the session URI.
//           If the PUT fails part-way, ask the session how much arrived
//           (Content-Range: bytes */N -> 308 + Range) and send the rest.
class Sender : public QObject
{
    Q_OBJECT

public:
    enum class Transport { None, Simple, Resumable };

    struct Result
    {
        bool ok = false;
        bool blocked = false;       // over the size limit; nothing was sent
        QString messageId;          // Gmail id of the sent message / draft's message
        QString threadId;
        QString draftId;            // drafts only
        Transport transport = Transport::None;
        int resumes = 0;            // resumable: how many times we resumed
        ApiError err;
    };
    using ResultCb = std::function<void(const Result &)>;

    explicit Sender(GmailClient *client, QObject *parent = nullptr);

    static Transport transportFor(qint64 bytes);

    void send(const QByteArray &mime, const QString &threadId, ResultCb cb);
    // existingDraftId empty -> drafts.create, else drafts.update.
    void saveDraft(const QByteArray &mime, const QString &threadId, const QString &existingDraftId, ResultCb cb);
    void deleteDraft(const QString &draftId, ResultCb cb);

    void setMaxResumes(int n) { m_maxResumes = n; }

signals:
    void progress(qint64 sent, qint64 total);

private:
    struct Job;
    void startResumable(std::shared_ptr<Job> job);
    void putBytes(std::shared_ptr<Job> job, qint64 offset);
    void queryOffset(std::shared_ptr<Job> job);
    void finish(std::shared_ptr<Job> job, const QJsonObject &json, const ApiError &err);

    GmailClient *m_client;
    int m_maxResumes = 5;
};

} // namespace zmail
