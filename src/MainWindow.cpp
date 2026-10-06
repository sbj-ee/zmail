#include "MainWindow.hpp"

#include "AboutDialog.hpp"
#include "UpdateChecker.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/MessageParser.h"
#include "core/ReplyBuilder.h"
#include "core/Signatures.h"
#include "core/SizeFormat.h"
#include "core/SyncEngine.h"
#include "ui/ComposeWindow.h"
#include "ui/ConnectDialog.h"
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
#include "ui/StripesDialog.h"
#include "ui/Theme.h"
#include "ui/ThemeEditorDialog.h"
#include "version.hpp"

#include <algorithm>
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
#include <QDesktopServices>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
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

QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

ViewMessage liveViewMessage(const zmail::CachedMessage &c, bool loading, const QString &error)
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
} // namespace

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
                                      [this]() { trashMessage(m_shownId); });
    del->setObjectName(QStringLiteral("menuActionDelete"));
    del->setProperty("lucide", QStringLiteral("trash"));
    del->setShortcuts({QKeySequence::Delete});
    message->addSeparator();
    QAction *junk = message->addAction(icon(QStringLiteral("shield-alert")), tr("Mark as &Junk"), this,
                                       [this]() { junkMessage(m_shownId); });
    junk->setObjectName(QStringLiteral("menuActionJunk"));
    junk->setProperty("lucide", QStringLiteral("shield-alert"));
    junk->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    QAction *notJunk = message->addAction(icon(QStringLiteral("mail")), tr("Not &Junk"), this,
                                          [this]() { notJunkMessage(m_shownId); });
    notJunk->setObjectName(QStringLiteral("menuActionNotJunk"));
    notJunk->setProperty("lucide", QStringLiteral("mail"));
    notJunk->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
    notJunk->setVisible(false);
    later(message, tr("S&nooze\u2026"));

    QMenu *settings = addMenu("menuSettings", tr("&Settings"));
    later(settings, tr("&Account\u2026"));
    later(settings, tr("&Rules (Sounds && Colours)\u2026"));
    QAction *sigs = settings->addAction(tr("Si&gnatures\u2026"), this, &MainWindow::showSignatures);
    sigs->setObjectName(QStringLiteral("actionSignatures"));
    QAction *privacy = settings->addAction(tr("&Privacy\u2026"), this, [this]() { showPrivacyDialog()->open(); });
    privacy->setObjectName(QStringLiteral("actionPrivacy"));
    QAction *sounds = settings->addAction(tr("S&ounds\u2026"), this, [this]() { showSoundDialog()->open(); });
    sounds->setObjectName(QStringLiteral("actionSounds"));
    settings->addSeparator();
    m_stripes = stripeStrengthFromSetting(QSettings().value(QStringLiteral("ui/rowStripes")));
    QAction *stripes = settings->addAction(tr("Row S&tripes\u2026"), this, [this]() { showStripesDialog(); });
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
            [this]() { trashMessage(m_shownId); });
    connect(findChild<QAction *>(QStringLiteral("actionJunk")), &QAction::triggered, this, [this]() {
        if (m_proxy->mailbox() == QLatin1String("Junk")) {
            notJunkMessage(m_shownId);
        } else {
            junkMessage(m_shownId);
        }
    });
    addSoundButton(tb);

    auto *spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_search = new QLineEdit(tb);
    m_search->setObjectName(QStringLiteral("searchBox"));
    m_search->setPlaceholderText(tr("Search  (from: subject: has:attachment is:unread \u2026)"));
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
        m_proxy->setSearchText(t);
        updateCounts();
    });
}

void MainWindow::buildPanes()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("mainSplitter"));
    m_splitter->setHandleWidth(5);

    m_mailboxes = new QTreeWidget(m_splitter);
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
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &cur) { showMessage(cur); });

    m_view = new MessageView(m_listSplitter);
    m_view->setObjectName(QStringLiteral("messageView"));
    m_view->body()->setObjectName(QStringLiteral("previewPane"));
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

