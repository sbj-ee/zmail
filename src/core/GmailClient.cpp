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
    enqueue({"GET", QStringLiteral("/profile"), {}, {}, 1, 0, false, std::move(cb)});
}

void GmailClient::listLabels(JsonCb cb)
{
    enqueue({"GET", QStringLiteral("/labels"), {}, {}, 1, 0, false, std::move(cb)});
}

void GmailClient::listMessages(const QString &labelId, int maxResults, const QString &pageToken, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("labelIds"), labelId);
    q.addQueryItem(QStringLiteral("maxResults"), QString::number(maxResults));
    if (!pageToken.isEmpty()) {
        q.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }
    enqueue({"GET", QStringLiteral("/messages"), q, {}, 5, 0, false, std::move(cb)});
}

void GmailClient::getMessageMetadata(const QString &id, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("metadata"));
    for (const char *h : {"From", "To", "Cc", "Subject", "Date"}) {
        q.addQueryItem(QStringLiteral("metadataHeaders"), QString::fromLatin1(h));
    }
    enqueue({"GET", QStringLiteral("/messages/") + id, q, {}, 5, 0, false, std::move(cb)});
}

void GmailClient::getMessageFull(const QString &id, JsonCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("full"));
    enqueue({"GET", QStringLiteral("/messages/") + id, q, {}, 5, 0, false, std::move(cb)});
}

void GmailClient::modifyLabels(const QString &id, const QStringList &add, const QStringList &remove, JsonCb cb)
{
    QJsonObject o;
    o.insert(QStringLiteral("addLabelIds"), QJsonArray::fromStringList(add));
    o.insert(QStringLiteral("removeLabelIds"), QJsonArray::fromStringList(remove));
    enqueue({"POST", QStringLiteral("/messages/%1/modify").arg(id), {}, QJsonDocument(o).toJson(QJsonDocument::Compact),
             5, 0, false, std::move(cb)});
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
    enqueue({"GET", QStringLiteral("/history"), q, {}, 2, 0, false, std::move(cb)});
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
    while (!m_queue.isEmpty() && m_tokens >= m_queue.first().units) {
        Call c = m_queue.takeFirst();
        m_tokens -= c.units;
        send(std::move(c));
    }
    if (!m_queue.isEmpty() && !m_pumpTimer->isActive()) {
        const double need = m_queue.first().units - m_tokens;
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
            ApiError e;
            e.isError = true;
            e.httpStatus = 401;
            e.reason = QStringLiteral("unauthenticated");
            e.message = authErr;
            c.cb({}, e);
            return;
        }
        QUrl url = m_base;
        url.setPath(m_base.path() + c.path);
        url.setQuery(c.query);
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", "Bearer " + token.toLatin1());
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(60000);
        QNetworkReply *r = nullptr;
        if (c.verb == "POST") {
            req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            r = m_nam->post(req, c.body);
        } else {
            r = m_nam->get(req);
        }
        ++m_sent;
        connect(r, &QNetworkReply::finished, this, [this, r, c = std::move(c)]() mutable {
            r->deleteLater();
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = r->readAll();
            const QJsonObject json = QJsonDocument::fromJson(body).object();
            if (r->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
                c.cb(json, ApiError::none());
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
            if (transient && c.attempt + 1 < m_maxAttempts) {
                int delay = std::min(64000, m_backoffBaseMs * (1 << c.attempt));
                delay += QRandomGenerator::global()->bounded(std::max(1, m_backoffBaseMs));
                bool ok = false;
                const int retryAfter = r->rawHeader("Retry-After").toInt(&ok);
                if (ok && retryAfter > 0) {
                    delay = std::max(delay, std::min(120, retryAfter) * 1000);
                }
                qCInfo(lcGmail) << "HTTP" << status << "for" << c.path << "; retry" << (c.attempt + 1) << "in"
                                << delay << "ms";
                retryLater(std::move(c), delay);
                return;
            }
            ApiError e;
            e.isError = true;
            e.httpStatus = status;
            e.reason = reason;
            e.message = err.value(QStringLiteral("message")).toString();
            if (e.message.isEmpty()) {
                e.message = r->errorString();
            }
            qCWarning(lcGmail) << "Gmail API error" << status << reason << "for" << c.path;
            c.cb({}, e);
        });
    });
}

} // namespace zmail
