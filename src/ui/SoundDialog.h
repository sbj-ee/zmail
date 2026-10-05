#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QPushButton;

namespace zmail::ui {

class NewMailSound;

// Settings > Sounds. Enable the new-mail chime, pick a custom .wav (or
// reset to the built-in default), and Test the current choice. Nothing is
// written until OK (save()).
class SoundDialog : public QDialog
{
    Q_OBJECT

public:
    // sound is the live NewMailSound used by MainWindow (Test plays through it).
    explicit SoundDialog(NewMailSound *sound, QWidget *parent = nullptr);

    bool soundEnabled() const;
    void setSoundEnabled(bool on);
    QString soundFile() const;
    void setSoundFile(const QString &path);

    void save() const; // writes QSettings and updates NewMailSound (done on OK)

private:
    void browse();
    void resetDefault();
    void test();
    void updatePathLabel();

    NewMailSound *m_sound = nullptr;
    QCheckBox *m_enable = nullptr;
    QLineEdit *m_path = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_default = nullptr;
    QPushButton *m_test = nullptr;
    QString m_file; // empty = built-in; mirrored into the path field
};

} // namespace zmail::ui
