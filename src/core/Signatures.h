#pragma once

#include <QList>
#include <QString>

class QSettings;

namespace zmail {

struct Signature
{
    QString name;
    QString html;   // rich version ("" = derive from text)
    QString text;   // plain version ("" = derive from html)

    QString plain() const;      // text, or html flattened (links "text <url>")
    // html run through sanitizeSignatureHtml(), or (a plain-only signature
    // from before styled signatures) the text escaped, newlines as <br>.
    QString richHtml() const;
};

// A signature's HTML as stored and as inserted into mail: the mail
// sanitizer (no scripts, handlers, forms, local files), re-serialized by
// Qt's rich-text engine so only formatting it understands survives (fonts,
// sizes, colours, bold/italic/underline, alignment, links), then with no
// resources at all: every <img>, background attribute and CSS url() is
// removed, and links keep only http(s): and mailto: targets. Returns the
// fragment that goes inside <body>, "" if nothing visible is left.
QString sanitizeSignatureHtml(const QString &html);
// text/plain version of signature HTML: lines kept, links as "text <url>".
QString signaturePlainFromHtml(const QString &html);
// Escaped HTML for a plain signature (newlines -> <br>).
QString signatureHtmlFromPlain(const QString &text);

// Signatures live in QSettings ("signatures/<n>/{name,text,html}", plus
// "signatures/default"). A store with no QSettings uses zmail's default
// settings file.
class SignatureStore
{
public:
    explicit SignatureStore(QSettings *settings = nullptr);
    ~SignatureStore();

    QList<Signature> all() const;
    void setAll(const QList<Signature> &sigs);
    QString defaultName() const;           // "" = no signature by default
    void setDefaultName(const QString &name);
    Signature find(const QString &name) const;

    // The text/plain delimiter line (RFC 3676 §4.3): "-- " then CRLF.
    static QString plainBlock(const Signature &s);

private:
    QSettings *m_settings;
    bool m_own = false;
};

} // namespace zmail
