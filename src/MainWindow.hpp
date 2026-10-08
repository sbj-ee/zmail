#pragma once

#include "core/Rules.h"

#include <QMainWindow>
#include <QPointer>
#include <QList>
#include <QStringList>

class QActionGroup;
class QLabel;
class QPrinter;
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
class ThemeEditorDialog;
class SafeHtmlView;
class MessageView;
class MessageWindow;
struct ViewMessage;
class ConnectDialog;
enum class ThemeMode;
class PrivacyDialog;
class StripesDialog;
class ListDialog;
class RulesDialog;
class ContactsWindow;
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
    // A mailto: link clicked in a message: a new compose window, prefilled.
    ComposeWindow *composeMailto(const QUrl &url);
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
    // Message-list text size in pt (0: the application font's) and extra row
    // height in px (Settings > Message List), remembered.
    void setListAppearance(int fontSize, int rowSpacing);
    int listFontSize() const { return m_listFontSize; }
    int listRowSpacing() const { return m_listRowSpacing; }
    zmail::ui::ListDialog *showListDialog(); // non-modal; returned for tests
    // Flag the selected messages in a colour (ui/Flags.h); empty clears.
    void setFlagOnSelected(const QString &color);
    void emptyTrash(bool confirm = true); // right-click Trash > Empty Trash
    // File > Save Attachments: the shown message's attachments into a
    // folder (asked for when empty). savedAttachments() is what the last
    // call wrote, in order, once it has finished.
    void saveAttachments(const QString &folder = {});
    QStringList savedAttachments() const { return m_savedAttachments; }
    // File > Print: the shown message, header and body. With no printer, the
    // print dialog chooses one. False if there was nothing to print or the
    // dialog was cancelled.
    bool printMessage(QPrinter *printer = nullptr);
    void copySelection(); // Edit > Copy
    // Eudora's Mailbox and Transfer menus, built from the sidebar: go to a
    // mailbox, or move the selected messages to one ("INBOX", a folder's
    // Gmail label id, or "TRASH" to delete them).
    void openMailbox(const QString &key);
    void transferSelected(const QString &target);
    // File > Send Queued Messages: everything waiting in Out, oldest first.
    // One that Gmail refuses stays queued, with the reason.
    void sendQueued();
    int queuedCount() const;
    void setViewAsPlainText(bool on); // View > View as Plain Text, remembered
    // Filters (Settings > Filters): the rules in force, the editor, and
    // Message > Filter Messages for the selection. A rule passed to the
    // editor is added to the list there (Make Filter).
    const zmail::Rules &rules() const { return m_rules; }
    void setRules(const QList<zmail::Rule> &rules); // saved, and the list recoloured
    zmail::ui::RulesDialog *showRulesDialog(const zmail::Rule *add = nullptr); // non-modal; returned for tests
    void filterSelected();
    // What a batch of newly arrived messages gets: each one's matching rule
    // applied, and one sound for the lot.
    void applyRulesToNewMail(const QStringList &ids);
    zmail::ui::PrivacyDialog *showPrivacyDialog(); // Settings > Privacy; caller shows it (tests)
    zmail::ui::SoundDialog *showSoundDialog(); // Settings > Sounds; caller shows it (tests)
    // Any theme id, built-in or custom ("custom:<stem>", "zterminal:<stem>");
    // remembered. A theme with ui.rowStripes also sets the row stripes.
    void setThemeId(const QString &id);
    // View > Theme > Theme Editor (non-modal, shown; returned for tests).
    zmail::ui::ThemeEditorDialog *showThemeEditor();

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
    void updateAccountStatus(); // colours the bottom-left account circle
    QString labelForMailbox(const QString &key) const;
    void showLiveMessage(int row);
    void updateMessageActions();
    void showSignatures();
    zmail::ui::ViewMessage sampleViewMessage(int row) const;
    void showContacts();
    void addSenderToContacts();
    void addToContacts(const QString &name, const QString &address); // unless already there
    void trashMessage(QString id); // single id (Message window); list actions use trashSelected
    void trashSelected();
    void trashMessages(const QStringList &ids);
    void onTrashFailed(const QString &id);
    QString currentListId() const;
    // Selected message-list rows (proxy order → source rows / Gmail ids).
    QList<int> selectedSourceRows() const;
    QStringList selectedMessageIds() const;
    void runFullTextSearch(const QString &text);
    void junkMessage(QString id);
    void junkSelected();
    void junkSelectedIds(const QStringList &ids);
    void notJunkMessage(QString id);
    void notJunkSelected();
    void notJunkSelectedIds(const QStringList &ids);
    void snoozeMessage(QString id, qint64 wakeMs);
    void snoozeSelected(qint64 wakeMs);
    void snoozeMessages(const QStringList &ids, qint64 wakeMs);
    void unsnoozeMessage(QString id);
    void unsnoozeSelected();
    void unsnoozeMessages(const QStringList &ids);
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
    // Labels-as-folders: New / Rename / Delete Folder, drag-move onto a label
    // or back onto In ("INBOX"). A message is in one folder at a time.
    void newLabelFolder(const QString &namePrefix = {});
    void renameLabelFolder(const QString &labelId, const QString &currentName);
    void deleteLabelFolder(const QString &labelId, const QString &displayName);
    void emptyLabelFolder(const QString &labelId, const QString &displayName); // its mail goes to Trash
    QMenu *buildFlagMenu(QWidget *parent);
    void moveMessagesToLabel(const QStringList &messageIds, const QString &targetLabelId);
    void setCurrentRead(bool read); // selected rows (Shift/Ctrl multi-select)
    void offerUndoDelete(const QStringList &ids);
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
    QLabel *m_accountStatus = nullptr; // coloured \u25cf next to the account line
    QLabel *m_syncLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    zmail::ui::MessageListModel *m_model = nullptr;
    zmail::ui::MessageFilterProxy *m_proxy = nullptr;
    UpdateChecker *m_updates = nullptr;
    QActionGroup *m_themeGroup = nullptr;
    QMenu *m_themeMenu = nullptr;
    QAction *m_themeEditorAction = nullptr;
    QList<QAction *> m_customThemeActions; // rebuilt from the themes dirs
    void rebuildCustomThemeActions();
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
    int m_listFontSize = 0;
    int m_listRowSpacing = 6;
    void applyListAppearance();
    void loadMoreIfListIsShort();
    QStringList m_savedAttachments;
    QMenu *m_mailboxMenu = nullptr;
    QMenu *m_transferMenu = nullptr;
    void rebuildMailboxMenus();
    bool m_sendingQueue = false;
    void sendNextQueued(int sent, int failed, QList<qint64> left);
    zmail::Rules m_rules;
    bool applyRule(const zmail::Rule &rule, const QString &messageId); // its flag / move / mark read
    QString m_lastSync;     // "8:45 AM"
    QString m_syncError;    // last SyncEngine::syncError; cleared on a later successful idle
    bool m_keepSyncErrorAcrossIdle = false;
    QString m_shownId;      // message currently in the preview
    int m_selectRowAfterReload = -1; // selectPastRemoved()'s row if its message vanished too
    // A Delete still waiting on Gmail: if it fails, the next reload selects
    // the message again, provided the user is still on the neighbour that
    // Delete moved to, in the same mailbox.
    struct PendingTrash
    {
        QString mailbox;
        QString neighbour; // id selected after the Delete ("" = none)
    };
    QHash<QString, PendingTrash> m_pendingTrash;
    QString m_reselectAfterReload; // id to select (and show) on the next reload
    QAction *m_undoDeleteAction = nullptr;
    QWidget *m_undoBar = nullptr;      // "Moved to Trash. [Undo]" in the status bar
    QLabel *m_undoLabel = nullptr;
    QTimer *m_undoTimer = nullptr;
    QStringList m_lastTrashed;        // ids the Undo puts back
    QTimer *m_reloadTimer = nullptr;
    QAction *m_hideSpamAction = nullptr;
    QTimer *m_snoozeTimer = nullptr;
    QMenu *buildSnoozeMenu(QWidget *parent = nullptr);
};
