#pragma once

#include "ContactStore.h"

#include <QObject>
#include <QJsonObject>
#include <functional>

class QNetworkAccessManager;

namespace zmail {

class AuthManager;
struct ApiError;

// Read-only Google People API client (connections + otherContacts).
class PeopleClient : public QObject
{
    Q_OBJECT
public:
    using JsonCb = std::function<void(const QJsonObject &json, const ApiError &err)>;

    PeopleClient(AuthManager *auth, QNetworkAccessManager *nam, QObject *parent = nullptr);
    void setApiBase(const QString &base) { m_apiBase = base; } // tests

    // people.connections.list — pass empty syncToken for a full sync.
    void listConnections(const QString &syncToken, const QString &pageToken, JsonCb cb);
    void listOtherContacts(const QString &pageToken, JsonCb cb);

    static Contact contactFromPerson(const QJsonObject &person, const QString &source);

signals:
    void syncError(const QString &message);

private:
    void get(const QString &path, const QList<QPair<QString, QString>> &query, JsonCb cb);

    AuthManager *m_auth = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QString m_apiBase = QStringLiteral("https://people.googleapis.com/v1");
};

// Drives ContactStore from PeopleClient; handles syncToken + 410 full resync.
class ContactsSync : public QObject
{
    Q_OBJECT
public:
    ContactsSync(PeopleClient *api, ContactStore *store, QObject *parent = nullptr);
    void sync(); // connections (incremental) then otherContacts (full page)

signals:
    void finished(bool ok, const QString &message);
    void progress(const QString &status);

private:
    void syncConnections(const QString &token, bool isRetry);
    void syncConnectionsPage(const QString &token, const QString &pageToken, bool isRetry);
    void syncOtherContacts(const QString &pageToken);
    void applyConnectionsPage(const QJsonObject &json, bool incremental);

    PeopleClient *m_api = nullptr;
    ContactStore *m_store = nullptr;
    int m_pending = 0;
};

} // namespace zmail
