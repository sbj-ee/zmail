#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <functional>

namespace zmail {

// Secret storage for refresh tokens. The real implementation is the OS
// keyring via QtKeychain (Secret Service / libsecret, KWallet). There is no
// plaintext fallback: if the keyring is unavailable, sign-in can't persist.
class TokenStore : public QObject
{
    Q_OBJECT

public:
    using ReadCb = std::function<void(bool ok, const QString &value, const QString &error)>;
    using DoneCb = std::function<void(bool ok, const QString &error)>;

    using QObject::QObject;
    virtual void read(const QString &key, ReadCb cb) = 0;
    virtual void write(const QString &key, const QString &value, DoneCb cb) = 0;
    virtual void remove(const QString &key, DoneCb cb) = 0;
};

class KeychainTokenStore : public TokenStore
{
    Q_OBJECT

public:
    explicit KeychainTokenStore(QObject *parent = nullptr);
    void read(const QString &key, ReadCb cb) override;
    void write(const QString &key, const QString &value, DoneCb cb) override;
    void remove(const QString &key, DoneCb cb) override;

    static constexpr const char *kService = "zmail";
};

// In-memory store for tests and offline demos. Never used by the real app.
class MemoryTokenStore : public TokenStore
{
    Q_OBJECT

public:
    using TokenStore::TokenStore;
    void read(const QString &key, ReadCb cb) override;
    void write(const QString &key, const QString &value, DoneCb cb) override;
    void remove(const QString &key, DoneCb cb) override;
    QHash<QString, QString> values;
};

} // namespace zmail
