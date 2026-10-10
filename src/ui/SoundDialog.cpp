#include "SoundDialog.h"

#include "NewMailSound.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QPushButton>
#include <QSoundEffect>
#include <QVBoxLayout>

namespace zmail::ui {

SoundDialog::SoundDialog(NewMailSound *sound, QWidget *parent)
    : QDialog(parent)
    , m_sound(sound)
{
    setObjectName(QStringLiteral("soundDialog"));
    setWindowTitle(tr("Sounds"));
    auto *lay = new QVBoxLayout(this);

    auto *group = new QGroupBox(tr("New mail"), this);
    group->setObjectName(QStringLiteral("newMailSoundGroup"));
    auto *gl = new QVBoxLayout(group);

    m_enable = new QCheckBox(tr("&Play sound for new mail"), group);
    m_enable->setObjectName(QStringLiteral("playSoundCheck"));
    gl->addWidget(m_enable);

    auto *note = new QLabel(tr("When new INBOX mail arrives while zmail is running. "
                               "One chime per sync batch; sent mail and the first sync "
                               "at startup are silent."),
                            group);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    gl->addWidget(note);

    // The sounds that come with zmail; Browse is for one of your own.
    auto *pickRow = new QHBoxLayout;
    auto *pickLabel = new QLabel(tr("&Sound:"), group);
    m_builtIn = new QComboBox(group);
    m_builtIn->setObjectName(QStringLiteral("builtInSoundCombo"));
    m_builtIn->setPlaceholderText(tr("A file of your own"));
    m_builtIn->addItem(tr("Chime (the usual one)"), QString());
    for (const NewMailSound::BuiltIn &b : NewMailSound::builtIns()) {
        m_builtIn->addItem(b.name, b.path);
    }
    pickLabel->setBuddy(m_builtIn);
    pickRow->addWidget(pickLabel);
    pickRow->addWidget(m_builtIn, 1);
    gl->addLayout(pickRow);
    connect(m_builtIn, &QComboBox::activated, this, [this](int index) {
        setSoundFile(m_builtIn->itemData(index).toString());
        test(); // hear it as it is picked
    });

    auto *pathRow = new QHBoxLayout;
    auto *pathLabel = new QLabel(tr("Sound &file:"), group);
    pathLabel->setObjectName(QStringLiteral("soundFileLabel"));
    m_path = new QLineEdit(group);
    m_path->setObjectName(QStringLiteral("soundFileEdit"));
    m_path->setReadOnly(true);
    m_path->setMinimumWidth(280);
    pathLabel->setBuddy(m_path);
    pathRow->addWidget(pathLabel);
    pathRow->addWidget(m_path, 1);
    m_browse = new QPushButton(tr("&Browse\u2026"), group);
    m_browse->setObjectName(QStringLiteral("browseSoundButton"));
    m_browse->setAutoDefault(false);
    pathRow->addWidget(m_browse);
    m_default = new QPushButton(tr("&Default"), group);
    m_default->setObjectName(QStringLiteral("defaultSoundButton"));
    m_default->setAutoDefault(false);
    pathRow->addWidget(m_default);
    gl->addLayout(pathRow);

    auto *testRow = new QHBoxLayout;
    testRow->addStretch(1);
    m_test = new QPushButton(tr("&Test"), group);
    m_test->setObjectName(QStringLiteral("testSoundButton"));
    m_test->setAutoDefault(false);
    testRow->addWidget(m_test);
    gl->addLayout(testRow);

    m_error = new QLabel(group);
    m_error->setObjectName(QStringLiteral("soundErrorLabel"));
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color:#b3261e"));
    m_error->hide();
    gl->addWidget(m_error);

    lay->addWidget(group);

    m_sentGroup = new QGroupBox(tr("Sent mail"), this);
    m_sentGroup->setObjectName(QStringLiteral("sentSoundGroup"));
    auto *sl = new QHBoxLayout(m_sentGroup);
    m_sentEnable = new QCheckBox(tr("Play a s&woosh when a message is sent"), m_sentGroup);
    m_sentEnable->setObjectName(QStringLiteral("sentSoundCheck"));
    sl->addWidget(m_sentEnable, 1);
    auto *sentTest = new QPushButton(tr("T&est"), m_sentGroup);
    sentTest->setObjectName(QStringLiteral("testSentSoundButton"));
    sentTest->setAutoDefault(false);
    sl->addWidget(sentTest);
    connect(sentTest, &QPushButton::clicked, this, [this]() {
        if (m_sent) {
            m_sent->playPreview();
        }
    });
    m_sentGroup->hide();
    lay->addWidget(m_sentGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_browse, &QPushButton::clicked, this, &SoundDialog::browse);
    connect(m_default, &QPushButton::clicked, this, &SoundDialog::resetDefault);
    connect(m_test, &QPushButton::clicked, this, &SoundDialog::test);

    if (m_sound) {
        setSoundEnabled(m_sound->isEnabled());
        setSoundFile(m_sound->soundFile());
    } else {
        setSoundEnabled(true);
        setSoundFile({});
    }
}

void SoundDialog::setSentSound(SentSound *sent)
{
    m_sent = sent;
    m_sentGroup->setVisible(sent != nullptr);
    m_sentEnable->setChecked(sent && sent->isEnabled());
}

bool SoundDialog::soundEnabled() const
{
    return m_enable->isChecked();
}

void SoundDialog::setSoundEnabled(bool on)
{
    m_enable->setChecked(on);
}

QString SoundDialog::soundFile() const
{
    return m_file;
}

void SoundDialog::setSoundFile(const QString &path)
{
    m_file = path.trimmed();
    m_error->hide();
    updatePathLabel();
}

void SoundDialog::updatePathLabel()
{
    m_builtIn->setCurrentIndex(m_builtIn->findData(m_file)); // none: a file of your own
    if (m_file.isEmpty()) {
        m_path->setText(tr("Default (built-in chime)"));
        m_path->setToolTip(NewMailSound::resourceUrl());
    } else if (NewMailSound::isBuiltIn(m_file)) {
        m_path->setText(tr("Built-in (%1)").arg(m_builtIn->currentIndex() >= 0 ? m_builtIn->currentText()
                                                                              : QFileInfo(m_file).fileName()));
        m_path->setToolTip(NewMailSound::urlFor(m_file).toString());
    } else {
        m_path->setText(m_file);
        m_path->setToolTip(m_file);
    }
}

void SoundDialog::browse()
{
    const QString start = NewMailSound::isBuiltIn(m_file) ? QString() : m_file;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose new-mail sound"), start,
        tr("Wave audio (*.wav);;All files (*)"));
    if (!path.isEmpty()) {
        setSoundFile(path);
    }
}

