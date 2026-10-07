#include <QDateTime>
#include "MainWindow.hpp"
#include "MainWindowDetail.h"

#include "AboutDialog.hpp"
#include "UpdateChecker.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/MessageParser.h"
#include "core/ReplyBuilder.h"
#include "core/Signatures.h"
#include "core/SizeFormat.h"
#include "core/SyncEngine.h"
#include "core/SnoozeTimes.h"
#include "ui/ComposeWindow.h"
#include "ui/ConnectDialog.h"
#include "ui/ContactsWindow.h"
#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "ui/NewMailSound.h"
#include "ui/SoundDialog.h"
#include "ui/SafeHtmlView.h"
#include "ui/PrivacyDialog.h"
#include "ui/SignaturesDialog.h"
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/Icons.h"
#include "ui/MessageListModel.h"
#include "ui/SelectionAfterRemoval.h"
#include "ui/ListDialog.h"
#include "ui/StripesDialog.h"
#include "ui/Theme.h"
#include "ui/ThemeEditorDialog.h"
#include "version.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QSettings>
#include <QGuiApplication>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QToolButton>
#include <QClipboard>
#include <QHBoxLayout>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QInputDialog>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QItemSelectionModel>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStyle>

using namespace zmail::ui;

namespace {
// Mailbox rows get 3 px of air above and below. Done with a delegate, not a
// widget style sheet: a style sheet freezes the tree's palette at polish
// time, so View > Theme > Dark left the sidebar white.
class RoomyRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        return QStyledItemDelegate::sizeHint(opt, index) + QSize(4, 6);
    }
};

// Message-list rows: the space between them is the user's choice
// (Settings > Message List), read from the list's "rowSpacing" property.
class MessageRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        return QStyledItemDelegate::sizeHint(opt, index) + QSize(0, parent()->property("rowSpacing").toInt());
    }
};

// Sidebar mailbox tree: accepts message-list drops onto user Gmail labels
// and onto In (folder-style move; back to the Inbox). onDrop(labelId, ids)
// is set by MainWindow; In is "INBOX".
class MailboxTree : public QTreeWidget
{
public:
    std::function<void(const QString &labelId, const QStringList &ids)> onDrop;

    explicit MailboxTree(QWidget *parent = nullptr) : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDropIndicatorShown(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (e->mimeData()->hasFormat(QByteArray(MessageListModel::kMessageIdsMime))) {
            e->acceptProposedAction();
        } else {
            e->ignore();
        }
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        if (!e->mimeData()->hasFormat(QByteArray(MessageListModel::kMessageIdsMime))) {
            e->ignore();
            return;
        }
        QTreeWidgetItem *it = itemAt(e->position().toPoint());
        const QString key = it ? it->data(0, Qt::UserRole).toString() : QString();
        if (!dropLabel(key).isEmpty()) {
            e->acceptProposedAction();
        } else {
            e->ignore();
        }
    }
    void dropEvent(QDropEvent *e) override
    {
        QTreeWidgetItem *it = itemAt(e->position().toPoint());
        const QString key = it ? it->data(0, Qt::UserRole).toString() : QString();
        if (dropLabel(key).isEmpty() || !onDrop) {
            e->ignore();
            return;
        }
        const QByteArray raw = e->mimeData()->data(QByteArray(MessageListModel::kMessageIdsMime));
        QStringList ids;
        for (const QByteArray &line : raw.split(char(10))) {
            const QString id = QString::fromUtf8(line).trimmed();
            if (!id.isEmpty()) {
                ids.append(id);
            }
        }
        e->acceptProposedAction();
        onDrop(dropLabel(key), ids);
    }

private:
    // The Gmail label a drop on this row moves to; empty: not a drop target.
    static QString dropLabel(const QString &key)
    {
        if (key == QLatin1String("In")) {
            return QStringLiteral("INBOX");
        }
        // User labels have Gmail ids like Label_12. System rows under Gmail
        // Labels (STARRED, …) are not folder drop targets.
        if (key.startsWith(QLatin1String("gmail:")) && key.mid(6).startsWith(QLatin1String("Label_"))) {
            return key.mid(6);
        }
        return {};
    }
};

QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

} // namespace

ViewMessage zmail::ui::detail::liveViewMessage(const zmail::CachedMessage &c, bool loading, const QString &error)
{
    ViewMessage v;
    v.id = c.id;
    v.from = c.fromName.isEmpty() ? c.fromAddr
                                  : (c.fromAddr.isEmpty() ? c.fromName
                                                          : QStringLiteral("%1 <%2>").arg(c.fromName, c.fromAddr));
    v.to = c.to;
    v.cc = c.cc;
    v.subject = c.subject;
    v.date = c.date();
    v.attachments = c.attachments;
    v.snippet = c.snippet;
    v.bodyHtml = c.bodyHtml;
    v.bodyText = c.bodyText;
    v.loading = loading;
    v.error = error;
    if (c.labels.contains(QStringLiteral("SPAM"))) {
        v.warning = QStringLiteral("<b>\u26a0 %1</b> %2")
                        .arg(esc(QObject::tr("Gmail put this message in Spam.")),
                             esc(QObject::tr("Links and remote images are blocked; check the sender before replying.")));
    }
    return v;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(baseTitle());

    // Stephen's desktop runs the Fildem global-menu extension, which swallowed
    // zterminal's menu bar. Keep the menu bar inside the window.
    menuBar()->setNativeMenuBar(false);

    m_updates = new UpdateChecker(this);
    connect(m_updates, &UpdateChecker::updateAvailable, this,
            [this](const QString &tag, const QString &url) {
                const auto choice = QMessageBox::question(
                    this, tr("Update available"),
                    tr("zmail %1 is available (you have %2).\n\nOpen the release page?")
                        .arg(tag, QString::fromLatin1(zmail::kVersionString)));
                if (choice == QMessageBox::Yes) {
                    QDesktopServices::openUrl(QUrl(url));
                }
            });
    connect(m_updates, &UpdateChecker::upToDate, this, [this]() {
        statusBar()->showMessage(
            tr("zmail %1 is up to date.").arg(QString::fromLatin1(zmail::kVersionString)), 5000);
    });
    connect(m_updates, &UpdateChecker::checkFailed, this, [this](const QString &reason) {
        statusBar()->showMessage(tr("Update check failed: %1").arg(reason), 8000);
    });

    m_model = new MessageListModel(this);
    m_model->setItems(sampleMail());
    m_proxy = new MessageFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setHideSpam(QSettings().value(QStringLiteral("mail/hideSpam"), true).toBool());

    m_sound = new NewMailSound(this);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);
    connect(m_reloadTimer, &QTimer::timeout, this, &MainWindow::reloadFromCache);
    m_snoozeTimer = new QTimer(this);
    m_snoozeTimer->setInterval(30 * 1000); // wake check while the app is open
    connect(m_snoozeTimer, &QTimer::timeout, this, &MainWindow::checkSnoozeWakes);

    buildMenus();
    buildToolbar();
    buildPanes();
    buildStatusBar();
    findChild<QAction *>(QString::fromLatin1(previewRight() ? "actionPreviewRight" : "actionPreviewBelow"))->setChecked(true);
    findChild<QAction *>(QStringLiteral("actionDarkMail"))->setChecked(m_view->darkMail());
    applyStripes();
    selectMailbox(QStringLiteral("In"));

    // Poll on window focus as well as on the 30 s timer.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st == Qt::ApplicationActive && m_live && m_session->sync()) {
            m_session->sync()->pollNow(false);
        }
    });
    resize(1180, 760);
}

