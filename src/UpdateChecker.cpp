#include "UpdateChecker.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QTimer>
#include <QUrl>

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(5000);
    connect(m_timeout, &QTimer::timeout, this, &UpdateChecker::onTimeout);
}

QList<int> UpdateChecker::parseSemver(QStringView s)
{
    if (!s.isEmpty() && (s.front() == QLatin1Char('v') || s.front() == QLatin1Char('V'))) {
        s = s.mid(1);
    }
    QList<int> parts{0, 0, 0};
    const QList<QStringView> bits = s.split(QLatin1Char('.'));
    for (int i = 0; i < 3 && i < bits.size(); ++i) {
        int value = 0;
        for (const QChar c : bits[i]) {
            if (!c.isDigit()) {
                break;
            }
            value = value * 10 + c.digitValue();
        }
        parts[i] = value;
    }
    return parts;
}

bool UpdateChecker::isNewer(const QString &tag, const QString &current)
{
    const QList<int> a = parseSemver(tag);
    const QList<int> b = parseSemver(current);
    for (int i = 0; i < 3; ++i) {
        if (a[i] > b[i]) {
            return true;
        }
        if (a[i] < b[i]) {
            return false;
        }
    }
    return false;
}

void UpdateChecker::checkForUpdates(const QString &currentVersion, const QString &repo)
{
    if (m_checking) {
        return;
    }
    m_currentVersion = currentVersion;
    m_checking = true;
    m_timedOut = false;

    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repo));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("zmail-update-check"));
    req.setRawHeader("Accept", "application/vnd.github+json");

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        if (m_reply) {
            onFinished(m_reply);
        }
    });
    m_timeout->start();
}

void UpdateChecker::onTimeout()
{
    if (m_reply) {
        m_timedOut = true;
        m_reply->abort();
    }
}

QString UpdateChecker::failureReason(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if ((status == 403 || status == 429) && reply->rawHeader("x-ratelimit-remaining") == "0") {
        bool ok = false;
        const qint64 reset = reply->rawHeader("x-ratelimit-reset").toLongLong(&ok);
        if (ok && reset > 0) {
            return QStringLiteral("GitHub's rate limit for update checks was reached. Try again after %1.")
                .arg(QDateTime::fromSecsSinceEpoch(reset).toLocalTime().toString(QStringLiteral("HH:mm")));
        }
        return QStringLiteral("GitHub's rate limit for update checks was reached. Try again later.");
    }
    if (status == 404) {
        return QStringLiteral("No published zmail release was found on GitHub.");
    }
    if (status >= 400) {
        return QStringLiteral("GitHub answered with HTTP %1.").arg(status);
    }
    return QStringLiteral("Could not reach GitHub (%1).").arg(reply->errorString());
}

void UpdateChecker::onFinished(QNetworkReply *reply)
{
    m_timeout->stop();
    m_checking = false;
    m_reply = nullptr;

    reply->deleteLater();

    if (m_timedOut) {
        m_timedOut = false;
        emit checkFailed(QStringLiteral("GitHub did not answer within 5 seconds."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit checkFailed(failureReason(reply));
        return;
    }

    const QByteArray body = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        emit checkFailed(QStringLiteral("GitHub sent a reply zmail could not read."));
        return;
    }
    const QJsonObject obj = doc.object();
    if (!obj.contains(QStringLiteral("tag_name")) || !obj.value(QStringLiteral("tag_name")).isString()) {
        emit checkFailed(QStringLiteral("No published zmail release was found on GitHub."));
        return;
    }

    const QString tag = obj.value(QStringLiteral("tag_name")).toString();
    if (!isNewer(tag, m_currentVersion)) {
        emit upToDate();
        return;
    }

    const QString htmlUrl = obj.value(QStringLiteral("html_url")).toString();
    emit updateAvailable(tag, htmlUrl);
}
