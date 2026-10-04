#pragma once

#include <QObject>

class QSoundEffect;

namespace zmail::ui {

// Plays the bundled CC0 chime (assets/sounds/new-mail.wav) when new INBOX
// mail arrives. Rule-based sounds and imported clips come later.
class NewMailSound : public QObject
{
    Q_OBJECT

public:
    explicit NewMailSound(QObject *parent = nullptr);
    void play();
    int playCount() const { return m_plays; }
    void setEnabled(bool on) { m_enabled = on; }
    bool isEnabled() const { return m_enabled; }
    static QString resourceUrl() { return QStringLiteral("qrc:/sounds/new-mail.wav"); }

private:
    QSoundEffect *m_effect = nullptr; // created lazily: no audio stack needed until mail arrives
    int m_plays = 0;
    bool m_enabled = true;
};

} // namespace zmail::ui
