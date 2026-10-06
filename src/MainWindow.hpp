#pragma once

#include <QMainWindow>
#include <QPointer>

class QActionGroup;
class QLabel;
class QLineEdit;
class QKeySequence;
class QMenu;
class QModelIndex;
class QPoint;
class QSplitter;
class QTextBrowser;
class QTimer;
class QAction;
class QToolBar;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;
class UpdateChecker;
class ComposeWindow;

namespace zmail {
class MailSession;
}
namespace zmail::ui {
class MessageListModel;
class MessageFilterProxy;
class NewMailSound;
class SoundDialog;
class SafeHtmlView;
class MessageView;
class MessageWindow;
struct ViewMessage;
class ConnectDialog;
enum class ThemeMode;
class PrivacyDialog;
class StripesDialog;
} // namespace zmail::ui

// Eudora-inspired main window: mailbox tree on the left, a dense sortable
// message list with the preview pane below it, an icon+label toolbar under
// the in-window menu bar, and a status bar with sync state and counts.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Attach a Gmail session. Without one (tests, offline screenshots) the
    // window shows the built-in sample data.
    void setSession(zmail::MailSession *session);
    zmail::MailSession *session() const { return m_session; }
    bool isLive() const { return m_live; }
    // Opens the first-run / sign-in dialog (non-modal; returns it for tests).
    zmail::ui::ConnectDialog *showConnectDialog(const QString &notice = {});
    zmail::ui::NewMailSound *newMailSound() const { return m_sound; }
    // View > Play Sound for New Mail / toolbar speaker (Ctrl+Shift+M),
    // remembered in QSettings notify/sound.
    bool newMailSoundOn() const;
    void setNewMailSoundOn(bool on);

    // "zmail 0.1.0": built from project(VERSION), never hardcoded.
    static QString baseTitle();

    ComposeWindow *openCompose(bool sampleReply = false);
    // Live mode: Reply / Reply All / Forward the message in the preview pane
    // (sample mode opens the sample reply).
    // zmail::ReplyBuilder::Kind; id "" = the message in the preview pane.
    ComposeWindow *composeReply(int kind, const QString &id = {});
    QString shownMessageId() const { return m_shownId; }
    QList<ComposeWindow *> composers() const { return m_composers; }
    // How long "Moved to Trash. [Undo]" (and Edit > Undo Delete) stays offered.
    static constexpr int kUndoDeleteMs = 8000;
    void selectMailbox(const QString &key);
    // View > Hide Spam from Folders (QSettings mail/hideSpam, default on).
    bool hideSpam() const;
    void setHideSpam(bool hide);
    void showSpamFolder(); // View > Show Spam
    void setTheme(zmail::ui::ThemeMode mode);
    // Message-list row stripes, 0..100 (Settings > Row Stripes), remembered.
    void setStripeStrength(int strength);
    int stripeStrength() const { return m_stripes; }
    zmail::ui::StripesDialog *showStripesDialog(); // non-modal; returned for tests
    zmail::ui::PrivacyDialog *showPrivacyDialog(); // Settings > Privacy; caller shows it (tests)
    zmail::ui::SoundDialog *showSoundDialog(); // Settings > Sounds; caller shows it (tests)

    // Preview pane under the list (Eudora) or to its right; remembered.
    void setPreviewRight(bool right);
    bool previewRight() const;
    zmail::ui::MessageView *messageView() const { return m_view; }
    // Open the list row (proxy index) in its own window, as double-click does.
    zmail::ui::MessageWindow *openMessageWindow(const QModelIndex &proxyIndex);
    QList<zmail::ui::MessageWindow *> messageWindows() const;

public slots:
    void showAbout();
    void checkForUpdates();

protected:
    void closeEvent(QCloseEvent *ev) override;
    void changeEvent(QEvent *ev) override;

