#pragma once

#include "core/ContactStore.h"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace zmail::ui {

// One contact, edited in a window of its own: name, nickname, addresses
// (the first is the primary one), categories, extra fields, comment and
// hidden. OK hands the result back (contact()); nothing is saved until then,
// and Cancel changes nothing. A contact synced from Google can be edited
// too: the changes are kept on this computer and Google's copy is left
// alone, and "Use Google's" drops the edits to its name and addresses.
class ContactEditDialog : public QDialog
{
    Q_OBJECT

public:
    ContactEditDialog(const zmail::Contact &contact, const QStringList &allCategories, QWidget *parent = nullptr);

    zmail::Contact contact() const;            // as edited
    bool revertRequested() const { return m_revert; } // "Use Google's" was pressed

    // What the buttons do, without their prompts.
    void addEmail(const QString &email);
    void removeCurrentEmail();
    void makeCurrentEmailPrimary();
    void addCategory(const QString &name); // listed and ticked
    void addField(const QString &name);
    void removeCurrentField();

    void accept() override; // refuses an address that isn't one

private:
    void refreshEmailMarks();

    zmail::Contact m_contact;
    bool m_revert = false;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_nickname = nullptr;
    QListWidget *m_emails = nullptr;
    QListWidget *m_categories = nullptr;
    QTableWidget *m_fields = nullptr;
    QPlainTextEdit *m_comment = nullptr;
    QCheckBox *m_hidden = nullptr;
    QLabel *m_problem = nullptr;
};

} // namespace zmail::ui