void MainWindow::populateMailboxes()
{
    const QSignalBlocker block(m_mailboxes);
    m_mailboxes->clear();
    auto countFor = [this](const QString &key, bool unreadOnly) {
        int n = 0;
        const bool hide = hideSpam();
        for (const MailItem &m : m_model->items()) {
            if (hide && key != QLatin1String("Junk") && m.mailboxes.contains(QStringLiteral("Junk"))) {
                continue;
            }
            const bool in = key.startsWith(QLatin1String("label:")) ? m.label == key.mid(6)
                                                                     : m.mailboxes.contains(key);
            if (in && (!unreadOnly || m.status == MailStatus::Unread)) {
                ++n;
            }
        }
        return n;
    };
    const bool showJunkFolder = !hideSpam() || m_proxy->mailbox() == QLatin1String("Junk");
    auto add = [&](QTreeWidgetItem *parent, const QString &name, const QIcon &ic, const QString &key,
                   bool countUnread = true, int forced = -1) {
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_mailboxes);
        it->setText(0, name);
        it->setIcon(0, ic);
        it->setData(0, Qt::UserRole, key);
        const int n = forced >= 0 ? forced : key.isEmpty() ? 0 : countFor(key, countUnread);
        if (n > 0) {
            it->setText(1, QString::number(n));
            it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            if (countUnread) {
                QFont f = m_mailboxes->font();
                f.setBold(true);
                it->setFont(0, f);
                it->setFont(1, f);
            }
        }
        return it;
    };
    const QPalette pal = QApplication::palette();
    if (m_live && m_session->cache()) {
        // Real Gmail labels: system ones map onto Eudora's mailboxes, user
        // labels (nested on "/") go under "Gmail Labels". Unread counts come
        // from Gmail, so they're right even for mail not cached yet.
        QHash<QString, zmail::CachedLabel> byId;
        for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
            byId.insert(l.id, l);
        }
        auto unread = [&](const QString &id) { return byId.contains(id) ? byId.value(id).unread : 0; };
        add(nullptr, tr("In"), icon(QStringLiteral("inbox")), QStringLiteral("In"), true, unread(QStringLiteral("INBOX")));
        QTreeWidgetItem *out = add(nullptr, tr("Out"), icon(QStringLiteral("send")), QStringLiteral("Out"), false, 0);
        out->setToolTip(0, tr("Sent mail (Gmail SENT)"));
        if (showJunkFolder) {
            QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                        icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                        QStringLiteral("Junk"), false, unread(QStringLiteral("SPAM")));
            junk->setForeground(0, suspiciousForeground(pal));
            junk->setToolTip(0, tr("Gmail Spam"));
        }
        add(nullptr, tr("Trash"), icon(QStringLiteral("trash")), QStringLiteral("Trash"), false, 0);

        auto *root = add(nullptr, tr("Gmail Labels"), icon(QStringLiteral("folder-open")), QString());
        root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
        struct S { const char *id; QString name; const char *icon; };
        for (const S &sys : {S{"STARRED", tr("Starred"), "star"}, S{"IMPORTANT", tr("Important"), "flag"},
                             S{"DRAFT", tr("Drafts"), "square-pen"}}) {
            const QString id = QString::fromLatin1(sys.id);
            if (byId.contains(id)) {
                add(root, sys.name, icon(QString::fromLatin1(sys.icon)), QStringLiteral("gmail:") + id, true,
                    id == QLatin1String("DRAFT") ? 0 : unread(id));
            }
        }
        QHash<QString, QTreeWidgetItem *> folders;
        QList<zmail::CachedLabel> user;
        for (const zmail::CachedLabel &l : byId) {
            if (l.type == QLatin1String("user")) {
                user.append(l);
            }
        }
        std::sort(user.begin(), user.end(), [](const auto &a, const auto &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        for (const zmail::CachedLabel &l : user) {
            QTreeWidgetItem *parent = root;
            const QStringList parts = l.name.split(QLatin1Char('/'));
            QString path;
            for (int i = 0; i + 1 < parts.size(); ++i) {
                path += (i ? QStringLiteral("/") : QString()) + parts[i];
                if (!folders.contains(path)) {
                    auto *f = add(parent, parts[i], icon(QStringLiteral("folder")), QString());
                    f->setFlags(f->flags() & ~Qt::ItemIsSelectable);
                    folders.insert(path, f);
                }
                parent = folders.value(path);
            }
            const QColor c = l.color.isEmpty() ? pal.color(QPalette::Mid) : QColor(l.color);
            QTreeWidgetItem *it = add(parent, parts.last(), swatch(c, 14), QStringLiteral("gmail:") + l.id, true, l.unread);
            folders.insert(l.name, it);
        }
        m_mailboxes->expandAll();
        return;
    }
    add(nullptr, tr("In"), icon(QStringLiteral("inbox")), QStringLiteral("In"));
    QTreeWidgetItem *out = add(nullptr, tr("Out"), icon(QStringLiteral("send")), QStringLiteral("Out"), false);
    out->setToolTip(0, tr("Queued and sent mail"));
    if (showJunkFolder) {
        QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                    icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                    QStringLiteral("Junk"), false);
        junk->setForeground(0, suspiciousForeground(pal));
        junk->setToolTip(0, tr("Gmail Spam"));
    }
    add(nullptr, tr("Trash"), icon(QStringLiteral("trash")), QStringLiteral("Trash"), false);

    auto *labels = add(nullptr, tr("Gmail Labels"), icon(QStringLiteral("folder-open")), QString());
    labels->setFlags(labels->flags() & ~Qt::ItemIsSelectable);
    struct L { QString name; QColor color; };
    for (const L &l : {L{tr("Family"), QColor(0x00, 0x89, 0x7b)}, L{tr("Work"), QColor(0x7b, 0x3f, 0xb5)},
                       L{tr("Receipts"), QColor(0xe0, 0x8e, 0x0b)}, L{tr("Travel"), QColor(0x1e, 0x6f, 0xd9)},
                       L{tr("Newsletters"), QColor(0x78, 0x80, 0x88)}}) {
        add(labels, l.name, swatch(l.color, 14), QStringLiteral("label:") + l.name, false);
    }
    m_mailboxes->expandAll();
}

