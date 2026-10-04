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

    QString plain() const;      // text, or html flattened
    QString richHtml() const;   // html, or text escaped
};

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
