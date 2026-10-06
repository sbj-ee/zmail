#include "SearchQuery.h"

#include <QRegularExpression>

namespace zmail {

namespace {
bool isOperatorToken(const QString &tok)
{
    static const QRegularExpression re(
        QStringLiteral("^-?(from|to|subject|label|has|is):"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(tok).hasMatch();
}

QStringList tokenize(const QString &text)
{
    QStringList tokens;
    QString cur;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            cur += c;
            continue;
        }
        if (!quoted && c.isSpace()) {
            if (!cur.isEmpty()) {
                tokens << cur;
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.isEmpty()) {
        tokens << cur;
    }
    return tokens;
}

QString escapeFtsToken(QString t)
{
    // Strip wrapping quotes for phrase handling.
    const bool phrase = t.size() >= 2 && t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"'));
    if (phrase) {
        t = t.mid(1, t.size() - 2);
    }
    t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    // FTS5 specials: drop bare * from user input inside the token.
    t.remove(QLatin1Char('*'));
    if (t.isEmpty()) {
        return {};
    }
    if (phrase || t.contains(QLatin1Char(' ')) || t.contains(QLatin1Char(':'))) {
        return QLatin1Char('"') + t + QLatin1Char('"');
    }
    // Prefix match so "fish" hits "fishing".
    return t + QLatin1Char('*');
}
} // namespace

QStringList SearchQuery::freeTextTokens(const QString &userText)
{
    QStringList out;
    for (const QString &tok : tokenize(userText.trimmed())) {
        if (isOperatorToken(tok)) {
            continue;
        }
        // Keep quoted phrases and bare words; drop lone punctuation.
        QString t = tok;
        if (t.startsWith(QLatin1Char('-')) && !t.startsWith(QLatin1String("-\""))) {
            // Negated free text: keep the leading '-' for FTS NOT — skip for LIKE simplicity.
            t = t.mid(1);
            if (t.isEmpty()) {
                continue;
            }
        }
        out << tok; // preserve original (including leading '-') for FTS
    }
    return out;
}

QString SearchQuery::toFts5(const QString &userText)
{
    QStringList parts;
    for (QString tok : freeTextTokens(userText)) {
        bool negate = false;
        if (tok.startsWith(QLatin1Char('-'))) {
            negate = true;
            tok = tok.mid(1);
        }
        const QString esc = escapeFtsToken(tok);
        if (esc.isEmpty()) {
            continue;
        }
        parts << (negate ? QStringLiteral("NOT %1").arg(esc) : esc);
    }
    return parts.join(QLatin1Char(' '));
}

QString SearchQuery::toLikeNeedle(const QString &userText)
{
    // First non-operator bare word (unquoted), stripped of FTS syntax.
    for (QString tok : freeTextTokens(userText)) {
        if (tok.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (tok.size() >= 2 && tok.startsWith(QLatin1Char('"')) && tok.endsWith(QLatin1Char('"'))) {
            tok = tok.mid(1, tok.size() - 2);
        }
        tok.remove(QLatin1Char('%'));
        tok.remove(QLatin1Char('_'));
        tok.remove(QLatin1Char('*'));
        if (!tok.isEmpty()) {
            return tok;
        }
    }
    // Fallback: whole string without operators.
    QString s = userText.trimmed();
    s.remove(QLatin1Char('%'));
    s.remove(QLatin1Char('_'));
    return s;
}

} // namespace zmail