void MainWindow::buildStatusBar()
{
    m_syncLabel = new QLabel(this);
    m_syncLabel->setObjectName(QStringLiteral("syncLabel"));
    m_syncLabel->setText(tr("\u25cf Offline sample data \u00b7 not signed in \u00b7 last sync: never"));
    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("countLabel"));
    statusBar()->addWidget(m_syncLabel, 1);
    statusBar()->addPermanentWidget(m_countLabel);
}

void MainWindow::updateCounts()
{
    int unread = 0;
    const int total = m_proxy->rowCount();
    qint64 bytes = 0;
    for (int r = 0; r < total; ++r) {
        const QModelIndex src = m_proxy->mapToSource(m_proxy->index(r, 0));
        const MailItem &m = m_model->item(src.row());
        unread += m.status == MailStatus::Unread;
        bytes += m.sizeBytes;
    }
    int queued = 0;
    for (const MailItem &m : m_model->items()) {
        queued += m.status == MailStatus::Queued;
    }
    QString box = m_proxy->mailbox();
    if (box.startsWith(QLatin1String("label:"))) {
        box = box.mid(6);
    } else if (box.startsWith(QLatin1String("gmail:")) && m_mailboxes->currentItem()) {
        box = m_mailboxes->currentItem()->text(0);
    }
    m_countLabel->setText(tr("%1: %2 messages, %3 unread, %4  \u00b7  %5 queued ")
                              .arg(box)
                              .arg(total)
                              .arg(unread)
                              .arg(zmail::formatSize(bytes))
                              .arg(queued));
}

