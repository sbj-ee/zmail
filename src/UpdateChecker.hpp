#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Background "Check for Updates" against GitHub releases/latest.
// Ported from sbj-ee/zwriter (same behavior as zedit): non-blocking, ~5s timeout, soft-fail, no-op
// while a check is already in flight. Semver compare tolerates leading 'v'
// and missing parts (= 0).
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    bool isChecking() const { return m_checking; }

public slots:
    // Safe to re-trigger while idle; ignored while in flight.
    void checkForUpdates(const QString &currentVersion,
                         const QString &repo = QStringLiteral("sbj-ee/zmail"));

signals:
    // Emitted only when a newer release is found (never on "up to date" /
    // network failure / no releases).
    void updateAvailable(const QString &tagName, const QString &htmlUrl);
    // Optional: emitted when the check finishes with no newer release
    // (manual Check for Updates can show a brief status). Silent otherwise.
    void upToDate();
    // The check could not be completed; reason is a short user-facing text.
    void checkFailed(const QString &reason);

private slots:
    void onFinished(QNetworkReply *reply);
    void onTimeout();

public:
    // Exposed for unit tests.
    static QList<int> parseSemver(QStringView s);
    static bool isNewer(const QString &tag, const QString &current);
    // The page "Open the release page?" may open: the reply's html_url only
    // if it is under https://github.com/sbj-ee/zmail/, otherwise the
    // releases page. The reply is untrusted (a proxy, a compromised account
    // or a bug could put any URL there).
    static QString releasePageUrl(const QString &htmlUrl);

private:
    static QString failureReason(QNetworkReply *reply);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_timeout = nullptr;
    QString m_currentVersion;
    bool m_checking = false;
    bool m_timedOut = false;
};