QString MainWindow::baseTitle()
{
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(zmail::kAppName),
                                       QString::fromLatin1(zmail::kVersionString));
}

void MainWindow::buildMenus()
{
    auto addMenu = [this](const char *objName, const QString &title) {
        QMenu *m = menuBar()->addMenu(title);
        m->setObjectName(QString::fromLatin1(objName));
        return m;
    };
    auto later = [](QMenu *menu, const QString &text, const QKeySequence &ks = {}) {
        QAction *a = menu->addAction(text);
        a->setShortcut(ks);
        a->setEnabled(false); // placeholder until the feature lands:
        a->setVisible(false); // hidden, not greyed out (menus collapse the spare separators)
        a->setProperty("placeholder", true);
        return a;
    };

    QMenu *file = addMenu("menuFile", tr("&File"));
    QAction *nm = file->addAction(tr("&New Message"), this, [this]() { openCompose(); });
    nm->setShortcut(QKeySequence::New);
    QAction *cm = file->addAction(tr("&Check Mail"), this, &MainWindow::checkMail);
    cm->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    file->addSeparator();
    m_signInAction = file->addAction(icon(QStringLiteral("log-in")), tr("Sign &In to Gmail\u2026"), this,
                                     [this]() { showConnectDialog(); });
    m_signInAction->setObjectName(QStringLiteral("actionSignIn"));
    m_signInAction->setProperty("lucide", QStringLiteral("log-in"));
    m_signOutAction = file->addAction(icon(QStringLiteral("log-out")), tr("Sign &Out"), this, [this]() {
        if (m_session && QMessageBox::question(this, tr("Sign out"),
                                               tr("Sign out of %1? zmail forgets the saved sign-in and revokes "
                                                  "it at Google. Your mail stays in Gmail.")
                                                   .arg(m_session->account())) == QMessageBox::Yes) {
            m_session->signOut();
        }
    });
    m_signOutAction->setObjectName(QStringLiteral("actionSignOut"));
    m_signOutAction->setProperty("lucide", QStringLiteral("log-out"));
    m_signInAction->setEnabled(false);
    m_signOutAction->setEnabled(false);
    file->addSeparator();
    later(file, tr("&Save Attachments\u2026"));
    later(file, tr("&Print\u2026"), QKeySequence::Print);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *edit = addMenu("menuEdit", tr("&Edit"));
    later(edit, tr("&Copy"), QKeySequence::Copy);
    QAction *find = edit->addAction(tr("&Find\u2026"), this, [this]() { m_search->setFocus(); });
    find->setShortcut(QKeySequence::Find);
    edit->addSeparator();
    m_undoDeleteAction = edit->addAction(tr("&Undo Delete"), this, &MainWindow::undoDelete);
    m_undoDeleteAction->setObjectName(QStringLiteral("actionUndoDelete"));
    m_undoDeleteAction->setShortcut(QKeySequence::Undo);
    m_undoDeleteAction->setEnabled(false);

    QMenu *view = addMenu("menuView", tr("&View"));
    later(view, tr("View as &Plain Text"));
    QMenu *pane = view->addMenu(tr("&Preview Pane"));
    pane->setObjectName(QStringLiteral("menuPreviewPane"));
    auto *paneGroup = new QActionGroup(this);
    QAction *below = pane->addAction(tr("&Below the List"), this, [this]() { setPreviewRight(false); });
    below->setObjectName(QStringLiteral("actionPreviewBelow"));
    QAction *right = pane->addAction(tr("&Right of the List"), this, [this]() { setPreviewRight(true); });
    right->setObjectName(QStringLiteral("actionPreviewRight"));
    for (QAction *a : {below, right}) {
        a->setCheckable(true);
        paneGroup->addAction(a);
    }
    view->addSeparator();
    QAction *zin = view->addAction(tr("Zoom &In"), this, [this]() { m_view->zoomIn(); });
    zin->setObjectName(QStringLiteral("actionZoomIn"));
    zin->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    QAction *zout = view->addAction(tr("Zoom &Out"), this, [this]() { m_view->zoomOut(); });
    zout->setObjectName(QStringLiteral("actionZoomOut"));
    zout->setShortcuts({QKeySequence::ZoomOut});
    QAction *zreset = view->addAction(tr("&Actual Size"), this, [this]() { m_view->resetZoom(); });
    zreset->setObjectName(QStringLiteral("actionZoomReset"));
    zreset->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    QAction *dark = view->addAction(tr("&Dark Background for Messages"));
    dark->setObjectName(QStringLiteral("actionDarkMail"));
    dark->setCheckable(true);
    connect(dark, &QAction::toggled, this, [this](bool on) { m_view->setDarkMail(on); });
    view->addAction(soundAction()); // Play Sound for New Mail (also on the toolbar)
    view->addSeparator();
    m_hideSpamAction = view->addAction(tr("&Hide Spam from Folders"));
    m_hideSpamAction->setObjectName(QStringLiteral("actionHideSpam"));
    m_hideSpamAction->setCheckable(true);
    m_hideSpamAction->setChecked(hideSpam());
    m_hideSpamAction->setToolTip(tr("Hide the Spam folder and keep spam out of Inbox and other views"));
    connect(m_hideSpamAction, &QAction::toggled, this, &MainWindow::setHideSpam);
    QAction *showSpam = view->addAction(tr("Show &Spam"), this, &MainWindow::showSpamFolder);
    showSpam->setObjectName(QStringLiteral("actionShowSpam"));
    showSpam->setToolTip(tr("Open the Spam / Junk mailbox"));
    view->addSeparator();
    QMenu *theme = view->addMenu(tr("&Theme"));
    theme->setObjectName(QStringLiteral("menuTheme"));
    m_themeGroup = new QActionGroup(this);
    struct T { const char *obj; QString text; ThemeMode mode; };
    for (const T &t : {T{"actionThemeLight", tr("&Light"), ThemeMode::Light},
                       T{"actionThemeDark", tr("&Dark"), ThemeMode::Dark},
                       T{"actionThemeSystem", tr("Follow &System"), ThemeMode::System},
                       // Brand themes shared with zterminal (ui/BrandThemes.h, docs/THEMES.md).
                       T{"actionThemeBoilermakers", tr("&Boilermakers"), ThemeMode::Boilermakers},
                       T{"actionThemeBadgers", tr("B&adgers"), ThemeMode::Badgers},
                       T{"actionThemePackers", tr("&Packers"), ThemeMode::Packers}}) {
        if (t.mode == ThemeMode::Boilermakers) {
            theme->addSeparator();
        }
        QAction *a = theme->addAction(t.text);
        a->setObjectName(QString::fromLatin1(t.obj));
        a->setData(themeId(t.mode));
        a->setCheckable(true);
        m_themeGroup->addAction(a);
        const ThemeMode mode = t.mode;
        connect(a, &QAction::triggered, this, [this, mode]() { setTheme(mode); });
    }
    // Custom themes (Theme Editor, *.ztheme.json) go between here and the editor.
    m_themeMenu = theme;
    theme->addSeparator();
    m_themeEditorAction = theme->addAction(tr("Theme &Editor\u2026"), this, [this]() { showThemeEditor(); });
    m_themeEditorAction->setObjectName(QStringLiteral("actionThemeEditor"));
    connect(theme, &QMenu::aboutToShow, this, &MainWindow::rebuildCustomThemeActions);
    rebuildCustomThemeActions();

    QMenu *message = addMenu("menuMessage", tr("&Message"));
    QAction *openWin = message->addAction(tr("&Open in New Window"), this, [this]() {
        openMessageWindow(m_list->currentIndex());
    });
    openWin->setObjectName(QStringLiteral("actionOpenMessage"));
    openWin->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    message->addSeparator();
    using Kind = zmail::ReplyBuilder::Kind;
    QAction *mr = message->addAction(tr("&Reply"), this, [this]() { composeReply(int(Kind::Reply)); });
    mr->setObjectName(QStringLiteral("menuActionReply"));
    mr->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    QAction *mra = message->addAction(tr("Reply &All"), this, [this]() { composeReply(int(Kind::ReplyAll)); });
    mra->setObjectName(QStringLiteral("menuActionReplyAll"));
    mra->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    QAction *mf = message->addAction(tr("&Forward"), this, [this]() { composeReply(int(Kind::Forward)); });
    mf->setObjectName(QStringLiteral("menuActionForward"));
    mf->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    message->addSeparator();
    QAction *markRead = message->addAction(icon(QStringLiteral("mail-open")), tr("Mark as R&ead"), this,
                                           [this]() { setCurrentRead(true); });
    markRead->setObjectName(QStringLiteral("actionMarkRead"));
    markRead->setProperty("lucide", QStringLiteral("mail-open"));
    QAction *markUnread = message->addAction(icon(QStringLiteral("mail")), tr("Mark as &Unread"), this,
                                             [this]() { setCurrentRead(false); });
    markUnread->setObjectName(QStringLiteral("actionMarkUnread"));
    markUnread->setProperty("lucide", QStringLiteral("mail"));
    QAction *del = message->addAction(icon(QStringLiteral("trash")), tr("&Delete"), this,
                                      [this]() { trashSelected(); });
    del->setObjectName(QStringLiteral("menuActionDelete"));
    del->setProperty("lucide", QStringLiteral("trash"));
    del->setShortcuts({QKeySequence::Delete});
    message->addSeparator();
    QAction *addContact = message->addAction(tr("Add Sender to &Contacts"), this, &MainWindow::addSenderToContacts);
    addContact->setObjectName(QStringLiteral("actionAddSenderToContacts"));
    QAction *junk = message->addAction(icon(QStringLiteral("shield-alert")), tr("Mark as &Junk"), this,
                                       [this]() { junkSelected(); });
    junk->setObjectName(QStringLiteral("menuActionJunk"));
    junk->setProperty("lucide", QStringLiteral("shield-alert"));
    junk->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    QAction *notJunk = message->addAction(icon(QStringLiteral("mail")), tr("Not &Junk"), this,
                                          [this]() { notJunkSelected(); });
    notJunk->setObjectName(QStringLiteral("menuActionNotJunk"));
    notJunk->setProperty("lucide", QStringLiteral("mail"));
    notJunk->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
    notJunk->setVisible(false);
    QMenu *snoozeMenu = buildSnoozeMenu(message);
    snoozeMenu->setTitle(tr("S&nooze"));
    snoozeMenu->setObjectName(QStringLiteral("menuSnooze"));
    message->addMenu(snoozeMenu);
    QAction *unsnooze = message->addAction(tr("&Unsnooze"), this, [this]() { unsnoozeSelected(); });
    unsnooze->setObjectName(QStringLiteral("menuActionUnsnooze"));
    unsnooze->setVisible(false);

    QMenu *settings = addMenu("menuSettings", tr("&Settings"));
    later(settings, tr("&Account\u2026"));
    later(settings, tr("&Rules (Sounds && Colours)\u2026"));
    QAction *sigs = settings->addAction(tr("Si&gnatures\u2026"), this, &MainWindow::showSignatures);
    sigs->setObjectName(QStringLiteral("actionSignatures"));
    QAction *privacy = settings->addAction(tr("&Privacy\u2026"), this, [this]() { showPrivacyDialog()->open(); });
    privacy->setObjectName(QStringLiteral("actionPrivacy"));
    QAction *sounds = settings->addAction(tr("S&ounds\u2026"), this, [this]() { showSoundDialog()->open(); });
    sounds->setObjectName(QStringLiteral("actionSounds"));
    QAction *contacts = settings->addAction(tr("&Contacts…"), this, &MainWindow::showContacts);
    contacts->setObjectName(QStringLiteral("actionContacts"));
    QAction *syncContacts = settings->addAction(tr("Sync &Contacts from Google"), this, [this]() {
        if (m_session) {
            m_session->enableContactsSync();
        } else {
            statusBar()->showMessage(tr("Sign in to Gmail to sync contacts."), 5000);
        }
    });
    syncContacts->setObjectName(QStringLiteral("actionSyncContacts"));
    settings->addSeparator();
    m_stripes = stripeStrengthFromSetting(QSettings().value(QStringLiteral("ui/rowStripes")));
    QAction *stripes = settings->addAction(tr("Row S&tripes\u2026"), this, [this]() { showStripesDialog(); });
    QAction *listLook = settings->addAction(tr("Message &List\u2026"), this, [this]() { showListDialog(); });
    listLook->setObjectName(QStringLiteral("actionMessageList"));
    stripes->setObjectName(QStringLiteral("actionRowStripes"));

    QMenu *help = addMenu("menuHelp", tr("&Help"));
    QAction *about = help->addAction(tr("&About zmail"), this, &MainWindow::showAbout);
    about->setObjectName(QStringLiteral("actionAbout"));
    QAction *upd = help->addAction(tr("Check for &Updates\u2026"), this, &MainWindow::checkForUpdates);
    upd->setObjectName(QStringLiteral("actionCheckForUpdates"));
}