void MainWindow::selectMailbox(const QString &key)
{
    const QString prev = m_proxy->mailbox();
    m_proxy->setMailbox(key);
    // Hide Spam omits Junk from the tree unless it is the current mailbox.
    if (hideSpam() && (prev == QLatin1String("Junk")) != (key == QLatin1String("Junk"))) {
        populateMailboxes();
    }
    {
        const QSignalBlocker block(m_mailboxes);
        QTreeWidgetItemIterator it(m_mailboxes);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == key) {
                m_mailboxes->setCurrentItem(*it);
                break;
            }
            ++it;
        }
    }
    updateCounts();
    if (!m_live && m_proxy->rowCount() > 0) {
        m_list->setCurrentIndex(m_proxy->index(0, 0));
    }
    updateMessageActions(); // nothing to delete in an empty mailbox
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
    // empty mailbox), in sample mode too.
    const bool selected = m_list && m_list->currentIndex().isValid() && (!m_live || !m_shownId.isEmpty());
    for (const char *n : {"actionDelete", "menuActionDelete", "actionMarkRead", "actionMarkUnread",
                          "actionJunk", "menuActionJunk", "menuActionNotJunk"}) {
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

void MainWindow::setSession(zmail::MailSession *session)
{
    m_session = session;
    if (!session) {
        return;
    }
    connect(session, &zmail::MailSession::ready, this, [this]() {
        m_live = true;
        attachSync();
        reloadFromCache();
        selectMailbox(QStringLiteral("In"));
        sessionStateChanged();
    });
    connect(session, &zmail::MailSession::stateChanged, this, &MainWindow::sessionStateChanged);
    installSignInBanner();
    connect(session, &zmail::MailSession::reauthRequired, this,
            [this](const QString &reason) { showConnectDialog(reason); });
    sessionStateChanged();
}

void MainWindow::sessionStateChanged()
{
    using State = zmail::MailSession::State;
    const State st = m_session ? m_session->state() : State::NeedsClient;
    if (m_live && st != State::SignedIn) {
        // Signed out (or the token was revoked): back to the sample data.
        m_live = false;
        m_model->setItems(sampleMail());
        populateMailboxes();
        selectMailbox(QStringLiteral("In"));
    }
    if (m_signInAction) {
        m_signInAction->setEnabled(m_session && st != State::SignedIn);
        m_signOutAction->setEnabled(m_session && st == State::SignedIn);
    }
    updateMessageActions();
    updateSyncLabel();
}

void MainWindow::attachSync()
{
    zmail::SyncEngine *sync = m_session->sync();
    if (!sync) {
        return;
    }
    connect(sync, &zmail::SyncEngine::labelsChanged, this, [this]() {
        const QString box = m_proxy->mailbox();
        populateMailboxes();
        const QSignalBlocker block(m_mailboxes);
        QTreeWidgetItemIterator it(m_mailboxes);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == box) {
                m_mailboxes->setCurrentItem(*it);
                break;
            }
            ++it;
        }
    });
    connect(sync, &zmail::SyncEngine::messagesChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    connect(sync, &zmail::SyncEngine::newMail, this, [this](const QStringList &ids) {
        m_sound->play();
        statusBar()->showMessage(tr("%n new message(s)", nullptr, int(ids.size())), 8000);
    });
    connect(sync, &zmail::SyncEngine::statusChanged, this, [this](const QString &s) { updateSyncLabel(s); });
    connect(sync, &zmail::SyncEngine::idle, this, [this]() {
        m_lastSync = QLocale(QLocale::English).toString(QTime::currentTime(), QStringLiteral("h:mm AP"));
        updateSyncLabel();
    });
    connect(sync, &zmail::SyncEngine::syncError, this,
            [this](const QString &e) { statusBar()->showMessage(e, 10000); });
}

QString MainWindow::labelForMailbox(const QString &key) const
{
    if (key == QLatin1String("In")) return QStringLiteral("INBOX");
    if (key == QLatin1String("Out")) return QStringLiteral("SENT");
    if (key == QLatin1String("Junk")) return QStringLiteral("SPAM");
    if (key == QLatin1String("Trash")) return QStringLiteral("TRASH");
    if (key.startsWith(QLatin1String("gmail:"))) return key.mid(6);
    return {};
}

void MainWindow::reloadFromCache()
{
    if (!m_live || !m_session->cache()) {
        return;
    }
    zmail::MailCache *cache = m_session->cache();
    QHash<QString, zmail::CachedLabel> labels;
    for (const zmail::CachedLabel &l : cache->labels()) {
        labels.insert(l.id, l);
    }
    const QPalette pal = QApplication::palette();
    QList<MailItem> items;
    for (const zmail::CachedMessage &c : cache->messages({}, 20000)) {
        MailItem m;
        m.id = c.id;
        const bool sent = c.labels.contains(QStringLiteral("SENT"));
        m.status = c.unread() ? MailStatus::Unread : sent ? MailStatus::Sent : MailStatus::Read;
        m.priority = MailPriority::Normal;
        m.hasAttachment = c.hasAttachment;
        for (const QString &id : c.labels) {
            const auto it = labels.constFind(id);
            if (it != labels.constEnd() && it->type == QLatin1String("user")) {
                m.label = it->name.section(QLatin1Char('/'), -1);
                m.labelColor = it->color.isEmpty() ? pal.color(QPalette::Mid) : QColor(it->color);
                break;
            }
        }
        if (sent && !c.labels.contains(QStringLiteral("INBOX"))) {
            const auto to = zmail::MessageParser::splitAddress(c.to.section(QLatin1Char(','), 0, 0));
            m.who = to.first;
            m.address = to.second;
        } else {
            m.who = c.fromName;
            m.address = c.fromAddr;
        }
        m.to = c.to;
        m.date = c.date().toLocalTime();
        m.sizeBytes = c.size;
        m.subject = c.subject.isEmpty() ? tr("(no subject)") : c.subject;
        m.suspicious = c.labels.contains(QStringLiteral("SPAM"));
        for (const QString &id : c.labels) {
            if (id == QLatin1String("INBOX")) m.mailboxes << QStringLiteral("In");
            else if (id == QLatin1String("SENT")) m.mailboxes << QStringLiteral("Out");
            else if (id == QLatin1String("SPAM")) m.mailboxes << QStringLiteral("Junk");
            else if (id == QLatin1String("TRASH")) m.mailboxes << QStringLiteral("Trash");
            m.mailboxes << QStringLiteral("gmail:") + id;
        }
        m.preview = c.hasBody ? c.bodyText : c.snippet;
        m.attachments = c.attachments;
        items.append(std::move(m));
    }
    const QString keep = m_shownId;
    const int fallbackRow = std::exchange(m_selectRowAfterReload, -1);
    const int scroll = m_list->verticalScrollBar()->value();
    m_model->setItems(std::move(items));
    const int row = keep.isEmpty() ? -1 : m_model->rowForId(keep);
    const QModelIndex pi = row >= 0 ? m_proxy->mapFromSource(m_model->index(row, 0)) : QModelIndex();
    if (pi.isValid()) {
        const QSignalBlocker block(m_list->selectionModel());
        m_list->setCurrentIndex(pi);
        m_shownId = keep;
    } else if (fallbackRow >= 0 && m_proxy->rowCount() > 0) {
        // The neighbour picked by selectPastRemoved() went too: same position.
        m_list->setCurrentIndex(m_proxy->index(std::min(fallbackRow, m_proxy->rowCount() - 1), 0));
    }
    m_list->verticalScrollBar()->setValue(scroll);
    updateCounts();
}