void SoundDialog::resetDefault()
{
    setSoundFile({});
}

QUrl SoundDialog::previewSource() const
{
    if (m_file.isEmpty()) {
        return QUrl(NewMailSound::resourceUrl());
    }
    return NewMailSound::urlFor(m_file);
}

void SoundDialog::test()
{
    // Play what the field shows, before it's saved, through the dialog's own
    // effect (NewMailSound keeps the saved choice until OK).
    m_error->hide();
    if (!m_file.isEmpty() && !NewMailSound::isUsableSoundFile(m_file)) {
        showError(tr("Can't play this file. Choose a readable .wav file."));
        return;
    }
    if (!m_preview) {
        m_preview = new QSoundEffect(this);
        m_preview->setObjectName(QStringLiteral("soundPreviewEffect"));
        m_preview->setVolume(NewMailSound::outputVolume()); // silent in a test run
        connect(m_preview, &QSoundEffect::statusChanged, this, &SoundDialog::playPreviewIfReady);
    }
    const QUrl src = previewSource();
    if (m_preview->source() != src || m_preview->status() == QSoundEffect::Error) {
        m_preview->setSource(QUrl()); // force a reload (a failed file may have been replaced)
        m_preview->setSource(src);    // loads asynchronously; plays on Ready
    }
    m_previewQueued = true;
    playPreviewIfReady();
}

void SoundDialog::playPreviewIfReady()
{
    if (!m_preview || !m_previewQueued) {
        return;
    }
    switch (m_preview->status()) {
    case QSoundEffect::Ready:
        m_previewQueued = false;
        ++m_previewPlays;
        m_preview->play();
        break;
    case QSoundEffect::Error:
        m_previewQueued = false;
        showError(QMediaDevices::audioOutputs().isEmpty()
                      ? tr("No audio output device to play the sound on.")
                      : tr("Couldn't load this sound file. Choose a PCM .wav file."));
        break;
    default:
        break; // Null / Loading: statusChanged brings us back
    }
}

void SoundDialog::showError(const QString &message)
{
    m_error->setText(message);
    m_error->show();
}

void SoundDialog::save() const
{
    if (m_sent) {
        m_sent->setEnabled(m_sentEnable->isChecked());
    }
    if (!m_sound) {
        return;
    }
    m_sound->setEnabled(m_enable->isChecked());
    m_sound->setSoundFile(m_file);
    m_sound->saveToSettings();
}

} // namespace zmail::ui
