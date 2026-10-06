#pragma once

#include <QDialog>
#include <QUrl>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSoundEffect;

namespace zmail::ui {

class NewMailSound;

// Settings > Sounds. Enable the new-mail chime, pick a custom .wav (or
// reset to the built-in default), and Test the current choice. Nothing is
// written until OK (save()).
class SoundDialog : public QDialog
{
    Q_OBJECT

public:
    // sound is the live NewMailSound used by MainWindow; OK saves into it.
    // Test previews through the dialog's own effect and leaves it alone.
    explicit SoundDialog(NewMailSound *sound, QWidget *parent = nullptr);

    bool soundEnabled() const;
    void setSoundEnabled(bool on);
    QString soundFile() const;
    void setSoundFile(const QString &path);

    void save() const; // writes QSettings and updates NewMailSound (done on OK)

    // What Test plays: the file in the path field (not yet saved), or the
    // built-in chime when it says Default.
    QUrl previewSource() const;
    QSoundEffect *previewEffect() const { return m_preview; } // null until the first Test (tests)
    int previewPlays() const { return m_previewPlays; }        // tests

private:
    void browse();
    void resetDefault();
    void test();
    void playPreviewIfReady();
    void showError(const QString &message);
    void updatePathLabel();

    NewMailSound *m_sound = nullptr;
    QCheckBox *m_enable = nullptr;
    QLineEdit *m_path = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_default = nullptr;
    QPushButton *m_test = nullptr;
    QLabel *m_error = nullptr; // inline "can't play this file"
    // Test's own effect, separate from NewMailSound's: previewing never
    // touches the saved choice, and play() waits for Ready.
    QSoundEffect *m_preview = nullptr;
    bool m_previewQueued = false;
    int m_previewPlays = 0;
    QString m_file; // empty = built-in; mirrored into the path field
};

} // namespace zmail::ui