void MainWindow::showLiveMessage(int row)
{
    const MailItem &item = m_model->item(row);
    const QString id = item.id;
    m_shownId = id;
    updateMessageActions();
    zmail::SyncEngine *sync = m_session->sync();
    zmail::MailCache *cache = m_session->cache();
    if (!sync || !cache) {
        return;
    }
    auto render = [this](const zmail::CachedMessage &c, bool loading, const QString &error) {
        m_view->setMessage(liveViewMessage(c, loading, error));
    };

    const zmail::CachedMessage c = cache->message(id);
    render(c, !c.hasBody, {});
    if (!c.hasBody) {
        QPointer<MainWindow> guard(this);
        sync->fetchBody(id, [guard, id, render](const zmail::CachedMessage &full, const QString &err) {
            if (guard && guard->m_shownId == id) {
                render(full, false, err);
            }
        });
    }
    if (c.unread()) {
        sync->markRead(id); // messages.modify removeLabelIds: ["UNREAD"]
        m_model->setStatus(row, MailStatus::Read);
        updateCounts();
    }
}

void MainWindow::checkMail()
{
    if (m_live && m_session->sync()) {
        updateSyncLabel(tr("Checking for new mail\u2026"));
        m_session->sync()->pollNow(true);
    } else if (m_session) {
        showConnectDialog();
    } else {
        m_syncLabel->setText(tr("\u25cf Offline sample data \u00b7 not signed in"));
    }
}

void MainWindow::updateSyncLabel(const QString &status)
{
    if (!m_syncLabel) {
        return;
    }
    using State = zmail::MailSession::State;
    if (!m_session) {
        m_syncLabel->setText(tr("\u25cf Offline sample data \u00b7 not signed in \u00b7 last sync: never"));
        return;
    }
    switch (m_session->state()) {
    case State::NeedsClient:
        m_syncLabel->setText(tr("\u25cf Sample data \u00b7 not connected \u00b7 File \u2192 Sign In to Gmail\u2026 to set up"));
        return;
    case State::SignedOut:
        m_syncLabel->setText(tr("\u25cf Sample data \u00b7 signed out \u00b7 last sync: never"));
        return;
    case State::Restoring:
        m_syncLabel->setText(tr("\u25cf Restoring sign-in\u2026"));
        return;
    case State::SigningIn:
        m_syncLabel->setText(tr("\u25cf Waiting for Google sign-in in your browser\u2026"));
        return;
    case State::SignedIn:
        break;
    }
    QString text = QStringLiteral("\u25cf %1").arg(m_session->account());
    if (!status.isEmpty()) {
        text += QStringLiteral(" \u00b7 ") + status;
    }
    text += QStringLiteral(" \u00b7 ") +
            (m_lastSync.isEmpty() ? tr("last sync: never")
                                  : tr("last sync %1 %2").arg(m_lastSync, QDateTime::currentDateTime().timeZoneAbbreviation()));
    m_syncLabel->setText(text);
}