void MainWindow::buildToolbar()
{
    auto *tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(22, 22));
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    // Toolbar buttons share the menu items' shortcuts; the tooltip names
    // the key the same way for every button ("Reply to sender (Ctrl+R)").
    struct B { const char *obj; const char *icon; QString text; QString tip; QKeySequence key; };
    const QList<B> buttons{
        {"actionCheckMail", "refresh-cw", tr("Check Mail"), tr("Check for new mail"), QKeySequence(Qt::CTRL | Qt::Key_M)},
        {"actionNewMessage", "square-pen", tr("New Message"), tr("Compose a new message"), QKeySequence(QKeySequence::New)},
        {"actionReply", "reply", tr("Reply"), tr("Reply to sender"), QKeySequence(Qt::CTRL | Qt::Key_R)},
        {"actionReplyAll", "reply-all", tr("Reply All"), tr("Reply to all recipients"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R)},
        {"actionForward", "forward", tr("Forward"), tr("Forward this message"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)},
        {"actionDelete", "trash", tr("Delete"), tr("Move to Trash"), QKeySequence(QKeySequence::Delete)},
        {"actionJunk", "shield-alert", tr("Junk"), tr("Mark as Junk (move to Spam)"),
         QKeySequence(Qt::CTRL | Qt::Key_J)},
        {"actionAttach", "paperclip", tr("Attach"), tr("New message with an attachment"), {}},
    };
    for (const B &b : buttons) {
        if (qstrcmp(b.obj, "actionDelete") == 0 || qstrcmp(b.obj, "actionReply") == 0) {
            tb->addSeparator();
        }
        QAction *a = tb->addAction(icon(QString::fromLatin1(b.icon)), b.text);
        a->setObjectName(QString::fromLatin1(b.obj));
        a->setToolTip(withShortcut(b.tip, b.key));
        a->setProperty("lucide", QString::fromLatin1(b.icon));
    }
    connect(findChild<QAction *>(QStringLiteral("actionNewMessage")), &QAction::triggered, this,
            [this]() { openCompose(); });
    using Kind = zmail::ReplyBuilder::Kind;
    connect(findChild<QAction *>(QStringLiteral("actionReply")), &QAction::triggered, this,
            [this]() { composeReply(int(Kind::Reply)); });
    connect(findChild<QAction *>(QStringLiteral("actionReplyAll")), &QAction::triggered, this,
            [this]() { composeReply(int(Kind::ReplyAll)); });
    connect(findChild<QAction *>(QStringLiteral("actionForward")), &QAction::triggered, this,
            [this]() { composeReply(int(Kind::Forward)); });
    connect(findChild<QAction *>(QStringLiteral("actionAttach")), &QAction::triggered, this, [this]() {
        ComposeWindow *c = openCompose();
        if (QAction *a = c->findChild<QAction *>(QStringLiteral("actionComposeAttach"))) {
            QMetaObject::invokeMethod(a, &QAction::trigger, Qt::QueuedConnection);
        }
    });
    connect(findChild<QAction *>(QStringLiteral("actionCheckMail")), &QAction::triggered, this,
            &MainWindow::checkMail);
    connect(findChild<QAction *>(QStringLiteral("actionDelete")), &QAction::triggered, this,
            [this]() { trashSelected(); });
    connect(findChild<QAction *>(QStringLiteral("actionJunk")), &QAction::triggered, this, [this]() {
        if (m_proxy->mailbox() == QLatin1String("Junk")) {
            notJunkSelected();
        } else {
            junkSelected();
        }
    });
    addSoundButton(tb);

    auto *spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_search = new QLineEdit(tb);
    m_search->setObjectName(QStringLiteral("searchBox"));
    m_search->setPlaceholderText(tr("Search all mail  (from: subject: has:attachment …)"));
    m_search->setToolTip(MessageFilterProxy::searchHelp());
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(300);
    QAction *lead = m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    lead->setProperty("lucide", QStringLiteral("search"));
    tb->addWidget(m_search);
    auto *pad = new QWidget(tb);
    pad->setFixedWidth(8);
    tb->addWidget(pad);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        runFullTextSearch(t);
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        runFullTextSearch(m_search->text());
    });
}

