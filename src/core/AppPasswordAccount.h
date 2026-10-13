#pragma once

#include <QString>

class QSettings;

namespace zmail {

// How an account talks to Gmail.
//   GmailOAuth        the Gmail API with the user's own OAuth client (the original path)
//   GmailAppPassword  IMAP (imap.gmail.com:993) + SMTP (smtp.gmail.com:465), logging in
//                     with a 16-letter Google app password. No OAuth client, so none of
//                     the "Testing" 7-day token expiry.
enum class AccountKind { GmailOAuth, GmailAppPassword };

// The non-secret half of an app-password account, kept in QSettings
// ("imap/..."). The app password itself only ever lives in the system keyring
// (TokenStore, key appPasswordKey(email)); there is no plaintext fallback.
struct AppPasswordAccount
{
    QString email;
    QString imapHost = QStringLiteral("imap.gmail.com");
    int imapPort = 993;
    QString smtpHost = QStringLiteral("smtp.gmail.com");
    int smtpPort = 465;

    bool isValid() const;

    // QSettings round trip. save() never writes a password.
    static AppPasswordAccount load(const QSettings &s);
    void save(QSettings &s) const;
    static void clear(QSettings &s);
    // Which kind the app should start with ("account/kind"); OAuth when unset.
    static AccountKind savedKind(const QSettings &s);

    // Keyring entry for this address's app password.
    static QString appPasswordKey(const QString &email);

    // Google shows app passwords as "abcd efgh ijkl mnop". Strip whitespace,
    // lower-case, and require exactly 16 ASCII letters. Returns an empty
    // string (and sets *error) for anything else, which also stops someone
    // pasting their real Google password here. Never echoes the input.
    static QString normalizeAppPassword(const QString &input, QString *error = nullptr);
    static bool looksLikeEmail(const QString &email);
};

} // namespace zmail