ConnectDialog *MainWindow::showConnectDialog(const QString &notice)
{
    if (!m_session) {
        return nullptr;
    }
    if (!m_connect) {
        m_connect = new ConnectDialog(m_session, this);
        m_connect->setAttribute(Qt::WA_DeleteOnClose);
    }
    if (!notice.isEmpty()) {
        m_connect->setNotice(notice);
    }
    m_connect->show();
    m_connect->raise();
    m_connect->activateWindow();
    return m_connect;
}

void MainWindow::installSignInBanner()
{
    using State = zmail::MailSession::State;
    connect(m_session, &zmail::MailSession::signInFailed, this, &MainWindow::showSignInBanner);
    connect(m_session, &zmail::MailSession::reauthRequired, this, &MainWindow::showSignInBanner);
    connect(m_session, &zmail::MailSession::stateChanged, this, [this](State s) {
        if (s == State::SigningIn || s == State::SignedIn) {
            hideSignInBanner();
        }
    });
}

void MainWindow::showSignInBanner(const QString &reason)
{
    if (!m_signInBanner) {
        // Its own toolbar row under the main toolbar; not movable or
        // closable from the context menu, hidden until needed.
        addToolBarBreak(Qt::TopToolBarArea);
        m_signInBanner = new QToolBar(tr("Sign-in"), this);
        m_signInBanner->setObjectName(QStringLiteral("signInBanner"));
        m_signInBanner->setMovable(false);
        m_signInBanner->setFloatable(false);
        m_signInBanner->toggleViewAction()->setVisible(false);
        m_signInBanner->setContextMenuPolicy(Qt::PreventContextMenu);
        auto *row = new QWidget(m_signInBanner);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(8, 4, 8, 4);
        h->setSpacing(8);
        auto *ic = new QLabel(row);
        const int px = style()->pixelMetric(QStyle::PM_SmallIconSize);
        ic->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(px, px));
        m_signInBannerText = new QLabel(row);
        m_signInBannerText->setObjectName(QStringLiteral("signInBannerText"));
        m_signInBannerText->setWordWrap(true);
        m_signInBannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *retry = new QPushButton(icon(QStringLiteral("refresh-cw")), tr("&Retry"), row);
        retry->setObjectName(QStringLiteral("signInRetry"));
        retry->setToolTip(tr("Open the Google sign-in page in your browser again"));
        connect(retry, &QPushButton::clicked, this, [this]() {
            hideSignInBanner();
            if (m_session->state() == zmail::MailSession::State::NeedsClient) {
                showConnectDialog();
            } else {
                m_session->signIn();
            }
        });
        auto *dismiss = new QPushButton(tr("Dismiss"), row);
        dismiss->setObjectName(QStringLiteral("signInDismiss"));
        connect(dismiss, &QPushButton::clicked, this, &MainWindow::hideSignInBanner);
        h->addWidget(ic);
        h->addWidget(m_signInBannerText, 1);
        h->addWidget(retry);
        h->addWidget(dismiss);
        m_signInBanner->addWidget(row);
        addToolBar(Qt::TopToolBarArea, m_signInBanner);
    }
    m_signInBannerText->setText(reason);
    m_signInBanner->show();
    statusBar()->showMessage(reason, 15000);
    // Taskbar/dock attention where the platform has it (Wayland: an
    // activation request, never a forced raise). zmail has no desktop
    // notifications, so none is added for this.
    QApplication::alert(this);
}

void MainWindow::hideSignInBanner()
{
    if (m_signInBanner) {
        m_signInBanner->hide();
    }
}

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
        w->setMessage(liveViewMessage(c, !c.hasBody, {}));
        if (!c.hasBody) {
            QPointer<MessageWindow> guard(w);
            sync->fetchBody(id, [guard](const zmail::CachedMessage &full, const QString &err) {
                if (guard) {
                    guard->setMessage(liveViewMessage(full, false, err));
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

void MainWindow::selectPastRemoved(const QStringList &ids)
{
    const QModelIndex cur = m_list->currentIndex();
    if (!cur.isValid() || !ids.contains(m_model->item(m_proxy->mapToSource(cur).row()).id)) {
        return; // e.g. deleted from its own window while another row is selected
    }
    QList<int> rows;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        if (ids.contains(m_model->item(m_proxy->mapToSource(m_proxy->index(r, 0)).row()).id)) {
            rows.append(r);
        }
    }
    // Pick the neighbour by id now, in view order (so sorting holds), and
    // select it before the model refreshes; reloadFromCache() keeps it by id.
    const SelectionAfterRemoval next = selectionAfterRemoval(m_proxy->rowCount(), rows);
    m_selectRowAfterReload = next.rowAfter;
    if (next.rowBefore < 0) {
        m_list->selectionModel()->clear(); // list will be empty: clears the preview
    } else {
        m_list->setCurrentIndex(m_proxy->index(next.rowBefore, 0)); // shows it in the preview
    }
}

void MainWindow::trashMessage(QString id) // by value: callers pass m_shownId, cleared below
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || id.isEmpty()) {
        statusBar()->showMessage(m_live ? tr("Select a message to delete.") : tr("Sign in to Gmail to delete mail."),
                                 5000);
        return;
    }
    selectPastRemoved({id});
    sync->trash(id); // optimistic; rolled back with an error if Gmail refuses
    if (m_shownId == id) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
    }
    offerUndoDelete(id);
}