void MainWindow::buildPanes()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("mainSplitter"));
    m_splitter->setHandleWidth(5);

    auto *mailboxTree = new MailboxTree(m_splitter);
    m_mailboxes = mailboxTree;
    m_mailboxes->setObjectName(QStringLiteral("mailboxTree"));
    m_mailboxes->setColumnCount(2);
    m_mailboxes->setHeaderHidden(true);
    m_mailboxes->setRootIsDecorated(true);
    m_mailboxes->setIndentation(14);
    m_mailboxes->setIconSize(QSize(16, 16));
    m_mailboxes->header()->setStretchLastSection(false);
    m_mailboxes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_mailboxes->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_mailboxes->setItemDelegate(new RoomyRowDelegate(m_mailboxes));
    mailboxTree->onDrop = [this](const QString &labelId, const QStringList &ids) {
        moveMessagesToLabel(ids, labelId);
    };

    m_listSplitter = new QSplitter(Qt::Vertical, m_splitter);
    m_listSplitter->setObjectName(QStringLiteral("listPreviewSplitter"));
    m_listSplitter->setHandleWidth(5);

    m_list = new QTreeView(m_listSplitter);
    m_list->setObjectName(QStringLiteral("messageList"));
    m_list->setModel(m_proxy);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSortingEnabled(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Shift+click / Shift+arrows range-select; Ctrl/Cmd+click toggles a row.
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setIconSize(QSize(14, 14));
    m_list->sortByColumn(MessageListModel::Date, Qt::DescendingOrder);
    QHeaderView *h = m_list->header();
    h->setStretchLastSection(true);
    h->setSectionsMovable(true);
    h->setHighlightSections(false);
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (int c : {int(MessageListModel::Status), int(MessageListModel::Priority),
                  int(MessageListModel::Attachment), int(MessageListModel::Label)}) {
        h->setSectionResizeMode(c, QHeaderView::Fixed);
        h->resizeSection(c, 28);
    }
    h->resizeSection(MessageListModel::Who, 190);
    h->resizeSection(MessageListModel::Date, 140);
    // Room for "888.8 MB" plus padding, at any font size or scale.
    h->resizeSection(MessageListModel::Size,
                     std::max(68, m_list->fontMetrics().horizontalAdvance(QStringLiteral("888.8 MB")) + 18));
    m_list->setItemDelegate(new MessageRowDelegate(m_list));
    m_listFontSize = QSettings().value(QStringLiteral("ui/listFontSize"), kListFontDefault).toInt();
    m_listRowSpacing = QSettings().value(QStringLiteral("ui/listRowSpacing"), kListSpacingDefault).toInt();
    applyListAppearance();
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &cur) { showMessage(cur); });

    m_view = new MessageView(m_listSplitter);
    m_view->setObjectName(QStringLiteral("messageView"));
    m_view->body()->setObjectName(QStringLiteral("previewPane"));
    connect(m_view, &MessageView::mailtoRequested, this, [this](const QUrl &u) { composeMailto(u); });
    connect(m_list, &QTreeView::doubleClicked, this, [this](const QModelIndex &i) { openMessageWindow(i); });

    populateMailboxes();
    connect(m_mailboxes, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) {
            const QString key = it->data(0, Qt::UserRole).toString();
            if (key == m_proxy->mailbox()) {
                return; // selectMailbox already set this (avoids a rebuild loop)
            }
            selectMailbox(key);
            if (m_live && m_session->sync()) {
                m_session->sync()->ensureLabel(labelForMailbox(key));
            }
            // Live mail: don't auto-open (that would mark the newest message read).
            if (m_live) {
                m_view->clear();
                m_shownId.clear();
                updateMessageActions();
            }
        }
    });

    // Infinite scroll: the next page of this label when the list hits bottom.
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        QScrollBar *sb = m_list->verticalScrollBar();
        if (m_live && m_session->sync() && sb->maximum() > 0 && v >= sb->maximum() - 2) {
            m_session->sync()->fetchMore(labelForMailbox(m_proxy->mailbox()));
        }
    });

    // ... and when the list is too short to scroll at all (see loadMoreIfListIsShort).
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &MainWindow::loadMoreIfListIsShort);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &MainWindow::loadMoreIfListIsShort);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &MainWindow::loadMoreIfListIsShort);

    m_listSplitter->setStretchFactor(0, 1);
    m_listSplitter->setStretchFactor(1, 2);
    m_listSplitter->setChildrenCollapsible(false);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({210, 970});
    {
        QSettings st;
        const QByteArray main = st.value(QStringLiteral("ui/mainSplitter")).toByteArray();
        if (!main.isEmpty()) {
            m_splitter->restoreState(main);
        }
        m_listSplitter->setOrientation(st.value(QStringLiteral("ui/previewRight"), false).toBool() ? Qt::Horizontal
                                                                                               : Qt::Vertical);
    }
    restoreListSplitter();
    connect(m_listSplitter, &QSplitter::splitterMoved, this, &MainWindow::saveSplitters);
    connect(m_splitter, &QSplitter::splitterMoved, this, &MainWindow::saveSplitters);
    installListActions();
    setCentralWidget(m_splitter);
}

