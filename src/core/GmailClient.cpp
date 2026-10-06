#include "GmailClient.h"

#include "AuthManager.h"
#include "Log.h"

#include <algorithm>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

namespace zmail {

namespace {
qint64 nowMs()
{
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t.elapsed();
}
} // namespace

GmailClient::GmailClient(AuthManager *auth, QNetworkAccessManager *nam, QUrl base, QObject *parent)
    : QObject(parent)
    , m_auth(auth)
    , m_nam(nam)
    , m_base(std::move(base))
    , m_pumpTimer(new QTimer(this))
{
    m_pumpTimer->setSingleShot(true);
    connect(m_pumpTimer, &QTimer::timeout, this, &GmailClient::pump);
    m_lastRefill = nowMs();
}

void GmailClient::setQuota(int unitsPerMinute, int burst)
{
    m_capacity = burst;
    m_tokens = burst;
    m_refillPerMs = double(std::max(0, unitsPerMinute - burst)) / 60000.0;
}

void GmailClient::getProfile(JsonCb cb)
{
    call("GET", QStringLiteral("/profile"), {}, {}, 1, std::move(cb));
}

void GmailClient::listLabels(JsonCb cb)
{
    call("GET", QStringLiteral("/labels"), {}, {}, 1, std::move(cb));
}

void GmailClient::getLabel(const QString &id, JsonCb cb)
{
    // Label ids are usually [A-Za-z0-9_]+ (INBOX, Label_12); percent-encode
    // anyway so odd ids never break the path.
    const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(id));
    call("GET", QStringLiteral("/labels/") + enc, {}, {}, 1, std::move(cb));
}

void GmailClient::createLabel(const QString &name, const QString &backgroundColor, JsonCb cb)
{
    QJsonObject o{{QStringLiteral("name"), name},
                  {QStringLiteral("labelListVisibility"), QStringLiteral("labelShow")},
                  {QStringLiteral("messageListVisibility"), QStringLiteral("show")}};
    if (!backgroundColor.isEmpty()) {
        o.insert(QStringLiteral("color"),
                 QJsonObject{{QStringLiteral("backgroundColor"), backgroundColor},
                             {QStringLiteral("textColor"), QStringLiteral("#ffffff")}});
    }
    call("POST", QStringLiteral("/labels"), {}, QJsonDocument(o).toJson(QJsonDocument::Compact), 5, std::move(cb));
}

void GmailClient::updateLabel(const QString &id, const QString &name, const QString &backgroundColor, JsonCb cb)
{
    QJsonObject o{{QStringLiteral("id"), id},
                  {QStringLiteral("name"), name},
                  {QStringLiteral("labelListVisibility"), QStringLiteral("labelShow")},
                  {QStringLiteral("messageListVisibility"), QStringLiteral("show")}};
    if (!backgroundColor.isEmpty()) {
        o.insert(QStringLiteral("color"),
                 QJsonObject{{QStringLiteral("backgroundColor"), backgroundColor},
                             {QStringLiteral("textColor"), QStringLiteral("#ffffff")}});
    }
    const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(id));
    call("PATCH", QStringLiteral("/labels/") + enc, {}, QJsonDocument(o).toJson(QJsonDocument::Compact), 5,
         std::move(cb));
}

void GmailClient::deleteLabel(const QString &id, JsonCb cb)
{
    const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(id));
    call("DELETE", QStringLiteral("/labels/") + enc, {}, {}, 5, std::move(cb));
}

void GmailClient::listMessages(const QString &labelId, int maxResults, const QString &pageToken, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("labelIds"), labelId);
    q.addQueryItem(QStringLiteral("maxResults"), QString::number(maxResults));
    if (!pageToken.isEmpty()) {
        q.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }
    call("GET", QStringLiteral("/messages"), q, {}, 5, std::move(cb));
}

void GmailClient::getMessageMetadata(const QString &id, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("metadata"));
    for (const char *h : {"From", "To", "Cc", "Reply-To", "Subject", "Date", "Message-ID", "References"}) {
        q.addQueryItem(QStringLiteral("metadataHeaders"), QString::fromLatin1(h));
    }
    call("GET", QStringLiteral("/messages/") + id, q, {}, 5, std::move(cb));
}