void MainWindow::junkMessage(QString id)
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || id.isEmpty()) {
        statusBar()->showMessage(
            m_live ? tr("Select a message to mark as Junk.") : tr("Sign in to Gmail to mark Junk."), 5000);
        return;
    }
    sync->markJunk(id);
    if (m_shownId == id) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
    }
    statusBar()->showMessage(tr("Moved to Spam."), 5000);
}

void MainWindow::notJunkMessage(QString id)
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || id.isEmpty()) {
        statusBar()->showMessage(
            m_live ? tr("Select a message to mark as Not Junk.") : tr("Sign in to Gmail to mark Not Junk."), 5000);
        return;
    }
    sync->markNotJunk(id);
    statusBar()->showMessage(tr("Moved out of Spam."), 5000);
}

bool MainWindow::hideSpam() const
{
    return m_proxy && m_proxy->hideSpam();
}

void MainWindow::setHideSpam(bool hide)
{
    QSettings().setValue(QStringLiteral("mail/hideSpam"), hide);
    if (m_proxy) {
        m_proxy->setHideSpam(hide);
    }
    if (m_hideSpamAction && m_hideSpamAction->isChecked() != hide) {
        const QSignalBlocker block(m_hideSpamAction);
        m_hideSpamAction->setChecked(hide);
    }
    const QString box = m_proxy ? m_proxy->mailbox() : QStringLiteral("In");
    populateMailboxes();
    selectMailbox(box);
    updateCounts();
    statusBar()->showMessage(hide ? tr("Spam folder hidden.") : tr("Spam folder shown."), 3000);
}

void MainWindow::showSpamFolder()
{
    selectMailbox(QStringLiteral("Junk"));
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
                          "menuActionDelete"}) {
        if (!*n) {
            menu->addSeparator();
        } else if (QAction *a = findChild<QAction *>(QString::fromLatin1(n))) {
            menu->addAction(a); // the menu-bar actions, so shortcuts show and stay in sync
        }
    }
    const QModelIndex cur = m_list->currentIndex();
    if (cur.isValid()) {
        const MailItem &m = m_model->item(m_proxy->mapToSource(cur).row());
        QAction *read = findChild<QAction *>(QStringLiteral("actionMarkRead"));
        QAction *unread = findChild<QAction *>(QStringLiteral("actionMarkUnread"));
        read->setVisible(m.status == MailStatus::Unread);
        unread->setVisible(m.status != MailStatus::Unread);
        if (!m.address.isEmpty()) {
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
    if (at.row() != m_list->currentIndex().row()) {
        m_list->setCurrentIndex(at); // right-click selects the row, then acts on it
    }
    QMenu *menu = buildListMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_list->viewport()->mapToGlobal(pos));
}

QMenu *MainWindow::buildMailboxMenu(QTreeWidgetItem *item)
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("mailboxMenu"));
    const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!key.isEmpty()) {
        QAction *all = menu->addAction(icon(QStringLiteral("mail-open")), tr("Mark All as &Read"), menu,
                                       [this, key]() { markAllRead(key); });
        all->setObjectName(QStringLiteral("actionMarkAllRead"));
        all->setEnabled(unreadIn(key) > 0);
        menu->addSeparator();
    }
    if (QAction *check = findChild<QAction *>(QStringLiteral("actionCheckMail"))) {
        menu->addAction(check);
    }
    menu->addSeparator();
    menu->addAction(tr("E&xpand All"), m_mailboxes, &QTreeView::expandAll)->setObjectName(QStringLiteral("actionExpandAll"));
    menu->addAction(tr("&Collapse All"), m_mailboxes, &QTreeView::collapseAll)->setObjectName(QStringLiteral("actionCollapseAll"));
    return menu;
}

