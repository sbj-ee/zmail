#include "AppPasswordAccount.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>

namespace zmail {

namespace {
const QString kKind = QStringLiteral("account/kind");
const QString kEmail = QStringLiteral("imap/email");
const QString kImapHost = QStringLiteral("imap/imapHost");
const QString kImapPort = QStringLiteral("imap/imapPort");
const QString kSmtpHost = QStringLiteral("imap/smtpHost");
const QString kSmtpPort = QStringLiteral("imap/smtpPort");
const QString kDisplayName = QStringLiteral("imap/displayName");
QString tr(const char *s) { return QCoreApplication::translate("zmail::AppPasswordAccount", s); }
} // namespace

bool AppPasswordAccount::isValid() const
{
    return looksLikeEmail(email) && !imapHost.isEmpty() && !smtpHost.isEmpty() && imapPort > 0 && imapPort < 65536
        && smtpPort > 0 && smtpPort < 65536;
}

AppPasswordAccount AppPasswordAccount::load(const QSettings &s)
{
    AppPasswordAccount a;
    a.email = s.value(kEmail).toString();
    a.imapHost = s.value(kImapHost, a.imapHost).toString();
    a.imapPort = s.value(kImapPort, a.imapPort).toInt();
    a.smtpHost = s.value(kSmtpHost, a.smtpHost).toString();
    a.smtpPort = s.value(kSmtpPort, a.smtpPort).toInt();
    a.displayName = s.value(kDisplayName).toString();
    return a;
}

void AppPasswordAccount::save(QSettings &s) const
{
    s.setValue(kKind, QStringLiteral("gmail-app-password"));
    s.setValue(kEmail, email);
    s.setValue(kImapHost, imapHost);
    s.setValue(kImapPort, imapPort);
    s.setValue(kSmtpHost, smtpHost);
    s.setValue(kSmtpPort, smtpPort);
    if (displayName.trimmed().isEmpty()) {
        s.remove(kDisplayName);
    } else {
        s.setValue(kDisplayName, displayName.trimmed());
    }
}

void AppPasswordAccount::clear(QSettings &s)
{
    s.remove(QStringLiteral("imap"));
    s.remove(kKind);
}

AccountKind AppPasswordAccount::savedKind(const QSettings &s)
{
    return s.value(kKind).toString() == QLatin1String("gmail-app-password") ? AccountKind::GmailAppPassword
                                                                           : AccountKind::GmailOAuth;
}

QString AppPasswordAccount::appPasswordKey(const QString &email)
{
    return QStringLiteral("app-password:") + email.trimmed().toLower();
}

QString AppPasswordAccount::normalizeAppPassword(const QString &input, QString *error)
{
    QString out;
    out.reserve(16);
    for (const QChar c : input) {
        if (!c.isSpace()) {
            out.append(c.toLower());
        }
    }
    bool ok = out.size() == 16;
    for (const QChar c : out) {
        ok = ok && c >= QLatin1Char('a') && c <= QLatin1Char('z');
    }
    if (!ok) {
        if (error) {
            *error = out.isEmpty() ? tr("Enter the app password Google showed you.")
                                   : tr("That isn't an app password. Google's are 16 letters, shown as four groups "
                                        "of four. Don't use your normal Google password here.");
        }
        return {};
    }
    if (error) {
        error->clear();
    }
    return out;
}

bool AppPasswordAccount::looksLikeEmail(const QString &email)
{
    static const QRegularExpression re(QStringLiteral(R"(^[^@\s"]+@[^@\s"]+\.[^@\s"]+$)"));
    return re.match(email.trimmed()).hasMatch();
}

} // namespace zmail