void GmailClient::getMessageFull(const QString &id, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("full"));
    call("GET", QStringLiteral("/messages/") + id, q, {}, 5, std::move(cb));
}

void GmailClient::modifyLabels(const QString &id, const QStringList &add, const QStringList &remove, JsonCb cb)
{
    QJsonObject o;
    o.insert(QStringLiteral("addLabelIds"), QJsonArray::fromStringList(add));
    o.insert(QStringLiteral("removeLabelIds"), QJsonArray::fromStringList(remove));
    call("POST", QStringLiteral("/messages/%1/modify").arg(id), {}, QJsonDocument(o).toJson(QJsonDocument::Compact),
         5, std::move(cb));
}

void GmailClient::trashMessage(const QString &id, JsonCb cb)
{
    call("POST", QStringLiteral("/messages/%1/trash").arg(id), {}, {}, 5, std::move(cb));
}

void GmailClient::untrashMessage(const QString &id, JsonCb cb)
{
    call("POST", QStringLiteral("/messages/%1/untrash").arg(id), {}, {}, 5, std::move(cb));
}

void GmailClient::listHistory(const QString &startHistoryId, const QString &pageToken, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("startHistoryId"), startHistoryId);
    for (const char *t : {"messageAdded", "messageDeleted", "labelAdded", "labelRemoved"}) {
        q.addQueryItem(QStringLiteral("historyTypes"), QString::fromLatin1(t));
    }
    q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("500"));
    if (!pageToken.isEmpty()) {
        q.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }
    call("GET", QStringLiteral("/history"), q, {}, 2, std::move(cb));
}

void GmailClient::listSendAs(JsonCb cb)
{
    call("GET", QStringLiteral("/settings/sendAs"), {}, {}, 1, std::move(cb));
}

void GmailClient::getAttachment(const QString &messageId, const QString &attachmentId, JsonCb cb)
{
    call("GET", QStringLiteral("/messages/%1/attachments/%2").arg(messageId, attachmentId), {}, {}, 5, std::move(cb));
}

void GmailClient::deleteDraft(const QString &draftId, JsonCb cb)
{
    call("DELETE", QStringLiteral("/drafts/") + draftId, {}, {}, 10, std::move(cb));
}

QUrl GmailClient::uploadBaseUrl() const
{
    QUrl u = m_base;
    u.setPath(QStringLiteral("/upload") + m_base.path());
    return u;
}

void GmailClient::call(QByteArray verb, QString path, QUrlQuery q, QByteArray body, int units, JsonCb cb)
{
    Request rq;
    rq.verb = std::move(verb);
    rq.path = std::move(path);
    rq.query = std::move(q);
    rq.body = std::move(body);
    rq.units = units;
    request(std::move(rq), [cb = std::move(cb)](const RawReply &r) { cb(r.json, r.err); });
}

void GmailClient::request(Request rq, RawCb cb)
{
    Call c;
    c.rq = std::move(rq);
    c.cb = std::move(cb);
    enqueue(std::move(c));
}

void GmailClient::enqueue(Call c)
{
    m_queue.append(std::move(c));
    pump();
}

void GmailClient::pump()
{
    const qint64 now = nowMs();
    m_tokens = std::min(m_capacity, m_tokens + double(now - m_lastRefill) * m_refillPerMs);
    m_lastRefill = now;
    while (!m_queue.isEmpty() && m_tokens >= m_queue.first().rq.units) {
        Call c = m_queue.takeFirst();
        m_tokens -= c.rq.units;
        send(std::move(c));
    }
    if (!m_queue.isEmpty() && !m_pumpTimer->isActive()) {
        const double need = m_queue.first().rq.units - m_tokens;
        const int wait = m_refillPerMs > 0 ? int(need / m_refillPerMs) + 1 : 1000;
        m_pumpTimer->start(std::max(1, wait));
    }
}

void GmailClient::retryLater(Call c, int delayMs)
{
    ++m_retries;
    ++c.attempt;
    QTimer::singleShot(delayMs, this, [this, c = std::move(c)]() mutable { enqueue(std::move(c)); });
}

