#include "Sender.h"

#include "Limits.h"
#include "Log.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>

namespace zmail {

struct Sender::Job
{
    QByteArray mime;
    QString threadId;
    QString draftId;
    bool draft = false;
    QUrl session;
    int resumes = 0;
    Transport transport = Transport::None;
    ResultCb cb;
};

Sender::Sender(GmailClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

Sender::Transport Sender::transportFor(qint64 bytes)
{
    if (bytes > limits::kSendLimitBytes || bytes > limits::kApiSendUploadMaxBytes) {
        return Transport::None;
    }
    return bytes < limits::kSimpleUploadMaxBytes ? Transport::Simple : Transport::Resumable;
}

namespace {
QJsonObject messageMeta(const QString &threadId)
{
    QJsonObject m;
    if (!threadId.isEmpty()) {
        m.insert(QStringLiteral("threadId"), threadId);
    }
    return m;
}
} // namespace

void Sender::send(const QByteArray &mime, const QString &threadId, ResultCb cb)
{
    auto job = std::make_shared<Job>();
    job->mime = mime;
    job->threadId = threadId;
    job->cb = std::move(cb);
    job->transport = transportFor(mime.size());
    if (job->transport == Transport::None) {
        Result r;
        r.blocked = true;
        r.err.isError = true;
        r.err.reason = QStringLiteral("messageTooLarge");
        r.err.message = tr("The message is %1 MB; Gmail accepts up to %2 MB.")
                            .arg(double(mime.size()) / 1e6, 0, 'f', 1)
                            .arg(double(limits::kSendLimitBytes) / 1e6, 0, 'f', 0);
        qCInfo(lcGmail) << "send blocked:" << mime.size() << "bytes";
        job->cb(r);
        return;
    }
    if (job->transport == Transport::Resumable) {
        startResumable(job);
        return;
    }
    QJsonObject body = messageMeta(threadId);
    body.insert(QStringLiteral("raw"), QString::fromLatin1(mime.toBase64(QByteArray::Base64UrlEncoding)));
    GmailClient::Request rq;
    rq.verb = "POST";
    rq.path = QStringLiteral("/messages/send");
    rq.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    rq.units = 100;
    rq.progress = [this](qint64 s, qint64 t) { emit progress(s, t); };
    m_client->request(std::move(rq), [this, job](const GmailClient::RawReply &r) { finish(job, r.json, r.err); });
}

void Sender::saveDraft(const QByteArray &mime, const QString &threadId, const QString &existingDraftId, ResultCb cb)
{
    auto job = std::make_shared<Job>();
    job->mime = mime;
    job->threadId = threadId;
    job->draftId = existingDraftId;
    job->draft = true;
    job->cb = std::move(cb);
    job->transport = transportFor(mime.size());
    if (job->transport == Transport::None) {
        Result r;
        r.blocked = true;
        r.err.isError = true;
        r.err.reason = QStringLiteral("messageTooLarge");
        r.err.message = tr("The draft is too large to save to Gmail.");
        job->cb(r);
        return;
    }
    if (job->transport == Transport::Resumable) {
        startResumable(job);
        return;
    }
    QJsonObject msg = messageMeta(threadId);
    msg.insert(QStringLiteral("raw"), QString::fromLatin1(mime.toBase64(QByteArray::Base64UrlEncoding)));
    QJsonObject body{{QStringLiteral("message"), msg}};
    GmailClient::Request rq;
    rq.verb = existingDraftId.isEmpty() ? "POST" : "PUT";
    rq.path = existingDraftId.isEmpty() ? QStringLiteral("/drafts") : QStringLiteral("/drafts/") + existingDraftId;
    rq.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    rq.units = existingDraftId.isEmpty() ? 10 : 15;
    m_client->request(std::move(rq), [this, job](const GmailClient::RawReply &r) { finish(job, r.json, r.err); });
}

void Sender::deleteDraft(const QString &draftId, ResultCb cb)
{
    m_client->deleteDraft(draftId, [cb = std::move(cb), draftId](const QJsonObject &, const ApiError &err) {
        Result r;
        r.ok = !err.isError || err.httpStatus == 404; // already gone is fine
        r.draftId = draftId;
        r.err = err;
        cb(r);
    });
}

void Sender::startResumable(std::shared_ptr<Job> job)
{
    QJsonObject meta = messageMeta(job->threadId);
    if (job->draft) {
        meta = QJsonObject{{QStringLiteral("message"), meta}};
    }
    GmailClient::Request rq;
    rq.verb = job->draft && !job->draftId.isEmpty() ? "PUT" : "POST";
    rq.upload = true;
    rq.path = job->draft ? (job->draftId.isEmpty() ? QStringLiteral("/drafts") : QStringLiteral("/drafts/") + job->draftId)
                         : QStringLiteral("/messages/send");
    rq.query.addQueryItem(QStringLiteral("uploadType"), QStringLiteral("resumable"));
    rq.body = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    rq.contentType = "application/json; charset=UTF-8";
    rq.headers = {{"X-Upload-Content-Type", "message/rfc822"},
                  {"X-Upload-Content-Length", QByteArray::number(job->mime.size())}};
    rq.units = job->draft ? (job->draftId.isEmpty() ? 10 : 15) : 100;
    qCInfo(lcGmail) << "resumable upload of" << job->mime.size() << "bytes";
    m_client->request(std::move(rq), [this, job](const GmailClient::RawReply &r) {
        const QUrl loc(QString::fromLatin1(r.headers.value("location")));
        if (r.err.isError || !loc.isValid() || loc.isEmpty()) {
            ApiError e = r.err;
            if (!e.isError) {
                e.isError = true;
                e.message = tr("Gmail did not return an upload session.");
            }
            finish(job, {}, e);
            return;
        }
        // Only follow a session URI on the API host we were talking to.
        const QUrl base = m_client->baseUrl();
        const bool sameOrigin = loc.host() == base.host() && loc.port() == base.port() && loc.scheme() == base.scheme();
        const bool google = loc.scheme() == QLatin1String("https") &&
                            (loc.host() == QLatin1String("googleapis.com") ||
                             loc.host().endsWith(QLatin1String(".googleapis.com")));
        if (!sameOrigin && !google) {
            ApiError e;
            e.isError = true;
            e.message = tr("Unexpected upload session host.");
            finish(job, {}, e);
            return;
        }
        job->session = loc;
        putBytes(job, 0);
    });
}

void Sender::putBytes(std::shared_ptr<Job> job, qint64 offset)
{
    const qint64 total = job->mime.size();
    GmailClient::Request rq;
    rq.verb = "PUT";
    rq.absoluteUrl = job->session;
    rq.body = job->mime.mid(offset);
    rq.contentType = "message/rfc822";
    if (offset > 0) {
        rq.headers = {{"Content-Range", QStringLiteral("bytes %1-%2/%3").arg(offset).arg(total - 1).arg(total).toLatin1()}};
    }
    rq.units = 0; // quota was charged when the session was created
    rq.retryTransient = false;
    rq.timeoutMs = 300000;
    rq.progress = [this, offset, total](qint64 s, qint64) { emit progress(offset + s, total); };
    m_client->request(std::move(rq), [this, job](const GmailClient::RawReply &r) {
        if (!r.err.isError) {
            finish(job, r.json, r.err);
            return;
        }
        const int s = r.err.httpStatus;
        const bool retryable = s == 0 || s == 308 || s >= 500 || s == 429;
        if (!retryable || job->resumes >= m_maxResumes) {
            finish(job, {}, r.err);
            return;
        }
        ++job->resumes;
        qCInfo(lcGmail) << "upload interrupted (HTTP" << s << "); resume" << job->resumes;
        QTimer::singleShot(std::min(8000, 250 << job->resumes), this, [this, job] { queryOffset(job); });
    });
}

void Sender::queryOffset(std::shared_ptr<Job> job)
{
    GmailClient::Request rq;
    rq.verb = "PUT";
    rq.absoluteUrl = job->session;
    rq.contentType = "message/rfc822";
    rq.headers = {{"Content-Range", QStringLiteral("bytes */%1").arg(job->mime.size()).toLatin1()}};
    rq.units = 0;
    rq.retryTransient = false;
    m_client->request(std::move(rq), [this, job](const GmailClient::RawReply &r) {
        if (!r.err.isError) { // it all arrived after all
            finish(job, r.json, r.err);
            return;
        }
        if (r.err.httpStatus == 308) {
            // "Range: bytes=0-N" -> resume at N+1; no Range -> nothing stored yet.
            static const QRegularExpression re(QStringLiteral("bytes=0-(\\d+)"));
            const auto m = re.match(QString::fromLatin1(r.headers.value("range")));
            const qint64 next = m.hasMatch() ? m.captured(1).toLongLong() + 1 : 0;
            putBytes(job, std::min<qint64>(next, job->mime.size()));
            return;
        }
        if ((r.err.httpStatus == 0 || r.err.httpStatus >= 500) && job->resumes < m_maxResumes) {
            ++job->resumes;
            QTimer::singleShot(std::min(8000, 250 << job->resumes), this, [this, job] { queryOffset(job); });
            return;
        }
        finish(job, {}, r.err); // 404/410: session expired; caller may start over
    });
}

void Sender::finish(std::shared_ptr<Job> job, const QJsonObject &json, const ApiError &err)
{
    Result r;
    r.ok = !err.isError;
    r.err = err;
    r.transport = job->transport;
    r.resumes = job->resumes;
    if (job->draft) {
        r.draftId = json.value(QStringLiteral("id")).toString();
        const QJsonObject m = json.value(QStringLiteral("message")).toObject();
        r.messageId = m.value(QStringLiteral("id")).toString();
        r.threadId = m.value(QStringLiteral("threadId")).toString();
    } else {
        r.messageId = json.value(QStringLiteral("id")).toString();
        r.threadId = json.value(QStringLiteral("threadId")).toString();
    }
    if (r.ok) {
        qCInfo(lcGmail) << (job->draft ? "draft saved" : "message sent") << r.messageId << "thread" << r.threadId;
    }
    job->cb(r);
}

} // namespace zmail
