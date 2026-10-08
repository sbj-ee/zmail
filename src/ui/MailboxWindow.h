#pragma once

#include <QMainWindow>
#include <QStringList>

class QAbstractItemModel;
class QLabel;
class QTreeView;

namespace zmail::ui {

class MessageFilterProxy;
class MessageListModel;
class MessageView;

// A mailbox in a window of its own, as in Eudora: the same live message
// list as the main window, for one mailbox or folder, with a preview pane
// below it, so two can be seen side by side. It shows what the main window's model holds, filtered, and
// asks the main window to open or delete (it has no mail logic of its own).
class MailboxWindow : public QMainWindow
{
    Q_OBJECT

public:
    MailboxWindow(MessageListModel *model, const QString &mailboxKey, const QString &title, QWidget *parent = nullptr);

    QString mailbox() const;
    QTreeView *list() const { return m_list; }
    MessageFilterProxy *proxy() const { return m_proxy; }
    QStringList selectedIds() const;
    MessageView *preview() const { return m_preview; } // the message selected in this window

signals:
    void openRequested(const QString &messageId);      // double-click, Enter
    void deleteRequested(const QStringList &messageIds); // Delete
    void moreRequested();                              // scrolled to the bottom
    void showRequested(const QString &messageId);      // one message selected: fill preview()
    // Right-click: the main window builds the menu (it has the commands).
    void menuRequested(const QStringList &messageIds, const QPoint &globalPos);

private:
    void updateCount();

    MessageListModel *m_model = nullptr;
    MessageFilterProxy *m_proxy = nullptr;
    QTreeView *m_list = nullptr;
    QLabel *m_count = nullptr;
    MessageView *m_preview = nullptr;
};

} // namespace zmail::ui