void GmailClient::send(Call c)
{
    m_auth->accessToken([this, c = std::move(c)](const QString &token, const QString &authErr) mutable {
        if (token.isEmpty()) {
            RawReply rr;
            rr.err.isError = true;
            rr.err.httpStatus = 401;
            rr.err.reason = QStringLiteral("unauthenticated");
            rr.err.message = authErr;
            c.cb(rr);
            return;
        }
        const Request &rq = c.rq;
        QUrl url;
        if (rq.absoluteUrl.isValid()) {
            url = rq.absoluteUrl;
        } else {
            url = rq.upload ? uploadBaseUrl() : m_base;
            url.setPath(url.path() + rq.path);
        }
        if (!rq.query.isEmpty()) {
            url.setQuery(rq.query);
        }
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", "Bearer " + token.toLatin1());
        req.setRawHeader("Accept", "application/json");
        // 308 is "Resume Incomplete" for resumable uploads, never a redirect.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        req.setTransferTimeout(rq.timeoutMs);
        for (const auto &h : rq.headers) {
            req.setRawHeader(h.first, h.second);
        }
        QNetworkReply *r = nullptr;
        if (rq.verb == "GET") {
            r = m_nam->get(req);
        } else if (rq.verb == "DELETE") {
            r = m_nam->deleteResource(req);
        } else {
            req.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(rq.contentType));
            if (rq.verb == "PUT") {
                r = m_nam->put(req, rq.body);
            } else if (rq.verb == "PATCH") {
                r = m_nam->sendCustomRequest(req, "PATCH", rq.body);
            } else {
                r = m_nam->post(req, rq.body); // POST and anything else
            }
        }
        if (rq.progress) {
            connect(r, &QNetworkReply::uploadProgress, this, [p = rq.progress](qint64 s, qint64 t) { p(s, t); });
        }
        ++m_sent;
        connect(r, &QNetworkReply::finished, this, [this, r, c = std::move(c)]() mutable {
            r->deleteLater();
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = r->readAll();
            const QJsonObject json = QJsonDocument::fromJson(body).object();
            RawReply rr;
            rr.status = status;
            rr.body = body;
            rr.json = json;
            for (const auto &h : r->rawHeaderPairs()) {
                rr.headers.insert(h.first.toLower(), h.second);
            }
            if (r->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
                c.cb(rr);
                return;
            }
            const QJsonObject err = json.value(QStringLiteral("error")).toObject();
            QString reason = err.value(QStringLiteral("status")).toString();
            const QJsonArray errs = err.value(QStringLiteral("errors")).toArray();
            if (!errs.isEmpty()) {
                reason = errs.first().toObject().value(QStringLiteral("reason")).toString();
            }
            if (status == 401 && !c.reauthed) {
                m_auth->invalidateAccessToken();
                c.reauthed = true;
                enqueue(std::move(c));
                return;
            }
            const bool rateLimited403 = status == 403 && (reason == QLatin1String("rateLimitExceeded") ||
                                                          reason == QLatin1String("userRateLimitExceeded"));
            const bool transient = status == 429 || status == 500 || status == 502 || status == 503 ||
                                   status == 504 || rateLimited403 ||
                                   (status == 0 && r->error() != QNetworkReply::OperationCanceledError);
            if (transient && c.rq.retryTransient && c.attempt + 1 < m_maxAttempts) {
                int delay = std::min(64000, m_backoffBaseMs * (1 << c.attempt));
                delay += QRandomGenerator::global()->bounded(std::max(1, m_backoffBaseMs));
                bool ok = false;
                const int retryAfter = r->rawHeader("Retry-After").toInt(&ok);
                if (ok && retryAfter > 0) {
                    delay = std::max(delay, std::min(120, retryAfter) * 1000);
                }
                qCInfo(lcGmail) << "HTTP" << status << "for" << c.rq.path << "; retry" << (c.attempt + 1) << "in"
                                << delay << "ms";
                retryLater(std::move(c), delay);
                return;
            }
            ApiError &e = rr.err;
            e.isError = true;
            e.httpStatus = status;
            e.reason = reason;
            e.message = err.value(QStringLiteral("message")).toString();
            if (e.message.isEmpty()) {
                e.message = r->errorString();
            }
            if (status != 308) {
                qCWarning(lcGmail) << "Gmail API error" << status << reason << "for" << c.rq.path;
            }
            rr.json = {};
            c.cb(rr);
        });
    });
}

} // namespace zmail