void MainWindow::buildStatusBar()
{
    auto *row = new QWidget(this);
    row->setObjectName(QStringLiteral("accountStatusRow"));
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    m_accountStatus = new QLabel(QStringLiteral("\u25cf"), row);
    m_accountStatus->setObjectName(QStringLiteral("accountStatus"));
    m_accountStatus->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_syncLabel = new QLabel(row);
    m_syncLabel->setObjectName(QStringLiteral("syncLabel"));
    lay->addWidget(m_accountStatus, 0);
    lay->addWidget(m_syncLabel, 1);
    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("countLabel"));
    statusBar()->addWidget(row, 1);
    statusBar()->addPermanentWidget(m_countLabel);
    updateSyncLabel();
}

void MainWindow::showMessage(const QModelIndex &proxyIndex)
{
    if (!proxyIndex.isValid()) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
        return;
    }
    if (m_live) {
        showLiveMessage(m_proxy->mapToSource(proxyIndex).row());
        return;
    }
    m_view->setMessage(sampleViewMessage(m_proxy->mapToSource(proxyIndex).row()));
    updateMessageActions();
}

ViewMessage MainWindow::sampleViewMessage(int row) const
{
    const MailItem &m = m_model->item(row);
    ViewMessage v;
    v.id = QStringLiteral("sample-%1").arg(row);
    const bool outgoing = m.mailboxes.contains(QStringLiteral("Out"));
    const QString who = QStringLiteral("%1 <%2>").arg(m.who, m.address);
    const QString me = QStringLiteral("Alex Morgan <alex.morgan@example.com>");
    v.from = outgoing ? me : who;
    v.to = outgoing ? who : me;
    v.subject = m.subject;
    v.date = m.date;
    v.label = m.label;
    v.labelColor = m.labelColor;
    v.attachments = m.attachments;
    v.bodyText = m.preview;
    if (m.suspicious) {
        v.warning = QStringLiteral(
                        "<b>\u26a0 This message looks suspicious.</b><br>"
                        "\u2022 The sender's domain <i>contoso-secure-login.example</i> isn't Contoso Bank's.<br>"
                        "\u2022 The link text doesn't match where the link really goes.<br>"
                        "\u2022 Urgent account-verification wording.<br>"
                        "<small>Remote images are blocked. zmail never deletes mail on its own.</small>");
    }
    return v;
}

void MainWindow::refreshIcons()
{
    for (QAction *a : findChildren<QAction *>()) {
        const QString name = a->property("lucide").toString();
        if (!name.isEmpty()) {
            a->setIcon(icon(name));
        }
    }
    const QString box = m_proxy->mailbox();
    const QModelIndex cur = m_list->currentIndex();
    populateMailboxes();
    selectMailbox(box);
    m_model->paletteChanged();
    applyStripes();
    if (cur.isValid()) {
        m_list->setCurrentIndex(cur);
    }
    showMessage(m_list->currentIndex());
    updateAccountStatus();
}

void MainWindow::setTheme(ThemeMode mode)
{
    setThemeId(themeId(mode));
}

void MainWindow::setThemeId(const QString &id)
{
    applyThemeId(id);
    QSettings().setValue(QLatin1String(kThemeSettingKey), currentThemeId()); // restored by main()
    if (const sbj::theme::Theme *t = currentCustomTheme(); t && t->ui.rowStripes) {
        setStripeStrength(*t->ui.rowStripes);
    }
    rebuildCustomThemeActions(); // also re-checks the current theme
    refreshIcons();
}

void MainWindow::rebuildCustomThemeActions()
{
    qDeleteAll(m_customThemeActions);
    m_customThemeActions.clear();
    QAction *before = m_themeEditorAction; // customs, a separator, then Theme Editor
    const QList<CustomTheme> custom = customThemes();
    for (const CustomTheme &c : custom) {
        auto *a = new QAction(c.name, m_themeMenu);
        a->setData(c.id);
        a->setCheckable(true);
        a->setObjectName(QStringLiteral("actionTheme:") + c.id);
        m_themeMenu->insertAction(before, a);
        m_themeGroup->addAction(a);
        const QString id = c.id;
        connect(a, &QAction::triggered, this, [this, id]() { setThemeId(id); });
        m_customThemeActions.append(a);
    }
    if (!custom.isEmpty()) {
        m_customThemeActions.append(m_themeMenu->insertSeparator(before));
    }
    const QString cur = currentThemeId();
    for (QAction *a : m_themeGroup->actions()) {
        a->setChecked(a->data().toString() == cur);
    }
}

ThemeEditorDialog *MainWindow::showThemeEditor()
{
    auto *dlg = new ThemeEditorDialog(currentThemeId(), m_stripes, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &ThemeEditorDialog::applied, this, &MainWindow::setThemeId);
    connect(dlg, &ThemeEditorDialog::themesChanged, this, &MainWindow::rebuildCustomThemeActions);
    dlg->show();
    return dlg;
}

ComposeWindow *MainWindow::openCompose(bool sampleReply)
{
    auto *c = new ComposeWindow(this);
    c->setWindowFlag(Qt::Window, true);
    if (m_live && m_session) {
        c->setSession(m_session);
        c->setAttribute(Qt::WA_DeleteOnClose, true);
        connect(c, &QObject::destroyed, this, [this, c]() { m_composers.removeAll(c); });
        connect(c, &ComposeWindow::sent, this,
                [this]() { statusBar()->showMessage(tr("Message sent"), 6000); });
    } else if (sampleReply) {
        c->loadSampleReply();
    }
    m_composers.append(c);
    c->show();
    return c;
}

