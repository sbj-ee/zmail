#include "SoundDialog.h"

#include "NewMailSound.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    lay->addWidget(group);

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
    updatePathLabel();
}

void SoundDialog::updatePathLabel()
{
    if (m_file.isEmpty()) {
        m_path->setText(tr("Default (built-in chime)"));
        m_path->setToolTip(NewMailSound::resourceUrl());
    } else {
        m_path->setText(m_file);
        m_path->setToolTip(m_file);
    }
}

void SoundDialog::browse()
{
    const QString start = m_file.isEmpty() ? QString() : m_file;
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

void SoundDialog::test()
{
    if (!m_sound) {
        return;
    }
    // Preview the dialog's current choice without committing yet.
    const QString prev = m_sound->soundFile();
    m_sound->setSoundFile(m_file);
    m_sound->playPreview();
    m_sound->setSoundFile(prev);
}

void SoundDialog::save() const
{
    if (!m_sound) {
        return;
    }
    m_sound->setEnabled(m_enable->isChecked());
    m_sound->setSoundFile(m_file);
    m_sound->saveToSettings();
}

} // namespace zmail::ui
