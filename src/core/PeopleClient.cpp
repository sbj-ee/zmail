#include "PeopleClient.h"
#include "AuthManager.h"
#include "GmailClient.h"
#include "Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace zmail {

PeopleClient::PeopleClient(AuthManager *auth, QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_auth(auth)
    , m_nam(nam)
{
}

Contact PeopleClient::contactFromPerson(const QJsonObject &person, const QString &source)
{
    Contact c;
    c.googleResource = person.value(QStringLiteral("resourceName")).toString();
    c.id = c.googleResource.isEmpty()
               ? QStringLiteral("people-") + QString::number(qHash(person))
               : c.googleResource;
    c.source = source;
    c.etag = person.value(QStringLiteral("etag")).toString();
    const QJsonArray names = person.value(QStringLiteral("names")).toArray();
    if (!names.isEmpty()) {
        c.displayName = names.at(0).toObject().value(QStringLiteral("displayName")).toString();
    }
    const QJsonArray emails = person.value(QStringLiteral("emailAddresses")).toArray();
    for (const QJsonValue &v : emails) {
        const QJsonObject o = v.toObject();
        ContactEmail e;
        e.email = o.value(QStringLiteral("value")).toString().trimmed().toLower();
        e.primary = o.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("primary")).toBool();
        if (!e.email.isEmpty()) {
            c.emails.append(e);
        }
    }
    if (c.displayName.isEmpty() && !c.emails.isEmpty()) {
        c.displayName = c.emails.first().email;
    }
    return c;
}

void PeopleClient::get(const QString &path, const QList<QPair<QString, QString>> &query, JsonCb cb)
{
    m_auth->accessToken([this, path, query, cb = std::move(cb)](const QString &token, const QString &authErr) {
        if (token.isEmpty()) {
            ApiError err;
            err.isError = true;
            err.httpStatus = 401;
            err.message = authErr.isEmpty() ? tr("Not signed in.") : authErr;
            cb({}, err);
            return;
        }
        QUrl url(m_apiBase + path);
        QUrlQuery q;
        for (const auto &p : query) {
            q.addQueryItem(p.first, p.second);
        }
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        QNetworkReply *r = m_nam->get(req);
        connect(r, &QNetworkReply::finished, this, [r, cb]() {
            r->deleteLater();
            ApiError err;
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = r->readAll();
            QJsonObject json = QJsonDocument::fromJson(body).object();
            if (r->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                err.isError = true;
                err.httpStatus = status;
                err.message = json.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
                if (err.message.isEmpty()) {
                    err.message = r->errorString();
                }
                err.reason = json.value(QStringLiteral("error")).toObject().value(QStringLiteral("status")).toString();
            }
            cb(json, err);
        });
    });
}

void PeopleClient::listConnections(const QString &syncToken, const QString &pageToken, JsonCb cb)
{
    QList<QPair<QString, QString>> q{
        {QStringLiteral("personFields"), QStringLiteral("names,emailAddresses,metadata")},
        {QStringLiteral("pageSize"), QStringLiteral("200")},
        {QStringLiteral("requestSyncToken"), QStringLiteral("true")},
    };
    if (!syncToken.isEmpty()) {
        q.append({QStringLiteral("syncToken"), syncToken});
    }
    if (!pageToken.isEmpty()) {
        q.append({QStringLiteral("pageToken"), pageToken});
    }
    get(QStringLiteral("/people/me/connections"), q, std::move(cb));
}

void PeopleClient::listOtherContacts(const QString &pageToken, JsonCb cb)
{
    QList<QPair<QString, QString>> q{
        {QStringLiteral("readMask"), QStringLiteral("names,emailAddresses")},
        {QStringLiteral("pageSize"), QStringLiteral("200")},
    };
    if (!pageToken.isEmpty()) {
        q.append({QStringLiteral("pageToken"), pageToken});
    }
    get(QStringLiteral("/otherContacts"), q, std::move(cb));
}

ContactsSync::ContactsSync(PeopleClient *api, ContactStore *store, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_store(store)
{
}

void ContactsSync::sync()
{
    if (!m_api || !m_store || !m_store->isOpen()) {
        emit finished(false, tr("Contacts store is not open."));
        return;
    }
    emit progress(tr("Syncing Google Contacts…"));
    syncConnections(m_store->meta(QStringLiteral("connectionsSyncToken")), false);
}

void ContactsSync::syncConnections(const QString &token, bool isRetry)
{
    syncConnectionsPage(token, {}, isRetry);
}

void ContactsSync::syncConnectionsPage(const QString &token, const QString &pageToken, bool isRetry)
{
    m_api->listConnections(token, pageToken, [this, token, isRetry](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            if ((err.httpStatus == 410 || err.httpStatus == 400) && !token.isEmpty() && !isRetry) {
                emit progress(tr("Contacts sync token expired; doing a full resync…"));
                m_store->setMeta(QStringLiteral("connectionsSyncToken"), {});
                m_store->clearGoogleContacts();
                syncConnectionsPage({}, {}, true);
                return;
            }
            QString msg = err.message;
            if (err.httpStatus == 403) {
                msg = tr("Contacts access denied or the People API is not enabled. "
                         "Enable People API in Google Cloud and grant Contacts permission.");
            }
            emit finished(false, msg);
            return;
        }
        applyConnectionsPage(json, !token.isEmpty());
        const QString next = json.value(QStringLiteral("nextPageToken")).toString();
        const QString newSync = json.value(QStringLiteral("nextSyncToken")).toString();
        if (!newSync.isEmpty()) {
            m_store->setMeta(QStringLiteral("connectionsSyncToken"), newSync);
        }
        if (!next.isEmpty()) {
            syncConnectionsPage(token.isEmpty() ? QString() : token, next, isRetry);
            return;
        }
        syncOtherContacts({});
    });
}

void ContactsSync::applyConnectionsPage(const QJsonObject &json, bool incremental)
{
    Q_UNUSED(incremental);
    const QJsonArray people = json.value(QStringLiteral("connections")).toArray();
    for (const QJsonValue &v : people) {
        Contact c = PeopleClient::contactFromPerson(v.toObject(), QStringLiteral("google"));
        if (c.emails.isEmpty()) {
            continue;
        }
        m_store->upsertContact(c);
    }
}

void ContactsSync::syncOtherContacts(const QString &pageToken)
{
    m_api->listOtherContacts(pageToken, [this](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            // Other contacts is best-effort; don't fail the whole sync on 403 for that scope alone.
            if (err.httpStatus == 403) {
                emit finished(true, tr("Google Contacts synced (Other contacts permission missing)."));
                return;
            }
            emit finished(false, err.message);
            return;
        }
        const QJsonArray people = json.value(QStringLiteral("otherContacts")).toArray();
        for (const QJsonValue &v : people) {
            Contact c = PeopleClient::contactFromPerson(v.toObject(), QStringLiteral("other"));
            if (c.emails.isEmpty()) {
                continue;
            }
            m_store->upsertContact(c);
        }
        const QString next = json.value(QStringLiteral("nextPageToken")).toString();
        if (!next.isEmpty()) {
            syncOtherContacts(next);
            return;
        }
        emit finished(true, tr("Contacts synced."));
    });
}

} // namespace zmail