ComposeWindow *MainWindow::composeReply(int kindInt, const QString &forId)
{
    const auto kind = zmail::ReplyBuilder::Kind(kindInt);
    if (!m_live) {
        return openCompose(true);
    }
    const QString id = forId.isEmpty() ? m_shownId : forId;
    if (id.isEmpty() || !m_session || !m_session->sync()) {
        return nullptr;
    }
    ComposeWindow *c = openCompose();
    QPointer<ComposeWindow> guard(c);
    m_session->sync()->fetchBody(id, [this, guard, kind, id](const zmail::CachedMessage &m, const QString &err) {
        if (!guard || !m_session) {
            return;
        }
        guard->setDraft(zmail::ReplyBuilder::make(kind, m, m_session->account()));
        if (kind == zmail::ReplyBuilder::Kind::Forward && m.hasAttachment) {
            guard->attachFromMessage(id);
        }
        if (!err.isEmpty()) {
            statusBar()->showMessage(err, 8000);
        }
    });
    return c;
}

ComposeWindow *MainWindow::composeMailto(const QUrl &url)
{
    ComposeWindow *c = openCompose();
    c->setMailto(zmail::Mailto::parse(url));
    return c;
}

void MainWindow::updateMessageActions()
{
    const bool on = !m_live || !m_shownId.isEmpty();
    for (const char *n : {"actionReply", "actionReplyAll", "actionForward", "menuActionReply", "menuActionReplyAll",
                          "menuActionForward"}) {
        if (QAction *a = findChild<QAction *>(QString::fromLatin1(n))) {
            a->setEnabled(on);
        }
    }
    // Delete / Mark / Junk: only with a message actually selected (so never in an
    // empty mailbox), in sample mode too. Multi-select keeps a current index.
    const bool selected = m_list && m_list->currentIndex().isValid() && (!m_live || !m_shownId.isEmpty());
    for (const char *n : {"actionDelete", "menuActionDelete", "actionMarkRead", "actionMarkUnread",
                          "actionJunk", "menuActionJunk", "menuActionNotJunk", "menuActionUnsnooze"}) {
        if (QAction *a = findChild<QAction *>(QString::fromLatin1(n))) {
            a->setEnabled(selected);
        }
    }
    const bool onJunk = m_proxy && m_proxy->mailbox() == QLatin1String("Junk");
    if (QAction *j = findChild<QAction *>(QStringLiteral("menuActionJunk"))) {
        j->setVisible(!onJunk);
    }
    if (QAction *nj = findChild<QAction *>(QStringLiteral("menuActionNotJunk"))) {
        nj->setVisible(onJunk);
    }
    if (QAction *tb = findChild<QAction *>(QStringLiteral("actionJunk"))) {
        const QString name = onJunk ? QStringLiteral("mail") : QStringLiteral("shield-alert");
        tb->setText(onJunk ? tr("Not Junk") : tr("Junk"));
        tb->setIconText(onJunk ? tr("Not Junk") : tr("Junk"));
        tb->setProperty("lucide", name);
        tb->setIcon(icon(name));
        tb->setToolTip(withShortcut(onJunk ? tr("Not Junk (move out of Spam)") : tr("Mark as Junk (move to Spam)"),
                                    QKeySequence(onJunk ? (Qt::CTRL | Qt::SHIFT | Qt::Key_J)
                                                        : (Qt::CTRL | Qt::Key_J))));
    }
    const bool onSnoozed = m_proxy && m_proxy->mailbox() == QLatin1String("Snoozed");
    if (QAction *u = findChild<QAction *>(QStringLiteral("menuActionUnsnooze"))) {
        u->setVisible(onSnoozed);
    }
    if (QMenu *sm = findChild<QMenu *>(QStringLiteral("menuSnooze"))) {
        sm->setEnabled(selected && !onSnoozed);
    }
}

void MainWindow::showSignatures()
{
    zmail::SignatureStore store;
    SignaturesDialog dlg(&store, this);
    dlg.exec();
}

