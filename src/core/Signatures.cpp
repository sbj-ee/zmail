#include "Signatures.h"

#include "RichText.h"

#include <QSettings>

namespace zmail {

QString Signature::plain() const
{
    return !text.isEmpty() ? text : richtext::htmlToPlainText(html);
}

QString Signature::richHtml() const
{
    if (!html.isEmpty()) {
        return html;
    }
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

SignatureStore::SignatureStore(QSettings *settings)
    : m_settings(settings)
{
    if (!m_settings) {
        m_settings = new QSettings();
        m_own = true;
    }
}

SignatureStore::~SignatureStore()
{
    if (m_own) {
        delete m_settings;
    }
}

QList<Signature> SignatureStore::all() const
{
    QList<Signature> out;
    const int n = m_settings->beginReadArray(QStringLiteral("signatures"));
    for (int i = 0; i < n; ++i) {
        m_settings->setArrayIndex(i);
        Signature s;
        s.name = m_settings->value(QStringLiteral("name")).toString();
        s.text = m_settings->value(QStringLiteral("text")).toString();
        s.html = m_settings->value(QStringLiteral("html")).toString();
        if (!s.name.isEmpty()) {
            out << s;
        }
    }
    m_settings->endArray();
    return out;
}

void SignatureStore::setAll(const QList<Signature> &sigs)
{
    m_settings->remove(QStringLiteral("signatures"));
    m_settings->beginWriteArray(QStringLiteral("signatures"), int(sigs.size()));
    for (int i = 0; i < sigs.size(); ++i) {
        m_settings->setArrayIndex(i);
        m_settings->setValue(QStringLiteral("name"), sigs[i].name);
        m_settings->setValue(QStringLiteral("text"), sigs[i].text);
        m_settings->setValue(QStringLiteral("html"), sigs[i].html);
    }
    m_settings->endArray();
}

QString SignatureStore::defaultName() const
{
    return m_settings->value(QStringLiteral("signatureDefault")).toString();
}

void SignatureStore::setDefaultName(const QString &name)
{
    m_settings->setValue(QStringLiteral("signatureDefault"), name);
}

Signature SignatureStore::find(const QString &name) const
{
    for (const Signature &s : all()) {
        if (s.name == name) {
            return s;
        }
    }
    return {};
}

QString SignatureStore::plainBlock(const Signature &s)
{
    const QString p = s.plain();
    return p.isEmpty() ? QString() : QStringLiteral("-- \n") + p;
}

} // namespace zmail
