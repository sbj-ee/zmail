#include "ClientConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zmail {

QString ClientConfig::defaultPath()
{
    const QByteArray env = qgetenv("ZMAIL_OAUTH_CLIENT");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }
    return QDir::homePath() + QStringLiteral("/.config/zmail/oauth-client.json");
}

ClientConfig::LoadResult ClientConfig::parse(const QByteArray &json)
{
    LoadResult r;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        r.status = LoadStatus::BadJson;
        r.message = QStringLiteral("The client file isn't valid JSON.");
        return r;
    }
    const QJsonObject root = doc.object();
    if (!root.contains(QStringLiteral("installed"))) {
        r.status = LoadStatus::NotDesktopClient;
        r.message = root.contains(QStringLiteral("web"))
                        ? QStringLiteral("This is a \"Web application\" client. zmail needs a \"Desktop app\" client.")
                        : QStringLiteral("This doesn't look like a Google OAuth client file (no \"installed\" section).");
        return r;
    }
    const QJsonObject in = root.value(QStringLiteral("installed")).toObject();
    r.config.clientId = in.value(QStringLiteral("client_id")).toString();
    r.config.clientSecret = in.value(QStringLiteral("client_secret")).toString();
    if (!r.config.isValid()) {
        r.status = LoadStatus::BadJson;
        r.message = QStringLiteral("The client file is missing client_id or client_secret.");
        return r;
    }
    // Only ever talk to Google's real endpoints, whatever the file says.
    const QUrl auth(in.value(QStringLiteral("auth_uri")).toString());
    const QUrl token(in.value(QStringLiteral("token_uri")).toString());
    auto ok = [](const QUrl &u, const QString &host) {
        return u.isEmpty() || (u.scheme() == QLatin1String("https") && u.host() == host);
    };
    if (!ok(auth, QStringLiteral("accounts.google.com")) || !ok(token, QStringLiteral("oauth2.googleapis.com"))) {
        r.status = LoadStatus::BadEndpoint;
        r.message = QStringLiteral("The client file points at non-Google sign-in endpoints; refusing to use it.");
        return r;
    }
    r.status = LoadStatus::Ok;
    return r;
}

ClientConfig::LoadResult ClientConfig::load(const QString &path)
{
    LoadResult r;
    QFile f(path);
    if (!f.exists()) {
        r.status = LoadStatus::Missing;
        r.message = QStringLiteral("No OAuth client file at %1.").arg(path);
        return r;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        r.status = LoadStatus::Unreadable;
        r.message = QStringLiteral("Can't read %1: %2").arg(path, f.errorString());
        return r;
    }
    r = parse(f.readAll());
    struct stat st{};
    if (::stat(QFile::encodeName(path).constData(), &st) == 0) {
        r.permissionsTooOpen = (st.st_mode & (S_IRWXG | S_IRWXO)) != 0;
    }
    if (r.status == LoadStatus::Ok && r.permissionsTooOpen) {
        r.message = QStringLiteral("%1 is readable by other users. It should be mode 0600 (chmod 600).").arg(path);
    }
    return r;
}

bool ClientConfig::tightenPermissions(const QString &path)
{
    return ::chmod(QFile::encodeName(path).constData(), S_IRUSR | S_IWUSR) == 0;
}

QString ClientConfig::install(const QString &source, const QString &dest)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Can't read %1: %2").arg(source, in.errorString());
    }
    const QByteArray data = in.readAll();
    const LoadResult parsed = parse(data);
    if (parsed.status != LoadStatus::Ok) {
        return parsed.message;
    }
    const QString dir = QFileInfo(dest).absolutePath();
    if (!QDir().mkpath(dir)) {
        return QStringLiteral("Can't create %1.").arg(dir);
    }
    ::chmod(QFile::encodeName(dir).constData(), S_IRWXU);

    // Create the temp file 0600 from the start, then rename into place.
    const QByteArray tmp = QFile::encodeName(dest + QStringLiteral(".tmp"));
    ::unlink(tmp.constData());
    const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return QStringLiteral("Can't write %1: %2").arg(dest, QString::fromLocal8Bit(std::strerror(errno)));
    }
    qsizetype off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + off, size_t(data.size() - off));
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.constData());
            return QStringLiteral("Write failed for %1.").arg(dest);
        }
        off += n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.constData(), QFile::encodeName(dest).constData()) != 0) {
        ::unlink(tmp.constData());
        return QStringLiteral("Can't move the client file into place at %1.").arg(dest);
    }
    tightenPermissions(dest);
    return {};
}

} // namespace zmail
