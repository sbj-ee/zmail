#include "MainWindow.hpp"

#include "AboutDialog.hpp"
#include "UpdateChecker.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/MessageParser.h"
#include "core/SyncEngine.h"
#include "ui/ComposeWindow.h"
#include "ui/ConnectDialog.h"
#include "ui/NewMailSound.h"
#include "ui/SafeHtmlView.h"
#include "ui/Icons.h"
#include "ui/MessageListModel.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <algorithm>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QScrollBar>
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
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>

using namespace zmail::ui;

namespace {
QString esc(const QString &s)
{
    return s.toHtmlEscaped();
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

    m_sound = new NewMailSound(this);
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);
    connect(m_reloadTimer, &QTimer::timeout, this, &MainWindow::reloadFromCache);

    buildMenus();
    buildToolbar();
    buildPanes();
    buildStatusBar();
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
        a->setEnabled(false); // placeholder until the feature lands
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

    QMenu *view = addMenu("menuView", tr("&View"));
    later(view, tr("View as &Plain Text"));
    QMenu *theme = view->addMenu(tr("&Theme"));
    theme->setObjectName(QStringLiteral("menuTheme"));
    m_themeGroup = new QActionGroup(this);
    struct T { const char *obj; QString text; ThemeMode mode; };
    for (const T &t : {T{"actionThemeLight", tr("&Light"), ThemeMode::Light},
                       T{"actionThemeDark", tr("&Dark"), ThemeMode::Dark},
                       T{"actionThemeSystem", tr("Follow &System"), ThemeMode::System}}) {
        QAction *a = theme->addAction(t.text);
        a->setObjectName(QString::fromLatin1(t.obj));
        a->setCheckable(true);
        a->setChecked(t.mode == currentTheme());
        m_themeGroup->addAction(a);
        const ThemeMode mode = t.mode;
        connect(a, &QAction::triggered, this, [this, mode]() { setTheme(mode); });
    }

    QMenu *message = addMenu("menuMessage", tr("&Message"));
    later(message, tr("&Reply"), QKeySequence(Qt::CTRL | Qt::Key_R));
    later(message, tr("Reply &All"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    later(message, tr("&Forward"));
    message->addSeparator();
    later(message, tr("Mark as &Suspicious"));
    later(message, tr("S&nooze\u2026"));

    QMenu *settings = addMenu("menuSettings", tr("&Settings"));
    later(settings, tr("&Account\u2026"));
    later(settings, tr("&Rules (Sounds && Colours)\u2026"));
    later(settings, tr("Si&gnatures\u2026"));

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

    struct B { const char *obj; const char *icon; QString text; QString tip; };
    const QList<B> buttons{
        {"actionCheckMail", "refresh-cw", tr("Check Mail"), tr("Check for new mail (Ctrl+M)")},
        {"actionNewMessage", "square-pen", tr("New Message"), tr("Compose a new message (Ctrl+N)")},
        {"actionReply", "reply", tr("Reply"), tr("Reply to sender")},
        {"actionReplyAll", "reply-all", tr("Reply All"), tr("Reply to all recipients")},
        {"actionForward", "forward", tr("Forward"), tr("Forward this message")},
        {"actionDelete", "trash", tr("Delete"), tr("Move to Trash")},
        {"actionAttach", "paperclip", tr("Attach"), tr("New message with an attachment")},
    };
    for (const B &b : buttons) {
        if (qstrcmp(b.obj, "actionDelete") == 0 || qstrcmp(b.obj, "actionReply") == 0) {
            tb->addSeparator();
        }
        QAction *a = tb->addAction(icon(QString::fromLatin1(b.icon)), b.text);
        a->setObjectName(QString::fromLatin1(b.obj));
        a->setToolTip(b.tip);
        a->setProperty("lucide", QString::fromLatin1(b.icon));
    }
    connect(findChild<QAction *>(QStringLiteral("actionNewMessage")), &QAction::triggered, this,
            [this]() { openCompose(); });
    connect(findChild<QAction *>(QStringLiteral("actionReply")), &QAction::triggered, this,
            [this]() { openCompose(true); });
    connect(findChild<QAction *>(QStringLiteral("actionCheckMail")), &QAction::triggered, this,
            &MainWindow::checkMail);

    auto *spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_search = new QLineEdit(tb);
    m_search->setObjectName(QStringLiteral("searchBox"));
    m_search->setPlaceholderText(tr("Search  (from: subject: has:attachment \u2026)"));
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
    m_mailboxes->setStyleSheet(QStringLiteral("QTreeWidget::item{padding:3px 2px;}"));

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
    h->resizeSection(MessageListModel::Size, 52);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &cur) { showMessage(cur); });

    m_preview = new SafeHtmlView(m_listSplitter);
    m_preview->setObjectName(QStringLiteral("previewPane"));
    m_preview->setOpenLinks(false);

