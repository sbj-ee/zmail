#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QWidget;

namespace zmail {
class ContactStore;
class ContactsSync;
struct ContactQuery;
}

namespace zmail::ui {

// Settings > Contacts. Google's contacts arrive raw (every address Gmail ever
// auto-saved); this is where they are put in order: categories down the left,
// the contacts of the chosen one in the middle, and on the right the chosen
// contact's categories, extra fields, comment and "hidden". All of that is
// the user's own and stays on this machine: nothing here is written to
// Google, and a resync leaves it alone. Edits are saved as they are made.
class ContactsWindow : public QDialog
{
    Q_OBJECT
public:
    explicit ContactsWindow(zmail::ContactStore *store, zmail::ContactsSync *sync = nullptr,
                            QWidget *parent = nullptr);

    // Prefer Settings→Sync Contacts path (enableContactsSync / re-auth) when set.
    void setSyncTrigger(std::function<void()> trigger) { m_syncTrigger = std::move(trigger); }

    // The left-hand choices that aren't categories.
    static constexpr const char *kAll = "\x01" "all";
    static constexpr const char *kUncategorized = "\x01" "none";
    static constexpr const char *kHidden = "\x01" "hidden";
    // Show kAll, kUncategorized, kHidden or a category by name.
    void showGroup(const QString &group);
    QString group() const { return m_group; }
    QStringList selectedIds() const;
    void selectContacts(const QStringList &ids);

    // What the buttons and menus do, without their prompts.
    void newCategory(const QString &name);
    void renameCategory(const QString &from, const QString &to);
    void deleteCategory(const QString &name);
    void setSelectedHidden(bool hidden);
    void addSelectedToCategory(const QString &name);
    void removeSelectedFromCategory(const QString &name);
    void addField(const QString &name);
    void removeCurrentField();

public slots:
    void refresh();
    void syncNow();

private:
    zmail::ContactQuery query() const;
    void refreshGroups();
    void refreshList();
    void showDetail();
    void saveFields();
    void saveCategories();
    QMenu *categorizeMenu(QWidget *parent);
    void showListMenu(const QPoint &pos);
    QString askCategoryName(const QString &title, const QString &current = {});

    zmail::ContactStore *m_store = nullptr;
    zmail::ContactsSync *m_sync = nullptr;
    std::function<void()> m_syncTrigger;
    QString m_group = QString::fromLatin1(kAll);
    QString m_detailId;     // the one contact on show in the detail pane
    bool m_loading = false; // filling widgets: their change signals save nothing

    QListWidget *m_groups = nullptr;
    QPushButton *m_renameCategory = nullptr;
    QPushButton *m_deleteCategory = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_hide = nullptr;
    QPushButton *m_categorize = nullptr;
    QLabel *m_status = nullptr;

    QWidget *m_detail = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_emails = nullptr;
    QLabel *m_source = nullptr;
    QCheckBox *m_hidden = nullptr;
    QListWidget *m_categoryChecks = nullptr;
    QTableWidget *m_fields = nullptr;
    QPushButton *m_removeField = nullptr;
    QPlainTextEdit *m_comment = nullptr;
};

} // namespace zmail::ui