void MainWindow::showMailboxMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_mailboxes->itemAt(pos);
    if (item && (item->flags() & Qt::ItemIsSelectable) && item != m_mailboxes->currentItem()) {
        m_mailboxes->setCurrentItem(item);
    }
    QMenu *menu = buildMailboxMenu(item);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_mailboxes->viewport()->mapToGlobal(pos));
}

int MainWindow::unreadIn(const QString &key) const
{
    int n = 0;
    for (const MailItem &m : m_model->items()) {
        const bool in = key.startsWith(QLatin1String("label:")) ? m.label == key.mid(6) : m.mailboxes.contains(key);
        n += in && m.status == MailStatus::Unread;
    }
    return n;
}

void MainWindow::markAllRead(const QString &key)
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    int n = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const MailItem &m = m_model->item(row);
        const bool in = key.startsWith(QLatin1String("label:")) ? m.label == key.mid(6) : m.mailboxes.contains(key);
        if (!in || m.status != MailStatus::Unread) {
            continue;
        }
        if (sync) {
            sync->markRead(m.id);
        }
        m_model->setStatus(row, MailStatus::Read);
        ++n;
    }
    if (!sync) {
        // Sample mail: clear the mailbox's unread badge (live mail
        // repopulates from Gmail's counts).
        for (QTreeWidgetItemIterator it(m_mailboxes); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == key) {
                (*it)->setText(1, QString());
                (*it)->setFont(0, m_mailboxes->font());
                (*it)->setFont(1, m_mailboxes->font());
            }
        }
    }
    updateCounts();
    statusBar()->showMessage(n == 1 ? tr("Marked 1 message as read.") : tr("Marked %1 messages as read.").arg(n), 5000);
}

void MainWindow::setCurrentRead(bool read)
{
    const QModelIndex cur = m_list->currentIndex();
    if (!cur.isValid()) {
        return;
    }
    const int row = m_proxy->mapToSource(cur).row();
    const MailItem &m = m_model->item(row);
    if (zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr) {
        read ? sync->markRead(m.id) : sync->markUnread(m.id);
    }
    m_model->setStatus(row, read ? MailStatus::Read : MailStatus::Unread);
    updateCounts();
}

void MainWindow::offerUndoDelete(const QString &id)
{
    if (!m_undoBar) {
        // "Moved to Trash.  [Undo]" at the right of the status bar for a few
        // seconds. Gmail's Trash is recoverable for 30 days anyway; this is
        // the quick way back from a slip of the Delete key.
        m_undoBar = new QWidget(this);
        m_undoBar->setObjectName(QStringLiteral("undoDeleteBar"));
        auto *h = new QHBoxLayout(m_undoBar);
        h->setContentsMargins(0, 0, 6, 0);
        h->setSpacing(6);
        m_undoLabel = new QLabel(m_undoBar);
        m_undoLabel->setObjectName(QStringLiteral("undoDeleteLabel"));
        auto *btn = new QToolButton(m_undoBar);
        btn->setObjectName(QStringLiteral("undoDeleteButton"));
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setText(tr("Undo"));
        connect(btn, &QToolButton::clicked, this, &MainWindow::undoDelete);
        btn->setToolTip(withShortcut(tr("Put the message back"), m_undoDeleteAction->shortcut()));
        h->addWidget(m_undoLabel);
        h->addWidget(btn);
        statusBar()->insertPermanentWidget(0, m_undoBar);
        m_undoTimer = new QTimer(this);
        m_undoTimer->setSingleShot(true);
        connect(m_undoTimer, &QTimer::timeout, this, [this]() {
            m_undoBar->hide();
            m_undoDeleteAction->setEnabled(false);
            m_lastTrashed.clear();
        });
    }
    m_lastTrashed = id;
    m_undoLabel->setText(tr("Moved to Trash."));
    m_undoBar->show();
    m_undoDeleteAction->setEnabled(true);
    m_undoTimer->start(kUndoDeleteMs);
}

void MainWindow::undoDelete()
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    const QString id = m_lastTrashed;
    m_lastTrashed.clear();
    if (m_undoTimer) {
        m_undoTimer->stop();
    }
    if (m_undoBar) {
        m_undoBar->hide();
    }
    m_undoDeleteAction->setEnabled(false);
    if (!sync || id.isEmpty() || !sync->untrash(id)) {
        return;
    }
    statusBar()->showMessage(tr("Message restored."), 4000);
}