    populateMailboxes();
    connect(m_mailboxes, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) {
            m_proxy->setMailbox(it->data(0, Qt::UserRole).toString());
            if (m_live && m_session->sync()) {
                m_session->sync()->ensureLabel(labelForMailbox(m_proxy->mailbox()));
            }
            updateCounts();
            // Live mail: don't auto-open (that would mark the newest message read).
            if (!m_live && m_proxy->rowCount() > 0) {
                m_list->setCurrentIndex(m_proxy->index(0, 0));
            } else {
                m_preview->clear();
                m_shownId.clear();
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

    m_listSplitter->setStretchFactor(0, 3);
    m_listSplitter->setStretchFactor(1, 2);
    m_listSplitter->setSizes({370, 300});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({210, 970});
    setCentralWidget(m_splitter);
}

void MainWindow::populateMailboxes()
{
    const QSignalBlocker block(m_mailboxes);
    m_mailboxes->clear();
    auto countFor = [this](const QString &key, bool unreadOnly) {
        int n = 0;
        for (const MailItem &m : m_model->items()) {
            const bool in = key.startsWith(QLatin1String("label:")) ? m.label == key.mid(6)
                                                                     : m.mailboxes.contains(key);
            if (in && (!unreadOnly || m.status == MailStatus::Unread)) {
                ++n;
            }
        }
        return n;
    };
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
        QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                    icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                    QStringLiteral("Junk"), false, 0);
        junk->setForeground(0, suspiciousForeground(pal));
        junk->setToolTip(0, tr("Gmail Spam"));
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
    QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                QStringLiteral("Junk"), false);
    junk->setForeground(0, suspiciousForeground(pal));
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
    m_countLabel->setText(tr("%1: %2 messages, %3 unread, %4 K  \u00b7  %5 queued ")
                              .arg(box)
                              .arg(total)
                              .arg(unread)
                              .arg(QLocale(QLocale::English).toString((bytes + 1023) / 1024))
                              .arg(queued));
}

void MainWindow::selectMailbox(const QString &key)
{
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
    m_proxy->setMailbox(key);
    updateCounts();
    if (!m_live && m_proxy->rowCount() > 0) {
        m_list->setCurrentIndex(m_proxy->index(0, 0));
    }
}

void MainWindow::showMessage(const QModelIndex &proxyIndex)
{
    if (!proxyIndex.isValid()) {
        m_preview->clear();
        m_shownId.clear();
        return;
    }
    if (m_live) {
        showLiveMessage(m_proxy->mapToSource(proxyIndex).row());
        return;
    }
    const MailItem &m = m_model->item(m_proxy->mapToSource(proxyIndex).row());
    const QPalette pal = QApplication::palette();
    const QString dim = pal.color(QPalette::PlaceholderText).name();
    const QString hdrBg = pal.color(QPalette::AlternateBase).name();
    const QString text = pal.color(QPalette::Text).name();

    QString html = QStringLiteral("<html><body style='color:%1'>").arg(text);
    if (m.suspicious) {
        html += QStringLiteral(
                    "<table width='100%' cellpadding='8' style='background:%1; color:%2'><tr><td>"
                    "<b>\u26a0 This message looks suspicious.</b><br>"
                    "\u2022 The sender's domain <i>contoso-secure-login.example</i> isn't Contoso Bank's.<br>"
                    "\u2022 The link text doesn't match where the link really goes.<br>"
                    "\u2022 Urgent account-verification wording.<br>"
                    "<small>Remote images are blocked. zmail never deletes mail on its own.</small>"
                    "</td></tr></table><p></p>")
                    .arg(suspiciousBackground(pal).name(), suspiciousForeground(pal).name());
    }
    html += QStringLiteral("<table width='100%' cellpadding='3' cellspacing='0' style='background:%1'>").arg(hdrBg);
    auto row = [&](const QString &k, const QString &v) {
        html += QStringLiteral("<tr><td width='76' align='right' style='color:%1'><b>%2</b></td><td>%3</td></tr>")
                    .arg(dim, k, v);
    };
    const bool outgoing = m.mailboxes.contains(QStringLiteral("Out"));
    row(outgoing ? tr("To:") : tr("From:"), QStringLiteral("%1 &lt;%2&gt;").arg(esc(m.who), esc(m.address)));
    row(outgoing ? tr("From:") : tr("To:"), QStringLiteral("Alex Morgan &lt;alex.morgan@example.com&gt;"));
    row(tr("Subject:"), QStringLiteral("<b>%1</b>").arg(esc(m.subject)));
    row(tr("Date:"), esc(QLocale(QLocale::English, QLocale::UnitedStates)
                             .toString(m.date, QStringLiteral("dddd, MMMM d, yyyy h:mm AP")))
                         + QStringLiteral(" CT"));
    if (!m.label.isEmpty()) {
        row(tr("Label:"), QStringLiteral("<span style='color:%1'>\u25a0</span> %2").arg(m.labelColor.name(), esc(m.label)));
    }
    if (!m.attachments.isEmpty()) {
        row(tr("Attached:"), esc(m.attachments.join(QStringLiteral(",  "))));
    }
    html += QStringLiteral("</table><div style='margin:10px 6px'>");
    html += esc(m.preview).replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    html += QStringLiteral("</div></body></html>");
    m_preview->setHtml(html);
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
    if (cur.isValid()) {
        m_list->setCurrentIndex(cur);
    }
    showMessage(m_list->currentIndex());
}