void MainWindow::showAbout()
{
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::checkForUpdates()
{
    statusBar()->showMessage(tr("Checking for updates\u2026"));
    m_updates->checkForUpdates(QString::fromLatin1(zmail::kVersionString),
                               QString::fromLatin1(zmail::kRepo));
}

// ---- Gmail session ---------------------------------------------------------

bool MainWindow::previewRight() const
{
    return m_listSplitter && m_listSplitter->orientation() == Qt::Horizontal;
}

void MainWindow::setPreviewRight(bool right)
{
    if (right != previewRight()) {
        saveSplitters();
        m_listSplitter->setOrientation(right ? Qt::Horizontal : Qt::Vertical);
        restoreListSplitter();
    }
    QSettings().setValue(QStringLiteral("ui/previewRight"), right);
    if (QAction *a = findChild<QAction *>(QString::fromLatin1(right ? "actionPreviewRight" : "actionPreviewBelow"))) {
        a->setChecked(true);
    }
}

void MainWindow::restoreListSplitter()
{
    // One remembered layout per orientation, so flipping back restores it.
    const bool right = previewRight();
    const QByteArray st =
        QSettings().value(right ? QStringLiteral("ui/listSplitterRight") : QStringLiteral("ui/listSplitterBelow"))
            .toByteArray();
    if (st.isEmpty() || !m_listSplitter->restoreState(st)) {
        // About 35/65: the message gets the larger share by default.
        m_listSplitter->setSizes({350, 650});
    }
    // restoreState() also restores orientation; keep the chosen one.
    m_listSplitter->setOrientation(right ? Qt::Horizontal : Qt::Vertical);
}

void MainWindow::saveSplitters()
{
    QSettings st;
    st.setValue(previewRight() ? QStringLiteral("ui/listSplitterRight") : QStringLiteral("ui/listSplitterBelow"),
                m_listSplitter->saveState());
    st.setValue(QStringLiteral("ui/mainSplitter"), m_splitter->saveState());
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    saveSplitters();
    QMainWindow::closeEvent(ev);
}

QList<MessageWindow *> MainWindow::messageWindows() const
{
    QList<MessageWindow *> out;
    for (const auto &w : m_messageWindows) {
        if (w) {
            out.append(w);
        }
    }
    return out;
}

MessageWindow *MainWindow::openMessageWindow(const QModelIndex &proxyIndex)
{
    if (!proxyIndex.isValid()) {
        return nullptr;
    }
    const int row = m_proxy->mapToSource(proxyIndex).row();
    auto *w = new MessageWindow(this);
    m_messageWindows.removeAll(nullptr);
    m_messageWindows.append(w);
    connect(w, &MessageWindow::composeRequested, this,
            [this](const QString &id, int kind) { composeReply(kind, id); });
    connect(w, &MessageWindow::mailtoRequested, this, [this](const QUrl &u) { composeMailto(u); });
    connect(w, &MessageWindow::deleteRequested, this, [this, w](const QString &id) {
        trashMessage(id);
        w->close();
    });
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    zmail::MailCache *cache = m_live && m_session ? m_session->cache() : nullptr;
    if (sync && cache) {
        const MailItem &item = m_model->item(row);
        const QString id = item.id;
        const zmail::CachedMessage c = cache->message(id);
        w->setMessage(detail::liveViewMessage(c, !c.hasBody, {}));
        if (!c.hasBody) {
            QPointer<MessageWindow> guard(w);
            sync->fetchBody(id, [guard](const zmail::CachedMessage &full, const QString &err) {
                if (guard) {
                    guard->setMessage(detail::liveViewMessage(full, false, err));
                }
            });
        }
        if (c.unread()) {
            sync->markRead(id);
            m_model->setStatus(row, MailStatus::Read);
            updateCounts();
        }
    } else {
        w->setMessage(sampleViewMessage(row));
        w->deleteAction()->setEnabled(false);
        w->deleteAction()->setToolTip(tr("Sign in to Gmail to delete mail"));
    }
    w->show();
    return w;
}

void MainWindow::showContacts()
{
    zmail::ContactStore *store = m_live && m_session ? m_session->contacts() : nullptr;
    if (!store || !store->isOpen()) {
        statusBar()->showMessage(tr("Sign in to Gmail to use Contacts."), 5000);
        return;
    }
    auto *dlg = new ContactsWindow(store, m_session->contactsSync(), this);
    dlg->setSyncTrigger([this]() {
        if (m_session) {
            m_session->enableContactsSync();
        }
    });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::addSenderToContacts()
{
    zmail::ContactStore *store = m_live && m_session ? m_session->contacts() : nullptr;
    if (!store || !store->isOpen()) {
        statusBar()->showMessage(tr("Sign in to Gmail to save contacts."), 5000);
        return;
    }
    if (m_shownId.isEmpty() || !m_session->cache()) {
        statusBar()->showMessage(tr("Select a message first."), 5000);
        return;
    }
    const zmail::CachedMessage c = m_session->cache()->message(m_shownId);
    if (c.fromAddr.isEmpty()) {
        statusBar()->showMessage(tr("This message has no From address."), 5000);
        return;
    }
    store->addLocalContact(c.fromName, c.fromAddr);
    statusBar()->showMessage(tr("Saved %1 locally (not uploaded to Google).").arg(c.fromAddr), 5000);
}

void MainWindow::runFullTextSearch(const QString &text)
{
    const QString trimmed = text.trimmed();
    m_proxy->setSearchText(text);
    if (trimmed.isEmpty()) {
        m_proxy->setSearchIds({});
        if (m_proxy->mailbox() == QLatin1String("Search")) {
            const QString back = m_mailboxBeforeSearch.isEmpty() ? QStringLiteral("In") : m_mailboxBeforeSearch;
            m_mailboxBeforeSearch.clear();
            populateMailboxes();
            selectMailbox(back);
        } else {
            updateCounts();
        }
        return;
    }
    QStringList ids;
    bool fullText = false;
    if (m_live && m_session && m_session->cache()) {
        ids = m_session->cache()->search(trimmed, 500, &fullText);
    }
    // Sample mode: empty searchIds → proxy matches terms against every loaded row.
    m_proxy->setSearchIds(ids, fullText);
    if (m_proxy->mailbox() != QLatin1String("Search")) {
        m_mailboxBeforeSearch = m_proxy->mailbox();
    }
    populateMailboxes();
    selectMailbox(QStringLiteral("Search"));
    if (m_live) {
        statusBar()->showMessage(tr("%n match(es)", nullptr, ids.size()), 3000);
    }
}

void MainWindow::setStripeStrength(int strength)
{
    m_stripes = std::clamp(strength, 0, kStripeMax);
    QSettings().setValue(QStringLiteral("ui/rowStripes"), m_stripes);
    applyStripes();
}

PrivacyDialog *MainWindow::showPrivacyDialog()
{
    auto *dlg = new PrivacyDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, []() {
        // The preview and any open message windows.
        for (QWidget *top : QApplication::topLevelWidgets()) {
            for (MessageView *v : top->findChildren<MessageView *>()) {
                v->reloadImagePolicy();
            }
        }
    });
    return dlg;
}

SoundDialog *MainWindow::showSoundDialog()
{
    auto *dlg = new SoundDialog(m_sound, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this]() {
        // Keep View/toolbar mute in step with Settings > Sounds.
        setNewMailSoundOn(m_sound->isEnabled());
    });
    return dlg;
}

void MainWindow::setListAppearance(int fontSize, int rowSpacing)
{
    m_listFontSize = fontSize;
    m_listRowSpacing = rowSpacing;
    applyListAppearance(); // clamps
    QSettings().setValue(QStringLiteral("ui/listFontSize"), m_listFontSize);
    QSettings().setValue(QStringLiteral("ui/listRowSpacing"), m_listRowSpacing);
}

void MainWindow::applyListAppearance()
{
    m_listFontSize = m_listFontSize <= 0 ? kListFontDefault : std::clamp(m_listFontSize, kListFontMin, kListFontMax);
    m_listRowSpacing = std::clamp(m_listRowSpacing, 0, kListSpacingMax);
    if (!m_list) {
        return;
    }
    // Only the size is set, so the family still follows the theme's UI font;
    // an empty font hands the size back to the application font too.
    QFont f;
    if (m_listFontSize != kListFontDefault) {
        f.setPointSize(m_listFontSize);
    }
    m_list->setFont(f);
    m_list->setProperty("rowSpacing", m_listRowSpacing);
    // Room for "888.8 MB" plus padding, at any font size or scale.
    QHeaderView *h = m_list->header();
    const int size = std::max(68, m_list->fontMetrics().horizontalAdvance(QStringLiteral("888.8 MB")) + 18);
    if (h->sectionSize(MessageListModel::Size) < size) {
        h->resizeSection(MessageListModel::Size, size);
    }
    m_list->doItemsLayout(); // uniform row heights are measured once
}

ListDialog *MainWindow::showListDialog()
{
    auto *dlg = new ListDialog(m_listFontSize, m_listRowSpacing, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &ListDialog::changed, this, [this](int fontSize, int rowSpacing) {
        // Live preview without touching the saved values.
        m_listFontSize = fontSize;
        m_listRowSpacing = rowSpacing;
        applyListAppearance();
    });
    connect(dlg, &ListDialog::finishedWith, this, &MainWindow::setListAppearance);
    dlg->show();
    return dlg;
}

// Scrolling to the bottom loads a folder's next page, but a list that
// doesn't fill the pane can't be scrolled: after deleting what a folder had
// loaded, it showed 2 messages under a count of 212 and never fetched the
// rest (0.5.7). Checked once the view has laid out the change.
void MainWindow::loadMoreIfListIsShort()
{
    QTimer::singleShot(0, this, [this]() {
        if (!(m_live && m_session && m_session->sync() && m_list && m_proxy)) {
            return;
        }
        const QString label = labelForMailbox(m_proxy->mailbox());
        if (label.isEmpty() || m_list->verticalScrollBar()->maximum() > 0 || !m_session->sync()->hasMore(label)) {
            return;
        }
        m_session->sync()->fetchMore(label);
    });
}

StripesDialog *MainWindow::showStripesDialog()
{
    auto *dlg = new StripesDialog(m_stripes, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &StripesDialog::strengthChanged, this, [this](int v) {
        // Live preview without touching the saved value.
        m_stripes = v;
        applyStripes();
    });
    connect(dlg, &StripesDialog::finishedWith, this, &MainWindow::setStripeStrength);
    dlg->show();
    return dlg;
}

