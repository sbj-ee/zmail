#include "TokenStore.h"

#include <QTimer>
#include <qt6keychain/keychain.h>

namespace zmail {

KeychainTokenStore::KeychainTokenStore(QObject *parent)
    : TokenStore(parent)
{
}

void KeychainTokenStore::read(const QString &key, ReadCb cb)
{
    auto *job = new QKeychain::ReadPasswordJob(QString::fromLatin1(kService), this);
    job->setAutoDelete(true);
    job->setKey(key);
    connect(job, &QKeychain::Job::finished, this, [cb](QKeychain::Job *j) {
        auto *rj = static_cast<QKeychain::ReadPasswordJob *>(j);
        if (rj->error() == QKeychain::NoError) {
            cb(true, rj->textData(), {});
        } else if (rj->error() == QKeychain::EntryNotFound) {
            cb(false, {}, {});
        } else {
            cb(false, {}, QStringLiteral("Keyring error: %1").arg(rj->errorString()));
        }
    });
    job->start();
}

void KeychainTokenStore::write(const QString &key, const QString &value, DoneCb cb)
{
    auto *job = new QKeychain::WritePasswordJob(QString::fromLatin1(kService), this);
    job->setAutoDelete(true);
    job->setKey(key);
    job->setTextData(value);
    connect(job, &QKeychain::Job::finished, this, [cb](QKeychain::Job *j) {
        cb(j->error() == QKeychain::NoError,
           j->error() == QKeychain::NoError ? QString()
                                            : QStringLiteral("Keyring error: %1").arg(j->errorString()));
    });
    job->start();
}

void KeychainTokenStore::remove(const QString &key, DoneCb cb)
{
    auto *job = new QKeychain::DeletePasswordJob(QString::fromLatin1(kService), this);
    job->setAutoDelete(true);
    job->setKey(key);
    connect(job, &QKeychain::Job::finished, this, [cb](QKeychain::Job *j) {
        const bool ok = j->error() == QKeychain::NoError || j->error() == QKeychain::EntryNotFound;
        cb(ok, ok ? QString() : QStringLiteral("Keyring error: %1").arg(j->errorString()));
    });
    job->start();
}

void MemoryTokenStore::read(const QString &key, ReadCb cb)
{
    const bool has = values.contains(key);
    const QString v = values.value(key);
    QTimer::singleShot(0, this, [cb, has, v]() { cb(has, v, {}); });
}

void MemoryTokenStore::write(const QString &key, const QString &value, DoneCb cb)
{
    values.insert(key, value);
    QTimer::singleShot(0, this, [cb]() { cb(true, {}); });
}

void MemoryTokenStore::remove(const QString &key, DoneCb cb)
{
    values.remove(key);
    QTimer::singleShot(0, this, [cb]() { cb(true, {}); });
}

} // namespace zmail
