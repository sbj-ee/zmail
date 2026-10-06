#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;
class QLabel;

namespace zmail {
class ContactStore;
class ContactsSync;
}

namespace zmail::ui {

class ContactsWindow : public QDialog
{
    Q_OBJECT
public:
    explicit ContactsWindow(zmail::ContactStore *store, zmail::ContactsSync *sync = nullptr,
                            QWidget *parent = nullptr);

public slots:
    void refresh();
    void syncNow();

private:
    zmail::ContactStore *m_store = nullptr;
    zmail::ContactsSync *m_sync = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace zmail::ui