// ---- new-mail sound on/off --------------------------------------------------

QAction *MainWindow::soundAction()
{
    if (!m_soundAction) {
        // One checkable action for View > Play Sound for New Mail and the
        // toolbar speaker; remembered in notify/sound (default on).
        m_soundAction = new QAction(tr("Play &Sound for New Mail"), this);
        m_soundAction->setObjectName(QStringLiteral("actionSound"));
        m_soundAction->setCheckable(true);
        m_soundAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        m_soundAction->setIconText(tr("Sound"));
        m_soundAction->setIconVisibleInMenu(false); // a plain check box in View, like Dark Background
        m_sound->setEnabled(QSettings().value(QLatin1String(NewMailSound::kEnabledKey), true).toBool());
        m_soundAction->setChecked(m_sound->isEnabled());
        connect(m_soundAction, &QAction::toggled, this, &MainWindow::setNewMailSoundOn);
        updateSoundAction();
    }
    return m_soundAction;
}

void MainWindow::addSoundButton(QToolBar *tb)
{
    tb->addSeparator();
    tb->addAction(soundAction());
}

bool MainWindow::newMailSoundOn() const
{
    return m_sound->isEnabled();
}

void MainWindow::setNewMailSoundOn(bool on)
{
    m_sound->setEnabled(on);
    QSettings().setValue(QLatin1String(NewMailSound::kEnabledKey), on);
    if (m_soundAction && m_soundAction->isChecked() != on) {
        const QSignalBlocker block(m_soundAction);
        m_soundAction->setChecked(on);
    }
    updateSoundAction();
    statusBar()->showMessage(on ? tr("New-mail sound on.") : tr("New-mail sound muted."), 3000);
}

void MainWindow::updateSoundAction()
{
    const bool on = m_sound->isEnabled();
    const QString name = on ? QStringLiteral("volume-2") : QStringLiteral("volume-x");
    m_soundAction->setProperty("lucide", name); // refreshIcons() keeps it on theme changes
    m_soundAction->setIcon(icon(name));
    const QString key = m_soundAction->shortcut().toString(QKeySequence::NativeText);
    m_soundAction->setToolTip(on ? tr("New-mail sound is on. Click to mute (%1)").arg(key)
                                 : tr("New-mail sound is muted. Click to turn it on (%1)").arg(key));
}

void MainWindow::changeEvent(QEvent *ev)
{
    QMainWindow::changeEvent(ev);
    if (ev->type() == QEvent::ApplicationPaletteChange) {
        applyStripes();
    }
}

void MainWindow::applyStripes()
{
    if (!m_list) {
        return;
    }
    // The application palette with the list's own stripe colour. Redone on
    // every theme/palette change (setTheme, changeEvent).
    const QPalette app = QApplication::palette();
    QPalette p = app;
    const QColor stripe = stripeColor(app, m_stripes);
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::AlternateBase, stripe);
    }
    m_list->setPalette(p);
    // The list's palette would otherwise hide the header's class palette
    // (brand themes colour the column headers; see applyTheme()).
    m_list->header()->setPalette(QApplication::palette(m_list->header()));
    m_list->setAlternatingRowColors(m_stripes > 0);
    m_list->viewport()->update();
}

// ---- list / mailbox actions, Undo Delete ------------------------------------

QString MainWindow::withShortcut(const QString &tip, const QKeySequence &key)
{
    return key.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, key.toString(QKeySequence::NativeText));
}

void MainWindow::installListActions()
{
    // Right-click menus.
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, &MainWindow::showListMenu);
    m_mailboxes->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_mailboxes, &QWidget::customContextMenuRequested, this, &MainWindow::showMailboxMenu);

    // Enter / Return on a row opens it in its own window (as double-click
    // does). Scoped to the list so Enter in the search box still searches.
    for (const QKeySequence &k : {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}) {
        auto *sc = new QShortcut(k, m_list, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this]() { openMessageWindow(m_list->currentIndex()); });
    }
    m_list->setToolTip(QString());
}

QMenu *MainWindow::buildListMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("messageListMenu"));
    for (const char *n : {"actionOpenMessage", "", "menuActionReply", "menuActionReplyAll", "menuActionForward", "",
                          "actionMarkRead", "actionMarkUnread", "", "menuActionJunk", "menuActionNotJunk", "",
                          "menuActionUnsnooze", "", "menuActionDelete"}) {
        if (!*n) {
            menu->addSeparator();
        } else if (QAction *a = findChild<QAction *>(QString::fromLatin1(n))) {
            menu->addAction(a); // the menu-bar actions, so shortcuts show and stay in sync
        }
    }
    if (m_proxy->mailbox() != QLatin1String("Snoozed")) {
        menu->insertMenu(findChild<QAction *>(QStringLiteral("menuActionDelete")), buildSnoozeMenu(menu));
    }
    const QModelIndex cur = m_list->currentIndex();
    if (cur.isValid()) {
        const MailItem &m = m_model->item(m_proxy->mapToSource(cur).row());
        QAction *read = findChild<QAction *>(QStringLiteral("actionMarkRead"));
        QAction *unread = findChild<QAction *>(QStringLiteral("actionMarkUnread"));
        const int nSel = m_list->selectionModel()->selectedRows().size();
        if (nSel > 1) {
            // Mixed selection: offer both so the user can force either state.
            read->setVisible(true);
            unread->setVisible(true);
        } else {
            read->setVisible(m.status == MailStatus::Unread);
            unread->setVisible(m.status != MailStatus::Unread);
        }
        if (nSel == 1 && !m.address.isEmpty()) {
            menu->addSeparator();
            const QString addr = m.address;
            QAction *copy = menu->addAction(icon(QStringLiteral("copy")), tr("Copy Address"), menu,
                                            [addr]() { QGuiApplication::clipboard()->setText(addr); });
            copy->setObjectName(QStringLiteral("actionCopyAddress"));
        }
    }
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        // Back to both visible for the menu bar.
        for (const char *n : {"actionMarkRead", "actionMarkUnread"}) {
            if (QAction *a = findChild<QAction *>(QString::fromLatin1(n))) {
                a->setVisible(true);
            }
        }
        updateMessageActions(); // Junk / Not Junk visibility follows the mailbox
    });
    return menu;
}

void MainWindow::showListMenu(const QPoint &pos)
{
    const QModelIndex at = m_list->indexAt(pos);
    if (!at.isValid()) {
        return;
    }
    QItemSelectionModel *sm = m_list->selectionModel();
    // Right-click outside the selection: select that row alone. On a selected
    // row: keep the multi-selection so Delete / Junk / … apply to all of it.
    if (!sm->isRowSelected(at.row(), QModelIndex())) {
        m_list->setCurrentIndex(at);
    } else if (m_list->currentIndex().row() != at.row()) {
        sm->setCurrentIndex(at, QItemSelectionModel::NoUpdate);
    }
    QMenu *menu = buildListMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_list->viewport()->mapToGlobal(pos));
}