void MainWindow::setTheme(ThemeMode mode)
{
    applyTheme(mode);
    const char *want = mode == ThemeMode::Dark ? "actionThemeDark"
                     : mode == ThemeMode::System ? "actionThemeSystem" : "actionThemeLight";
    for (QAction *a : m_themeGroup->actions()) {
        a->setChecked(a->objectName() == QLatin1String(want));
    }
    refreshIcons();
}

ComposeWindow *MainWindow::openCompose(bool sampleReply)
{
    auto *c = new ComposeWindow(this);
    c->setWindowFlag(Qt::Window, true);
    if (sampleReply) {
        c->loadSampleReply();
    }
    m_composers.append(c);
    c->show();
    return c;
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
    const int scroll = m_list->verticalScrollBar()->value();
    m_model->setItems(std::move(items));
    const int row = keep.isEmpty() ? -1 : m_model->rowForId(keep);
    if (row >= 0) {
        const QModelIndex pi = m_proxy->mapFromSource(m_model->index(row, 0));
        if (pi.isValid()) {
            const QSignalBlocker block(m_list->selectionModel());
            m_list->setCurrentIndex(pi);
            m_shownId = keep;
        }
    }
    m_list->verticalScrollBar()->setValue(scroll);
    updateCounts();
}

void MainWindow::showLiveMessage(int row)
{
    const MailItem &item = m_model->item(row);
    const QString id = item.id;
    m_shownId = id;
    zmail::SyncEngine *sync = m_session->sync();
    zmail::MailCache *cache = m_session->cache();
    if (!sync || !cache) {
        return;
    }
    auto render = [this](const zmail::CachedMessage &c, bool loading, const QString &error) {
        const QPalette pal = QApplication::palette();
        const QString dim = pal.color(QPalette::PlaceholderText).name();
        QString html = QStringLiteral("<html><body style='color:%1'>").arg(pal.color(QPalette::Text).name());
        if (c.labels.contains(QStringLiteral("SPAM"))) {
            html += QStringLiteral("<table width='100%' cellpadding='8' style='background:%1; color:%2'><tr><td>"
                                   "<b>\u26a0 Gmail put this message in Spam.</b> Links and remote images are "
                                   "blocked; check the sender before replying.</td></tr></table><p></p>")
                        .arg(suspiciousBackground(pal).name(), suspiciousForeground(pal).name());
        }
        html += QStringLiteral("<table width='100%' cellpadding='3' cellspacing='0' style='background:%1'>")
                    .arg(pal.color(QPalette::AlternateBase).name());
        auto row = [&](const QString &k, const QString &v) {
            html += QStringLiteral("<tr><td width='76' align='right' style='color:%1'><b>%2</b></td><td>%3</td></tr>")
                        .arg(dim, k, v);
        };
        row(tr("From:"), QStringLiteral("%1 &lt;%2&gt;").arg(esc(c.fromName), esc(c.fromAddr)));
        if (!c.to.isEmpty()) {
            row(tr("To:"), esc(c.to));
        }
        row(tr("Subject:"), QStringLiteral("<b>%1</b>").arg(esc(c.subject.isEmpty() ? tr("(no subject)") : c.subject)));
        const QDateTime dt = c.date().toLocalTime();
        row(tr("Date:"), esc(QLocale(QLocale::English, QLocale::UnitedStates)
                                 .toString(dt, QStringLiteral("dddd, MMMM d, yyyy h:mm AP"))
                             + QLatin1Char(' ') + dt.timeZoneAbbreviation()));
        if (!c.attachments.isEmpty()) {
            row(tr("Attached:"), esc(c.attachments.join(QStringLiteral(",  "))));
        }
        html += QStringLiteral("</table>");
        int blocked = 0;
        QString body;
        if (loading) {
            body = QStringLiteral("<p>%1</p><p style='color:%2'><i>%3</i></p>")
                       .arg(esc(c.snippet), dim, tr("Loading message\u2026"));
        } else if (!c.bodyHtml.isEmpty()) {
            body = SafeHtmlView::sanitize(c.bodyHtml, &blocked);
        } else {
            body = QStringLiteral("<div style='white-space:pre-wrap'>%1</div>")
                       .arg(esc(c.bodyText.isEmpty() ? c.snippet : c.bodyText));
        }
        if (blocked > 0) {
            html += QStringLiteral("<p style='color:%1'><small>%2</small></p>")
                        .arg(dim, tr("%n remote image(s) blocked.", nullptr, blocked));
        }
        if (!error.isEmpty()) {
            html += QStringLiteral("<p style='color:#b3261e'>%1</p>").arg(esc(tr("Couldn't load the message: %1").arg(error)));
        }
        html += QStringLiteral("<div style='margin:10px 6px'>") + body + QStringLiteral("</div></body></html>");
        m_preview->resetBlocked();
        m_preview->setHtml(html);
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
