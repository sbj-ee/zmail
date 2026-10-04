#pragma once

#include <QMainWindow>

class QActionGroup;
class QLabel;
class QLineEdit;
class QModelIndex;
class QSplitter;
class QTextBrowser;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class UpdateChecker;
class ComposeWindow;

namespace zmail::ui {
class MessageListModel;
class MessageFilterProxy;
enum class ThemeMode;
} // namespace zmail::ui

// Eudora-inspired main window: mailbox tree on the left, a dense sortable
// message list with the preview pane below it, an icon+label toolbar under
// the in-window menu bar, and a status bar with sync state and counts.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // "zmail 0.1.0": built from project(VERSION), never hardcoded.
    static QString baseTitle();

    ComposeWindow *openCompose(bool sampleReply = false);
    void selectMailbox(const QString &key);
    void setTheme(zmail::ui::ThemeMode mode);

public slots:
    void showAbout();
    void checkForUpdates();

private:
    void buildMenus();
    void buildToolbar();
    void buildPanes();
    void buildStatusBar();
    void populateMailboxes();
    void showMessage(const QModelIndex &proxyIndex);
    void updateCounts();
    void refreshIcons();

    QSplitter *m_splitter = nullptr;
    QSplitter *m_listSplitter = nullptr;
    QTreeWidget *m_mailboxes = nullptr;
    QTreeView *m_list = nullptr;
    QTextBrowser *m_preview = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_syncLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    zmail::ui::MessageListModel *m_model = nullptr;
    zmail::ui::MessageFilterProxy *m_proxy = nullptr;
    UpdateChecker *m_updates = nullptr;
    QActionGroup *m_themeGroup = nullptr;
    QList<ComposeWindow *> m_composers;
};
