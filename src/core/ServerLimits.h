#pragma once

#include <QString>

namespace zmail {

// What a Gmail IMAP / SMTP refusal means for "Gmail (app password)" accounts,
// and how long to wait before trying again. Gmail answers, among others:
//   IMAP  NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)
//         NO [ALERT] Too many simultaneous connections. (Failure)
//         NO [ALERT] Account exceeded command or bandwidth limits. (Failure)
//         NO [THROTTLED] ...   * BYE [UNAVAILABLE] Temporary System Error
//   SMTP  535 5.7.8 Username and Password not accepted
//         454 4.7.0 Too many login attempts, please try again later
//         421 4.7.0 Try again later, closing connection / 421 4.3.0 Temporary System Problem
//         550 5.4.5 Daily user sending limit exceeded
namespace ServerLimits {

enum class Kind {
    None,               // nothing special: an ordinary error or a dropped connection
    AuthFailed,         // the app password was refused: never retry on our own (Google locks accounts)
    TooManyConnections, // Gmail allows 15 IMAP connections per account
    Bandwidth,          // command / bandwidth limits (IMAP download caps, throttling)
    Unavailable,        // temporary server trouble
    SendingLimit,       // daily sending limit: retrying today won't help
};

Kind classifyImap(const QString &text);
// code: the SMTP reply code; text: the reply text (enhanced status code included).
Kind classifySmtp(int code, const QString &text);

// Delay before attempt number `attempt` (0-based) after `kind`, with +-20%
// jitter: Unavailable / None 2 s doubling to 5 min; TooManyConnections 60 s
// doubling to 15 min; Bandwidth 10 min doubling to 60 min. Never for
// AuthFailed / SendingLimit (-1). scale shrinks it for tests.
qint64 backoffMs(Kind kind, int attempt, double scale = 1.0, bool jitter = true);

// One sentence for the status bar / error dialog. Never echoes a password.
QString explain(Kind kind, const QString &server);

} // namespace ServerLimits
} // namespace zmail
