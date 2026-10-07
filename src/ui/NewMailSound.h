#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class QSoundEffect;

namespace zmail::ui {

// Plays the bundled CC0 chime (assets/sounds/new-mail.wav) when new INBOX
// mail arrives. A custom WAV from Settings > Sounds replaces it; a missing
// or unreadable custom file falls back to the built-in sound. Rule-based
// sounds and imported clips come later.
class NewMailSound : public QObject
{
    Q_OBJECT

public:
    // QSettings keys (Organisation/App from QApplication).
    static constexpr const char *kEnabledKey = "notify/sound";
    static constexpr const char *kFileKey = "notify/soundFile";

    explicit NewMailSound(QObject *parent = nullptr);

    // Play if enabled. One call per sync batch (SyncEngine::newMail).
    void play();
    // Always play the resolved source (Settings > Sounds > Test).
    void playPreview();
    // A filter's own sound: this .wav, or the usual sound if it has gone
    // missing. Muted like play() unless preview is set (the editor's Test).
    void playFile(const QString &path, bool preview = false);

    int playCount() const { return m_plays; }

    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }

    // Absolute path to a user WAV, or empty for the built-in chime.
    void setSoundFile(const QString &path);
    QString soundFile() const { return m_soundFile; }

    // file:// custom path when usable, else qrc:/sounds/new-mail.wav.
    QUrl resolvedSource() const;
    bool usingCustomFile() const;

    static QString resourceUrl() { return QStringLiteral("qrc:/sounds/new-mail.wav"); }
    static QString resourcePath() { return QStringLiteral(":/sounds/new-mail.wav"); }

    // True when path is a readable local .wav (case-insensitive extension).
    static bool isUsableSoundFile(const QString &path);

    void loadFromSettings();
    void saveToSettings() const;

private:
    void ensureEffect();
    void playResolved();

    QSoundEffect *m_effect = nullptr; // created lazily: no audio stack until needed
    int m_plays = 0;
    bool m_enabled = true;
    QString m_soundFile; // empty = built-in
};

} // namespace zmail::ui
