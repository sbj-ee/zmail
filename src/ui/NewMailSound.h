#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QSoundEffect;

namespace zmail::ui {

// Plays the bundled CC0 chime (assets/sounds/new-mail.wav) when new INBOX
// mail arrives. Another built-in sound or a custom WAV from Settings >
// Sounds replaces it; a missing or unreadable file falls back to the chime.
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

    // The other sounds that come with zmail (assets/sounds/, all CC0), by
    // name and resource path (":/sounds/bell.wav"). One is chosen the way a
    // custom file is: its path is the sound file.
    struct BuiltIn {
        QString name;
        QString path;
    };
    static QList<BuiltIn> builtIns();
    static bool isBuiltIn(const QString &path) { return path.startsWith(QLatin1String(":/sounds/")); }
    // What plays `path`: qrc: for a built-in, file: for a file on disk.
    static QUrl urlFor(const QString &path);

    // Absolute path to a user WAV, a built-in's resource path, or empty for
    // the chime.
    void setSoundFile(const QString &path);
    QString soundFile() const { return m_soundFile; }

    // file:// custom path when usable, else qrc:/sounds/new-mail.wav.
    QUrl resolvedSource() const;
    bool usingCustomFile() const;

    static QString resourceUrl() { return QStringLiteral("qrc:/sounds/new-mail.wav"); }
    static QString resourcePath() { return QStringLiteral(":/sounds/new-mail.wav"); }

    // The volume sounds play at: 0 in a test or headless run, so running the
    // tests (which simulate mail arriving) doesn't chime on the developer's
    // speakers; normal otherwise.
    static float outputVolume();

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

// The bundled CC0 "swoosh" (assets/sounds/sent.wav) as a message leaves:
// Send, and each message File > Send Queued Messages gets out. On unless
// Settings > Sounds turns it off (QSettings "notify/sentSound").
class SentSound : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *kEnabledKey = "notify/sentSound";

    explicit SentSound(QObject *parent = nullptr);

    void play();        // if enabled
    void playPreview(); // always (Settings > Sounds > Test)
    int playCount() const { return m_plays; }

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    static QString resourceUrl() { return QStringLiteral("qrc:/sounds/sent.wav"); }
    static QString resourcePath() { return QStringLiteral(":/sounds/sent.wav"); }

private:
    QSoundEffect *m_effect = nullptr;
    int m_plays = 0;
    bool m_enabled = true;
};

} // namespace zmail::ui
