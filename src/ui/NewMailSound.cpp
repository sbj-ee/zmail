#include "NewMailSound.h"

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QSoundEffect>
#include <QUrl>

namespace zmail::ui {

NewMailSound::NewMailSound(QObject *parent)
    : QObject(parent)
{
    loadFromSettings();
}

bool NewMailSound::isUsableSoundFile(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo fi(path);
    if (!fi.isFile() || !fi.isReadable() || fi.size() <= 0) {
        return false;
    }
    return fi.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0;
}

QList<NewMailSound::BuiltIn> NewMailSound::builtIns()
{
    return {{tr("Bell"), QStringLiteral(":/sounds/bell.wav")},
            {tr("Marimba"), QStringLiteral(":/sounds/marimba.wav")},
            {tr("Glass"), QStringLiteral(":/sounds/glass.wav")},
            {tr("Knock"), QStringLiteral(":/sounds/knock.wav")},
            {tr("Water Drop"), QStringLiteral(":/sounds/drop.wav")},
            {tr("Harp"), QStringLiteral(":/sounds/harp.wav")}};
}

QUrl NewMailSound::urlFor(const QString &path)
{
    return isBuiltIn(path) ? QUrl(QStringLiteral("qrc") + path) : QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
}

bool NewMailSound::usingCustomFile() const
{
    return isUsableSoundFile(m_soundFile);
}

QUrl NewMailSound::resolvedSource() const
{
    if (usingCustomFile()) {
        return urlFor(m_soundFile);
    }
    return QUrl(resourceUrl());
}

void NewMailSound::setSoundFile(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (m_soundFile == trimmed) {
        return;
    }
    m_soundFile = trimmed;
    if (m_effect) {
        m_effect->setSource(resolvedSource());
    }
}

void NewMailSound::loadFromSettings()
{
    QSettings st;
    m_enabled = st.value(QLatin1String(kEnabledKey), true).toBool();
    m_soundFile = st.value(QLatin1String(kFileKey)).toString().trimmed();
    if (m_effect) {
        m_effect->setSource(resolvedSource());
    }
}

void NewMailSound::saveToSettings() const
{
    QSettings st;
    st.setValue(QLatin1String(kEnabledKey), m_enabled);
    if (m_soundFile.isEmpty()) {
        st.remove(QLatin1String(kFileKey));
    } else {
        st.setValue(QLatin1String(kFileKey), m_soundFile);
    }
}

float NewMailSound::outputVolume()
{
    const bool silent = QStandardPaths::isTestModeEnabled() || QGuiApplication::platformName() == QLatin1String("offscreen") ||
                        qEnvironmentVariableIsSet("ZMAIL_SILENT");
    return silent ? 0.0f : 0.8f;
}

void NewMailSound::ensureEffect()
{
    if (!m_effect) {
        m_effect = new QSoundEffect(this);
    }
    m_effect->setVolume(outputVolume());
    // Always re-resolve: the custom file may have appeared or vanished.
    m_effect->setSource(resolvedSource());
}

void NewMailSound::playResolved()
{
    ++m_plays;
    ensureEffect();
    m_effect->play();
}

void NewMailSound::play()
{
    if (!m_enabled) {
        return;
    }
    playResolved();
}

void NewMailSound::playPreview()
{
    playResolved();
}

void NewMailSound::playFile(const QString &path, bool preview)
{
    if (!m_enabled && !preview) {
        return;
    }
    if (!isUsableSoundFile(path)) {
        playResolved();
        return;
    }
    ++m_plays;
    ensureEffect();
    m_effect->setSource(urlFor(path));
    m_effect->play();
}

SentSound::SentSound(QObject *parent)
    : QObject(parent)
    , m_enabled(QSettings().value(QLatin1String(kEnabledKey), true).toBool())
{
}

void SentSound::setEnabled(bool on)
{
    m_enabled = on;
    QSettings().setValue(QLatin1String(kEnabledKey), on);
}

void SentSound::play()
{
    if (m_enabled) {
        playPreview();
    }
}

void SentSound::playPreview()
{
    ++m_plays;
    if (!m_effect) {
        m_effect = new QSoundEffect(this);
        m_effect->setSource(QUrl(resourceUrl()));
    }
    m_effect->setVolume(NewMailSound::outputVolume());
    m_effect->play();
}

} // namespace zmail::ui
