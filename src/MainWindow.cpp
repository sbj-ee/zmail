#include "MainWindow.hpp"

#include "AboutDialog.hpp"
#include "UpdateChecker.hpp"
#include "ui/ComposeWindow.h"
#include "ui/Icons.h"
#include "ui/MessageListModel.h"
#include "ui/Theme.h"
#include "version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
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

    buildMenus();
    buildToolbar();
    buildPanes();
    buildStatusBar();
    selectMailbox(QStringLiteral("In"));
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
    QAction *cm = file->addAction(tr("&Check Mail"), this, [this]() {
        m_syncLabel->setText(tr("\u25cf Offline sample data \u00b7 sign-in arrives in M1"));
    });
    cm->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
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
    connect(findChild<QAction *>(QStringLiteral("actionCheckMail")), &QAction::triggered, this, [this]() {
        m_syncLabel->setText(tr("\u25cf Offline sample data \u00b7 sign-in arrives in M1"));
    });

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

    m_preview = new QTextBrowser(m_listSplitter);
    m_preview->setObjectName(QStringLiteral("previewPane"));
    m_preview->setOpenLinks(false);

    populateMailboxes();
    connect(m_mailboxes, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) {
            m_proxy->setMailbox(it->data(0, Qt::UserRole).toString());
            updateCounts();
            if (m_proxy->rowCount() > 0) {
                m_list->setCurrentIndex(m_proxy->index(0, 0));
            } else {
                m_preview->clear();
            }
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
                   bool countUnread = true) {
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_mailboxes);
        it->setText(0, name);
        it->setIcon(0, ic);
        it->setData(0, Qt::UserRole, key);
        const int n = key.isEmpty() ? 0 : countFor(key, countUnread);
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
    if (m_proxy->rowCount() > 0) {
        m_list->setCurrentIndex(m_proxy->index(0, 0));
    }
}

void MainWindow::showMessage(const QModelIndex &proxyIndex)
{
    if (!proxyIndex.isValid()) {
        m_preview->clear();
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
