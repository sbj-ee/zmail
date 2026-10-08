#pragma once

#include "core/Stationery.h"

#include <QDialog>

class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace zmail::ui {

// Settings > Stationery: the saved templates on the left, the chosen one's
// name, recipients, subject and text on the right. OK keeps the list
// (items()), Cancel leaves the old one.
class StationeryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StationeryDialog(const QList<zmail::Stationery> &items, QWidget *parent = nullptr);

    QList<zmail::Stationery> items() const { return m_items; }
    int current() const { return m_current; }
    void setCurrent(int index);
    int add(const zmail::Stationery &item = {}); // appended and selected; its index
    void removeCurrent();

private:
    void refreshList();
    void load();
    void commit();

    QList<zmail::Stationery> m_items;
    int m_current = -1;
    bool m_loading = false;
    QListWidget *m_list = nullptr;
    QPushButton *m_delete = nullptr;
    QWidget *m_editor = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_to = nullptr;
    QLineEdit *m_cc = nullptr;
    QLineEdit *m_subject = nullptr;
    QPlainTextEdit *m_body = nullptr;
};

} // namespace zmail::ui
