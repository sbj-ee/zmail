#pragma once

#include "RemoteImages.h"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

namespace zmail::ui {

// Settings > Privacy. "Remote images": Always load (default) / Ask / Never,
// "block tracking pixels", and the Ask-mode sender allow list (add, remove,
// clear). Nothing is saved until OK (save()).
class PrivacyDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PrivacyDialog(QWidget *parent = nullptr);

    RemoteImageMode mode() const;
    void setMode(RemoteImageMode m);
    QStringList senders() const;
    QListWidget *senderList() const { return m_list; }

    void save() const; // writes everything to QSettings (done on OK)

private:
    void addSender();
    void updateButtons();

    QRadioButton *m_always = nullptr;
    QRadioButton *m_ask = nullptr;
    QRadioButton *m_never = nullptr;
    QCheckBox *m_trackers = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_add = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_clear = nullptr;
};

} // namespace zmail::ui
