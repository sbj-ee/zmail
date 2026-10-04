#include "NewMailSound.h"

#include <QSoundEffect>
#include <QUrl>

namespace zmail::ui {

NewMailSound::NewMailSound(QObject *parent)
    : QObject(parent)
{
}

void NewMailSound::play()
{
    if (!m_enabled) {
        return;
    }
    ++m_plays;
    if (!m_effect) {
        m_effect = new QSoundEffect(this);
        m_effect->setSource(QUrl(resourceUrl()));
        m_effect->setVolume(0.8f);
    }
    m_effect->play();
}

} // namespace zmail::ui