private:
    void buildMenus();
    void buildToolbar();
    void buildPanes();
    void buildStatusBar();
    void populateMailboxes();
    void showMessage(const QModelIndex &proxyIndex);
    void updateCounts();
    void refreshIcons();
    void sessionStateChanged();
    void attachSync();
    void reloadFromCache();
    // Before messages leave the list (Delete / move to Trash): if the current
    // row is one of them, select and preview the row below (above at the
    // bottom; nothing when the list empties).
    void selectPastRemoved(const QStringList &ids);
    void checkMail();
    void updateSyncLabel(const QString &status = {});
    QString labelForMailbox(const QString &key) const;
    void showLiveMessage(int row);
    void updateMessageActions();
    void showSignatures();
    zmail::ui::ViewMessage sampleViewMessage(int row) const;
    void trashMessage(QString id);
    void runFullTextSearch(const QString &text);
    void junkMessage(QString id);
    void notJunkMessage(QString id);
    void snoozeMessage(QString id, qint64 wakeMs);
    void unsnoozeMessage(QString id);
    void customSnooze();
    void checkSnoozeWakes();
    // Right-click menus, Enter to open, Mark Read/Unread, Undo Delete.
    void installListActions();
    QMenu *buildListMenu();
    QMenu *buildMailboxMenu(QTreeWidgetItem *item);
    void showListMenu(const QPoint &pos);
    void showMailboxMenu(const QPoint &pos);
    int unreadIn(const QString &key) const;
    void markAllRead(const QString &key);
    void setCurrentRead(bool read);
    void offerUndoDelete(const QString &id);
    void undoDelete();
    static QString withShortcut(const QString &tip, const QKeySequence &key);
    void saveSplitters();
    void restoreListSplitter();

    QSplitter *m_splitter = nullptr;
    QSplitter *m_listSplitter = nullptr;
    QTreeWidget *m_mailboxes = nullptr;
    QTreeView *m_list = nullptr;
    zmail::ui::MessageView *m_view = nullptr;
    QList<QPointer<zmail::ui::MessageWindow>> m_messageWindows;
    QLineEdit *m_search = nullptr;
    QString m_mailboxBeforeSearch; // restored when the search box is cleared
    QLabel *m_syncLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    zmail::ui::MessageListModel *m_model = nullptr;
    zmail::ui::MessageFilterProxy *m_proxy = nullptr;
    UpdateChecker *m_updates = nullptr;
    QActionGroup *m_themeGroup = nullptr;
    QList<ComposeWindow *> m_composers;
    zmail::MailSession *m_session = nullptr;
    zmail::ui::NewMailSound *m_sound = nullptr;
    QAction *m_soundAction = nullptr;
    QAction *soundAction();
    void addSoundButton(QToolBar *tb);
    void updateSoundAction();
    QPointer<zmail::ui::ConnectDialog> m_connect;
    QAction *m_signInAction = nullptr;
    QAction *m_signOutAction = nullptr;
    // Sign-in problems (timeout, browser error, revoked token) as a banner
    // across the top of the main window with Retry: it doesn't depend on
    // raising a dialog over the browser, which Wayland compositors refuse.
    void installSignInBanner();
    void showSignInBanner(const QString &reason);
    void hideSignInBanner();
    QToolBar *m_signInBanner = nullptr;
    QLabel *m_signInBannerText = nullptr;
    bool m_live = false;
    int m_stripes = 40;
    void applyStripes();
    QString m_lastSync;     // "8:45 AM"
    QString m_shownId;      // message currently in the preview
    int m_selectRowAfterReload = -1; // selectPastRemoved()'s row if its message vanished too
    QAction *m_undoDeleteAction = nullptr;
    QWidget *m_undoBar = nullptr;      // "Moved to Trash. [Undo]" in the status bar
    QLabel *m_undoLabel = nullptr;
    QTimer *m_undoTimer = nullptr;
    QString m_lastTrashed;             // id the Undo puts back
    QTimer *m_reloadTimer = nullptr;
    QAction *m_hideSpamAction = nullptr;
    QTimer *m_snoozeTimer = nullptr;
    QMenu *buildSnoozeMenu(QWidget *parent = nullptr);
};
