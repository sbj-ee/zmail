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
#include "ui/Flags.h"
#include "core/ContactStore.h"
#include "core/Sender.h"
#include "core/GmailClient.h"
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
#include "ui/RulesDialog.h"
#include "ui/MailboxWindow.h"
#include "core/Stationery.h"
#include "ui/StationeryDialog.h"
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
#include <QScopedValueRollback>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QItemSelection>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QSaveFile>
#include <QFileDialog>
#include <QTextDocument>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QStackedWidget>
#include <QListWidget>
#include <QAbstractButton>
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

// The message list. Shift+click and Shift+arrows select a range from the
// current message, upwards or downwards. QAbstractItemView keeps its own
// anchor for that, moved only by a click or a key press, so after anything
// else made a message current (Delete moving on to the next one, a search,
// the list reloading) a range started from wherever the last click was.
class MessageListView : public QTreeView
{
public:
    using QTreeView::QTreeView;

protected:
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override
    {
        QTreeView::currentChanged(current, previous);
        if (!m_extending) {
            m_anchor = current;
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const QModelIndex at = indexAt(e->position().toPoint());
        if (e->button() == Qt::LeftButton && e->modifiers() == Qt::ShiftModifier && at.isValid()) {
            selectRangeTo(at);
            e->accept();
            return;
        }
        QTreeView::mousePressEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier) {
            CursorAction action = MoveUp;
            bool move = true;
            switch (e->key()) {
            case Qt::Key_Up: action = MoveUp; break;
            case Qt::Key_Down: action = MoveDown; break;
            case Qt::Key_PageUp: action = MovePageUp; break;
            case Qt::Key_PageDown: action = MovePageDown; break;
            case Qt::Key_Home: action = MoveHome; break;
            case Qt::Key_End: action = MoveEnd; break;
            default: move = false; break;
            }
            if (move && currentIndex().isValid()) {
                const QModelIndex to = moveCursor(action, Qt::NoModifier);
                if (to.isValid()) {
                    selectRangeTo(to);
                }
                e->accept();
                return;
            }
        }
        QTreeView::keyPressEvent(e);
    }

private:
    void selectRangeTo(const QModelIndex &to)
    {
        // The anchor has to be part of what is selected now: if the
        // selection was replaced since (a reload putting it back, Delete
        // moving on), the range starts at the current message.
        if (!m_anchor.isValid() || !selectionModel()->isRowSelected(m_anchor.row(), QModelIndex())) {
            m_anchor = currentIndex().isValid() ? currentIndex() : to;
        }
        const int first = std::min(m_anchor.row(), to.row());
        const int last = std::max(m_anchor.row(), to.row());
        const QScopedValueRollback<bool> extending(m_extending, true);
        selectionModel()->select(QItemSelection(model()->index(first, 0), model()->index(last, 0)),
                                 QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        selectionModel()->setCurrentIndex(model()->index(to.row(), 0), QItemSelectionModel::NoUpdate);
        scrollTo(to);
    }

    QPersistentModelIndex m_anchor; // where a range starts: the last message made current on its own
    bool m_extending = false;
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
            setDropTarget(it);
            e->acceptProposedAction();
        } else {
            setDropTarget(nullptr);
            e->ignore();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent *e) override
    {
        setDropTarget(nullptr);
        QTreeWidget::dragLeaveEvent(e);
    }
    // The folder a drop would go to is lit while the drag is over it.
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (m_dropTarget.isValid() && index.row() == m_dropTarget.row() && index.parent() == m_dropTarget.parent()) {
            QColor fill = palette().color(QPalette::Highlight);
            painter->save();
            painter->setPen(fill);
            fill.setAlphaF(0.30);
            painter->setBrush(fill);
            painter->drawRect(QRect(0, option.rect.top(), viewport()->width() - 1, option.rect.height() - 1));
            painter->restore();
        }
        QTreeWidget::drawRow(painter, option, index);
    }
    void dropEvent(QDropEvent *e) override
    {
        QTreeWidgetItem *it = itemAt(e->position().toPoint());
        const QString key = it ? it->data(0, Qt::UserRole).toString() : QString();
        if (dropLabel(key).isEmpty() || !onDrop) {
            setDropTarget(nullptr);
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
        setDropTarget(nullptr);
        onDrop(dropLabel(key), ids);
    }

private:
    void setDropTarget(QTreeWidgetItem *item)
    {
        const QModelIndex index = item ? indexFromItem(item) : QModelIndex();
        if (index == m_dropTarget) {
            return;
        }
        m_dropTarget = index;
        // For the tests: the key of the row that is lit, or nothing.
        setProperty("dropTarget", item ? item->data(0, Qt::UserRole).toString() : QString());
        viewport()->update();
    }
    QPersistentModelIndex m_dropTarget;

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
    QAction *nm = file->addAction(icon(QStringLiteral("square-pen")), tr("&New Message"), this, [this]() { openCompose(); });
    nm->setShortcut(QKeySequence::New);
    // Filled when opened, from the stationery saved at that moment.
    const auto stationeryMenu = [this](QMenu *menu, bool reply) {
        connect(menu, &QMenu::aboutToShow, this, [this, menu, reply]() {
            menu->clear();
            for (const zmail::Stationery &s : zmail::StationeryStore().all()) {
                const QString name = s.name;
                menu->addAction(name, this, [this, name, reply]() { reply ? replyWith(name) : newMessageWith(name); });
            }
            if (menu->isEmpty()) {
                menu->addAction(tr("(no stationery yet)"))->setEnabled(false);
            }
            menu->addSeparator();
            menu->addAction(tr("Edit Stationery\u2026"), this, [this]() { showStationeryDialog(); });
        });
    };
    QMenu *newWith = file->addMenu(tr("New Message &With"));
    newWith->setObjectName(QStringLiteral("menuNewMessageWith"));
    stationeryMenu(newWith, false);
    QAction *cm = file->addAction(icon(QStringLiteral("refresh-cw")), tr("&Check Mail"), this, &MainWindow::checkMail);
    cm->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    QAction *sq = file->addAction(icon(QStringLiteral("send")), tr("Send &Queued Messages"), this, [this]() { sendQueued(); });
    sq->setObjectName(QStringLiteral("actionSendQueued"));
    sq->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T)); // as in Eudora
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
    QAction *saveAtt = file->addAction(icon(QStringLiteral("paperclip")), tr("&Save Attachments\u2026"), this,
                                       [this]() { saveAttachments(); });
    saveAtt->setObjectName(QStringLiteral("actionSaveAttachments"));
    QAction *print = file->addAction(tr("&Print\u2026"), this, [this]() { printMessage(); });
    print->setObjectName(QStringLiteral("actionPrint"));
    print->setShortcut(QKeySequence::Print);
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *edit = addMenu("menuEdit", tr("&Edit"));
    QAction *copy = edit->addAction(icon(QStringLiteral("copy")), tr("&Copy"), this, &MainWindow::copySelection);
    copy->setObjectName(QStringLiteral("actionCopy"));
    copy->setShortcut(QKeySequence::Copy);
    QAction *selectAll = edit->addAction(tr("Select &All"), this, [this]() {
        // In a text field or the message, its text; otherwise every message in the list.
        QWidget *focus = QApplication::focusWidget();
        if (auto *line = qobject_cast<QLineEdit *>(focus)) {
            line->selectAll();
        } else if (focus && m_view->isAncestorOf(focus)) {
            m_view->body()->selectAll();
        } else {
            m_list->selectAll();
            m_list->setFocus();
        }
    });
    selectAll->setObjectName(QStringLiteral("actionSelectAll"));
    selectAll->setShortcut(QKeySequence::SelectAll);
    QAction *find = edit->addAction(icon(QStringLiteral("search")), tr("&Find\u2026"), this, [this]() { m_search->setFocus(); });
    find->setShortcut(QKeySequence::Find);
    edit->addSeparator();
    m_undoDeleteAction = edit->addAction(tr("&Undo Delete"), this, &MainWindow::undoDelete);
    m_undoDeleteAction->setObjectName(QStringLiteral("actionUndoDelete"));
    m_undoDeleteAction->setShortcut(QKeySequence::Undo);
    m_undoDeleteAction->setEnabled(false);

    QMenu *view = addMenu("menuView", tr("&View"));
    QAction *plain = view->addAction(tr("View as &Plain Text"));
    plain->setObjectName(QStringLiteral("actionPlainText"));
    plain->setCheckable(true);
    plain->setChecked(QSettings().value(QStringLiteral("viewer/plainText"), false).toBool());
    connect(plain, &QAction::toggled, this, &MainWindow::setViewAsPlainText);
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
    m_notify = QSettings().value(QStringLiteral("notify/desktop"), true).toBool();
    QAction *notify = view->addAction(tr("&Notify for New Mail"));
    notify->setObjectName(QStringLiteral("actionNotify"));
    notify->setCheckable(true);
    notify->setChecked(m_notify);
    notify->setToolTip(tr("Show a desktop notification when mail arrives and zmail isn't in front"));
    connect(notify, &QAction::toggled, this, &MainWindow::setNotifyOn);
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

    m_mailboxMenu = addMenu("menuMailbox", tr("Mail&box")); // filled by rebuildMailboxMenus()
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
    QMenu *replyWithMenu = message->addMenu(tr("Reply Wit&h"));
    replyWithMenu->setObjectName(QStringLiteral("menuReplyWith"));
    stationeryMenu(replyWithMenu, true);
    message->addSeparator();
    QAction *markRead = message->addAction(icon(QStringLiteral("mail-open")), tr("Mark as R&ead"), this,
                                           [this]() { setCurrentRead(true); });
    markRead->setObjectName(QStringLiteral("actionMarkRead"));
    markRead->setProperty("lucide", QStringLiteral("mail-open"));
    QAction *markUnread = message->addAction(icon(QStringLiteral("mail")), tr("Mark as &Unread"), this,
                                             [this]() { setCurrentRead(false); });
    markUnread->setObjectName(QStringLiteral("actionMarkUnread"));
    markUnread->setProperty("lucide", QStringLiteral("mail"));
    message->addMenu(buildFlagMenu(message));
    QAction *filterNow = message->addAction(tr("Fi&lter Messages"), this, [this]() { filterSelected(); });
    filterNow->setObjectName(QStringLiteral("actionFilterMessages"));
    // Eudora's key is Ctrl+J, but that was already Mark as Junk here (with
    // Ctrl+Shift+J for Not Junk). Sharing it made Qt treat the key as
    // ambiguous and run neither (0.6.0-0.6.6).
    filterNow->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
    QAction *del = message->addAction(icon(QStringLiteral("trash")), tr("&Delete"), this,
                                      [this]() { trashSelected(); });
    del->setObjectName(QStringLiteral("menuActionDelete"));
    del->setProperty("lucide", QStringLiteral("trash"));
    del->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::CTRL | Qt::Key_D)}); // Ctrl+D as in Eudora
    message->addSeparator();
    QAction *addContact = message->addAction(tr("Add Sender to &Contacts"), this, &MainWindow::addSenderToContacts);
    addContact->setObjectName(QStringLiteral("actionAddSenderToContacts"));
    addContact->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K)); // Eudora's Make Nickname
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

    m_transferMenu = addMenu("menuTransfer", tr("&Transfer"));
    QMenu *settings = addMenu("menuSettings", tr("&Settings"));
    later(settings, tr("&Account\u2026"));
    QAction *all = settings->addAction(tr("&Settings\u2026"), this, [this]() { showSettings(); });
    all->setObjectName(QStringLiteral("actionSettings"));
    all->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settings->addSeparator();
    QAction *filters = settings->addAction(tr("&Filters\u2026"), this, [this]() { showSettings(QStringLiteral("filters")); });
    filters->setObjectName(QStringLiteral("actionFilters"));
    m_rules.load();
    QAction *sigs = settings->addAction(tr("Si&gnatures\u2026"), this, [this]() { showSettings(QStringLiteral("signatures")); });
    sigs->setObjectName(QStringLiteral("actionSignatures"));
    QAction *stat = settings->addAction(tr("S&tationery\u2026"), this, [this]() { showSettings(QStringLiteral("stationery")); });
    stat->setObjectName(QStringLiteral("actionStationery"));
    QAction *privacy = settings->addAction(tr("&Privacy\u2026"), this, [this]() { showSettings(QStringLiteral("privacy")); });
    privacy->setObjectName(QStringLiteral("actionPrivacy"));
    QAction *sounds = settings->addAction(tr("S&ounds\u2026"), this, [this]() { showSettings(QStringLiteral("sounds")); });
    sounds->setObjectName(QStringLiteral("actionSounds"));
    QAction *contacts = settings->addAction(tr("&Contacts…"), this, &MainWindow::showContacts);
    contacts->setObjectName(QStringLiteral("actionContacts"));
    contacts->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L)); // Eudora's address book
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
    QAction *stripes = settings->addAction(tr("Row Stri&pes\u2026"), this, [this]() { showSettings(QStringLiteral("stripes")); });
    QAction *listLook = settings->addAction(tr("Message &List\u2026"), this, [this]() { showSettings(QStringLiteral("list")); });
    listLook->setObjectName(QStringLiteral("actionMessageList"));
    stripes->setObjectName(QStringLiteral("actionRowStripes"));

    QMenu *help = addMenu("menuHelp", tr("&Help"));
    QAction *keys = help->addAction(tr("&Keyboard Shortcuts"), this, [this]() { showShortcuts(); });
    keys->setObjectName(QStringLiteral("actionShortcuts"));
    keys->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    help->addSeparator();
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

    m_list = new MessageListView(m_listSplitter);
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
    // An empty list says so, instead of being a blank rectangle.
    m_emptyHint = new QLabel(m_list->viewport());
    m_emptyHint->setObjectName(QStringLiteral("emptyListHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setForegroundRole(QPalette::PlaceholderText);
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *hintLayout = new QVBoxLayout(m_list->viewport());
    hintLayout->addWidget(m_emptyHint);
    m_emptyHint->hide();
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
    // A click in the flag column flags the message (in the colour used last)
    // or clears its flag.
    connect(m_list, &QTreeView::clicked, this, [this](const QModelIndex &i) {
        if (!i.isValid() || i.column() != MessageListModel::Priority || QApplication::keyboardModifiers() != Qt::NoModifier) {
            return;
        }
        const MailItem &m = m_model->item(m_proxy->mapToSource(i).row());
        const QString last = QSettings().value(QStringLiteral("ui/lastFlag"), QString::fromLatin1(kDefaultFlag)).toString();
        setFlagOnSelected(m.flag.isEmpty() ? (flagOrder(last) ? last : QString::fromLatin1(kDefaultFlag)) : QString());
    });

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
        connect(c, &ComposeWindow::sent, this, [this]() {
            statusBar()->showMessage(tr("Message sent"), 6000);
            reloadFromCache(); // it may have been in the queue
            populateMailboxes();
        });
        connect(c, &ComposeWindow::queued, this, [this]() {
            reloadFromCache();
            populateMailboxes();
            const QString key = findChild<QAction *>(QStringLiteral("actionSendQueued"))->shortcut().toString(QKeySequence::NativeText);
            statusBar()->showMessage(tr("Queued in Out. File \u203a Send Queued Messages (%1) sends it.").arg(key), 8000);
        });
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

// ---- Settings: one window ------------------------------------------------------

namespace {
// A settings dialog living inside the Settings window is a page, not a
// dialog: Escape and Enter belong to the window around it. (Left to the
// page, Escape would reject and hide just that page.)
class PageKeys : public QObject
{
public:
    PageKeys(QDialog *window, QObject *parent) : QObject(parent), m_window(window) {}

protected:
    bool eventFilter(QObject *, QEvent *ev) override
    {
        if (ev->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(ev)->key();
            if (key == Qt::Key_Escape) {
                m_window->reject();
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                return true; // never an accidental OK from inside a page
            }
        }
        return false;
    }

private:
    QDialog *m_window;
};
} // namespace

// Filters, Signatures, Stationery, Message List, Row Stripes, Sounds and
// Privacy were seven dialogs under seven menu entries. Here they are the
// sections of one window: the same dialogs, as pages, with one OK and one
// Cancel for all of them.
QDialog *MainWindow::showSettings(const QString &section)
{
    auto *win = new QDialog(this);
    win->setObjectName(QStringLiteral("settingsWindow"));
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(tr("Settings"));
    win->resize(1040, 660);
    auto *lay = new QVBoxLayout(win);
    auto *body = new QHBoxLayout;
    auto *sections = new QListWidget(win);
    sections->setObjectName(QStringLiteral("settingsSections"));
    sections->setFixedWidth(170);
    auto *stack = new QStackedWidget(win);
    stack->setObjectName(QStringLiteral("settingsPages"));
    body->addWidget(sections);
    body->addWidget(stack, 1);
    lay->addLayout(body, 1);

    struct Page { QString id; QString title; QDialog *dialog; };
    QList<Page> pages;
    {
        const QScopedValueRollback<bool> embedding(m_embedding, true); // built, not shown as windows of their own
        pages.append({QStringLiteral("filters"), tr("Filters"), showRulesDialog()});
        auto *store = new zmail::SignatureStore;
        auto *sigs = new SignaturesDialog(store, this);
        connect(sigs, &QObject::destroyed, this, [store]() { delete store; });
        pages.append({QStringLiteral("signatures"), tr("Signatures"), sigs});
        pages.append({QStringLiteral("stationery"), tr("Stationery"), showStationeryDialog()});
        pages.append({QStringLiteral("list"), tr("Message List"), showListDialog()});
        pages.append({QStringLiteral("stripes"), tr("Row Stripes"), showStripesDialog()});
        pages.append({QStringLiteral("sounds"), tr("Sounds"), showSoundDialog()});
        pages.append({QStringLiteral("privacy"), tr("Privacy"), showPrivacyDialog()});
    }
    QList<QDialogButtonBox *> boxes;
    for (const Page &p : std::as_const(pages)) {
        p.dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        // A short page (two fields and three buttons) sits at the top of its
        // section, not stretched over the whole height.
        const bool small = p.dialog->sizeHint().height() < 320;
        auto *holder = new QWidget(stack);
        auto *hl = new QVBoxLayout(holder);
        hl->setContentsMargins(0, 0, 0, 0);
        p.dialog->setParent(holder); // a child widget now, not a window
        hl->addWidget(p.dialog, small ? 0 : 1);
        if (small) {
            p.dialog->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
            hl->addStretch(1);
        }
        p.dialog->setProperty("settingsSection", p.id);
        p.dialog->installEventFilter(new PageKeys(win, p.dialog));
        // Its own OK / Cancel are hidden; the window's stand in for them.
        const QList<QDialogButtonBox *> own = p.dialog->findChildren<QDialogButtonBox *>(QString(), Qt::FindDirectChildrenOnly);
        QDialogButtonBox *box = own.isEmpty() ? nullptr : own.last();
        if (box) {
            box->hide();
        }
        boxes.append(box);
        // Flush with the window: the page's own margins would double them.
        if (p.dialog->layout()) {
            p.dialog->layout()->setContentsMargins(6, 0, 0, 0);
        }
        stack->addWidget(holder);
        auto *item = new QListWidgetItem(p.title, sections);
        item->setData(Qt::UserRole, p.id);
    }
    connect(sections, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    int start = 0;
    for (int i = 0; i < pages.size(); ++i) {
        if (pages.at(i).id == section) {
            start = i;
        }
    }
    sections->setCurrentRow(start);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, win);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, win, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, win, &QDialog::reject);
    // Every page is told, whichever one is on show: OK keeps them all,
    // Cancel (or closing the window) puts every preview back.
    connect(win, &QDialog::finished, this, [pages, boxes](int result) {
        for (int i = 0; i < pages.size(); ++i) {
            QDialog *page = pages.at(i).dialog;
            QAbstractButton *press = boxes.at(i)
                                         ? boxes.at(i)->button(result == QDialog::Accepted ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
                                         : nullptr;
            if (press) {
                press->click(); // exactly what its own button did when it was a dialog
            } else if (result == QDialog::Accepted) {
                page->accept();
            } else {
                page->reject();
            }
        }
    });
    win->show();
    return win;
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

MessageWindow *MainWindow::openMessageWindowFor(const QString &messageId)
{
    const int row = m_model->rowForId(messageId);
    return row < 0 ? nullptr : openMessageWindow(m_proxy->mapFromSource(m_model->index(row, 0)), row);
}

MessageWindow *MainWindow::openMessageWindow(const QModelIndex &proxyIndex, int sourceRow)
{
    // By source row when given (a mailbox window: the message may not be in
    // the main list's current mailbox, so it has no proxy index here).
    if (!proxyIndex.isValid() && sourceRow < 0) {
        return nullptr;
    }
    const int row = sourceRow >= 0 ? sourceRow : m_proxy->mapToSource(proxyIndex).row();
    if (m_model->item(row).id.startsWith(QLatin1String("queued:"))) {
        editQueued(m_model->item(row).id); // not mail yet: it opens to be changed, not read
        return nullptr;
    }
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
    if (m_shownId.isEmpty() || !m_session || !m_session->cache()) {
        statusBar()->showMessage(m_live ? tr("Select a message first.") : tr("Sign in to Gmail to save contacts."), 5000);
        return;
    }
    const zmail::CachedMessage c = m_session->cache()->message(m_shownId);
    addToContacts(c.fromName, c.fromAddr);
}

void MainWindow::addToContacts(const QString &name, const QString &address)
{
    zmail::ContactStore *store = m_live && m_session ? m_session->contacts() : nullptr;
    if (!store || !store->isOpen()) {
        statusBar()->showMessage(tr("Sign in to Gmail to save contacts."), 5000);
        return;
    }
    if (address.trimmed().isEmpty()) {
        statusBar()->showMessage(tr("This message has no From address."), 5000);
        return;
    }
    if (store->hasEmail(address)) {
        statusBar()->showMessage(tr("%1 is already in Contacts.").arg(address), 5000);
        return;
    }
    store->addLocalContact(name, address);
    statusBar()->showMessage(tr("Added %1 to Contacts (kept on this computer, not uploaded to Google).").arg(address), 5000);
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
    // ...and for "12/31/2026  12:59 PM".
    const int date = m_list->fontMetrics().horizontalAdvance(QStringLiteral("88/88/8888  88:88 PM")) + 18;
    if (h->sectionSize(MessageListModel::Date) < date) {
        h->resizeSection(MessageListModel::Date, date);
    }
    m_list->doItemsLayout(); // uniform row heights are measured once
    for (MailboxWindow *w : mailboxWindows()) {
        styleMailboxWindow(w);
    }
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
    if (!m_embedding) {
        dlg->show();
    }
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
        // If the last page asked for on this mailbox's behalf added nothing
        // to the list, another won't either (an emptied Trash, a folder of
        // hidden spam): stop there instead of paging through all of it.
        const int rows = m_proxy->rowCount();
        if (m_autoLoadMailbox == m_proxy->mailbox() && rows <= m_autoLoadRows) {
            return;
        }
        m_autoLoadMailbox = m_proxy->mailbox();
        m_autoLoadRows = rows;
        m_session->sync()->fetchMore(label);
    });
}

// ---- File > Save Attachments / Print, Edit > Copy, View as Plain Text --------

namespace {
// A name that is safe to create inside `dir`, and not taken: "a.pdf", "a (2).pdf".
QString freeFileName(const QDir &dir, const QString &wanted)
{
    QString name = QFileInfo(wanted).fileName(); // no directories from the sender
    name.remove(QLatin1Char('/'));
    name.remove(QLatin1Char('\\'));
    name.remove(QChar(0));
    while (name.startsWith(QLatin1Char('.'))) {
        name.remove(0, 1); // not a hidden file
    }
    if (name.trimmed().isEmpty()) {
        name = QStringLiteral("attachment");
    }
    const QFileInfo fi(name);
    const QString stem = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString ext = fi.completeBaseName().isEmpty() || fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    QString candidate = name;
    for (int n = 2; dir.exists(candidate); ++n) {
        candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(ext);
    }
    return candidate;
}
} // namespace

void MainWindow::saveAttachments(const QString &folder)
{
    m_savedAttachments.clear();
    if (!(m_live && m_session && m_session->api()) || m_shownId.isEmpty()) {
        statusBar()->showMessage(m_live ? tr("Select a message first.") : tr("Sign in to Gmail to save attachments."), 5000);
        return;
    }
    QString dir = folder;
    if (dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(
            this, tr("Save Attachments To"),
            QSettings().value(QStringLiteral("attachments/folder"),
                              QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString());
        if (dir.isEmpty()) {
            return;
        }
        QSettings().setValue(QStringLiteral("attachments/folder"), dir);
    }
    const QString id = m_shownId;
    zmail::GmailClient *api = m_session->api();
    statusBar()->showMessage(tr("Fetching attachments\u2026"));
    api->getMessageFull(id, [this, api, id, dir](const QJsonObject &json, const zmail::ApiError &err) {
        if (err.isError) {
            statusBar()->showMessage(tr("Couldn't fetch the attachments: %1").arg(err.message), 8000);
            return;
        }
        const QList<zmail::MessageParser::AttachmentRef> refs = zmail::MessageParser::bodyFromFull(json).attachmentRefs;
        if (refs.isEmpty()) {
            statusBar()->showMessage(tr("This message has no attachments."), 5000);
            return;
        }
        auto left = std::make_shared<int>(int(refs.size()));
        auto failed = std::make_shared<int>(0);
        const auto write = [this, dir, left, failed](const QString &name, const QByteArray &data, bool ok) {
            if (ok) {
                const QDir d(dir);
                const QString file = d.filePath(freeFileName(d, name));
                QSaveFile out(file);
                if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit()) {
                    m_savedAttachments << file;
                } else {
                    ++*failed;
                }
            } else {
                ++*failed;
            }
            if (--*left > 0) {
                return;
            }
            const int saved = int(m_savedAttachments.size());
            QString text = saved == 1 ? tr("Saved 1 attachment to %1.").arg(QDir::toNativeSeparators(dir))
                                      : tr("Saved %1 attachments to %2.").arg(saved).arg(QDir::toNativeSeparators(dir));
            if (*failed > 0) {
                text += QLatin1Char(' ') + (*failed == 1 ? tr("1 could not be saved.") : tr("%1 could not be saved.").arg(*failed));
            }
            statusBar()->showMessage(text, 8000);
        };
        for (const auto &ref : refs) {
            if (ref.attachmentId.isEmpty()) {
                write(ref.fileName, ref.inlineData, true);
                continue;
            }
            api->getAttachment(id, ref.attachmentId, [this, ref, write](const QJsonObject &a, const zmail::ApiError &e) {
                write(ref.fileName, zmail::MessageParser::decodeBase64Url(a.value(QStringLiteral("data")).toString()), !e.isError);
            });
        }
    });
}

bool MainWindow::printMessage(QPrinter *printer)
{
    if (m_view->message().id.isEmpty() && m_view->body()->document()->isEmpty()) {
        statusBar()->showMessage(tr("Select a message to print."), 5000);
        return false;
    }
    QPrinter own(QPrinter::HighResolution);
    if (!printer) {
        QPrintDialog dlg(&own, this);
        dlg.setWindowTitle(tr("Print Message"));
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        printer = &own;
    }
    const std::unique_ptr<QTextDocument> doc(m_view->printableDocument());
    doc->print(printer);
    statusBar()->showMessage(tr("Sent to the printer."), 4000);
    return true;
}

void MainWindow::copySelection()
{
    // What is selected where the cursor is: a text field, the message...
    if (auto *edit = qobject_cast<QLineEdit *>(QApplication::focusWidget()); edit && edit->hasSelectedText()) {
        edit->copy();
        return;
    }
    if (m_view->body()->textCursor().hasSelection()) {
        m_view->body()->copy();
        return;
    }
    // ...or else the selected messages, a line each.
    QStringList lines;
    for (int row : selectedSourceRows()) {
        const MailItem &m = m_model->item(row);
        lines << QStringLiteral("%1\t%2\t%3").arg(m.who, MessageListModel::formatDate(m.date).simplified(), m.subject);
    }
    if (lines.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    statusBar()->showMessage(lines.size() == 1 ? tr("Copied 1 message's sender, date and subject.")
                                               : tr("Copied %1 messages' senders, dates and subjects.").arg(lines.size()),
                             4000);
}

void MainWindow::setViewAsPlainText(bool on)
{
    m_view->setPlainText(on);
    for (MessageWindow *w : messageWindows()) {
        w->view()->setPlainText(on);
    }
    if (QAction *a = findChild<QAction *>(QStringLiteral("actionPlainText")); a && a->isChecked() != on) {
        const QSignalBlocker block(a);
        a->setChecked(on);
    }
}

// ---- stationery ----------------------------------------------------------------

ComposeWindow *MainWindow::newMessageWith(const QString &stationeryName)
{
    ComposeWindow *c = openCompose();
    const zmail::Stationery s = zmail::StationeryStore().find(stationeryName);
    if (c && !s.name.isEmpty()) {
        c->setStationery(s);
    }
    return c;
}

ComposeWindow *MainWindow::replyWith(const QString &stationeryName)
{
    ComposeWindow *c = composeReply(int(zmail::ReplyBuilder::Kind::Reply));
    if (!c) {
        statusBar()->showMessage(tr("Select a message to reply to."), 5000);
        return nullptr;
    }
    const zmail::Stationery s = zmail::StationeryStore().find(stationeryName);
    if (!s.name.isEmpty()) {
        c->setStationery(s); // applied again once the original has been fetched and quoted
    }
    return c;
}

StationeryDialog *MainWindow::showStationeryDialog()
{
    auto *dlg = new StationeryDialog(zmail::StationeryStore().all(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg]() {
        if (!zmail::StationeryStore().setAll(dlg->items())) {
            statusBar()->showMessage(tr("Couldn't save the stationery to %1.").arg(zmail::StationeryStore::defaultPath()), 8000);
        }
    });
    if (!m_embedding) {
        dlg->show();
    }
    return dlg;
}

// ---- Help > Keyboard Shortcuts ---------------------------------------------------

// Every key the menus have, read from the menus themselves so the list
// can't fall out of date, plus the compose window's.
QDialog *MainWindow::showShortcuts()
{
    auto *dlg = new QDialog(this);
    dlg->setObjectName(QStringLiteral("shortcutsDialog"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Keyboard Shortcuts"));
    dlg->resize(520, 620);
    auto *lay = new QVBoxLayout(dlg);
    auto *tree = new QTreeWidget(dlg);
    tree->setObjectName(QStringLiteral("shortcutsList"));
    tree->setHeaderLabels({tr("Command"), tr("Key")});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lay->addWidget(tree);
    const auto heading = [tree](const QString &text) {
        auto *item = new QTreeWidgetItem(tree, {text});
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setFirstColumnSpanned(true);
    };
    const auto row = [tree](const QString &command, const QString &key) {
        new QTreeWidgetItem(tree, {QStringLiteral("    ") + command, key});
    };
    const auto keysOf = [](const QAction *a) {
        QStringList keys;
        for (const QKeySequence &k : a->shortcuts()) {
            if (!k.isEmpty()) {
                keys << k.toString(QKeySequence::NativeText);
            }
        }
        return keys.join(QStringLiteral(", "));
    };
    std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &path) {
        for (QAction *a : menu->actions()) {
            if (a->isSeparator() || a->property("placeholder").toBool()) {
                continue;
            }
            QString text = a->text();
            text.remove(QLatin1Char('&'));
            text.remove(QStringLiteral("\u2026"));
            if (a->menu()) {
                walk(a->menu(), path + text + QStringLiteral(" \u203a "));
            } else if (!keysOf(a).isEmpty()) {
                row(path + text, keysOf(a));
            }
        }
    };
    for (QAction *top : menuBar()->actions()) {
        if (!top->menu()) {
            continue;
        }
        const int before = tree->topLevelItemCount();
        QString title = top->text();
        title.remove(QLatin1Char('&'));
        heading(title);
        walk(top->menu(), QString());
        if (tree->topLevelItemCount() == before + 1) {
            delete tree->takeTopLevelItem(before); // a menu with no keys: no heading either
        }
    }
    heading(tr("Message list"));
    row(tr("Select a range"), tr("Shift+Click, Shift+Up / Down"));
    row(tr("Add or remove one message"), tr("Ctrl+Click"));
    row(tr("Zoom the message"), tr("Ctrl+Wheel"));
    heading(tr("Writing a message"));
    row(tr("Send"), tr("Ctrl+Enter, Ctrl+E"));
    row(tr("Send Later (queue in Out)"), tr("Ctrl+Shift+Enter"));
    row(tr("Save Draft"), QKeySequence(QKeySequence::Save).toString(QKeySequence::NativeText));
    row(tr("Attach"), tr("Ctrl+H"));
    row(tr("Insert link"), tr("Ctrl+K"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    lay->addWidget(buttons);
    dlg->show();
    return dlg;
}

// ---- mailbox windows -----------------------------------------------------------

QList<MailboxWindow *> MainWindow::mailboxWindows() const
{
    QList<MailboxWindow *> out;
    for (const QPointer<MailboxWindow> &w : m_mailboxWindows) {
        if (w) {
            out << w.data();
        }
    }
    return out;
}

void MainWindow::styleMailboxWindow(MailboxWindow *w)
{
    QTreeView *list = w->list();
    QFont f;
    if (m_listFontSize != kListFontDefault) {
        f.setPointSize(m_listFontSize);
    }
    list->setFont(f);
    list->setProperty("rowSpacing", m_listRowSpacing);
    list->setPalette(m_list->palette()); // the stripe colour
    list->setAlternatingRowColors(m_stripes > 0);
    list->doItemsLayout();
}

void MainWindow::showMessageIn(MessageView *view, const QString &id)
{
    const int row = m_model->rowForId(id);
    if (!view || row < 0) {
        return;
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    zmail::MailCache *cache = m_live && m_session ? m_session->cache() : nullptr;
    if (!sync || !cache) {
        view->setMessage(sampleViewMessage(row));
        return;
    }
    if (id.startsWith(QLatin1String("queued:"))) {
        const zmail::MailCache::QueuedMessage q = cache->queuedMessage(id.mid(7).toLongLong());
        ViewMessage v;
        v.id = id;
        v.from = m_session->fromHeader();
        v.to = q.to;
        v.cc = q.cc;
        v.subject = q.subject;
        v.date = QDateTime::fromMSecsSinceEpoch(q.createdMs);
        v.bodyText = q.text;
        v.warning = tr("<b>Queued.</b> Double-click to change it.");
        view->setMessage(v);
        return;
    }
    const zmail::CachedMessage c = cache->message(id);
    view->setMessage(detail::liveViewMessage(c, !c.hasBody, {}));
    if (!c.hasBody) {
        QPointer<MessageView> guard(view);
        sync->fetchBody(id, [guard, id](const zmail::CachedMessage &full, const QString &err) {
            if (guard && guard->message().id == id) { // still the one on show
                guard->setMessage(detail::liveViewMessage(full, false, err));
            }
        });
    }
    if (c.unread()) {
        sync->markRead(id);
        m_model->setStatus(row, MailStatus::Read);
        updateCounts();
    }
}

QMenu *MainWindow::buildMenuFor(const QStringList &ids, QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("mailboxWindowMenu"));
    // While this menu is up, "the selected messages" are these: the Flag and
    // Transfer submenus and Mark as Read are the main window's own commands.
    m_actOnIds = ids;
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        QTimer::singleShot(0, this, [this]() { m_actOnIds.clear(); }); // after the chosen command has run
    });
    const QString first = ids.value(0);
    const bool one = ids.size() == 1;
    const bool queued = first.startsWith(QLatin1String("queued:"));
    QAction *open = menu->addAction(queued ? tr("&Edit") : tr("&Open"), this, [this, first]() { openMessageWindowFor(first); });
    open->setObjectName(QStringLiteral("mwOpen"));
    open->setEnabled(one);
    if (!queued) {
        menu->addSeparator();
        struct R { const char *obj; const char *iconName; QString text; zmail::ReplyBuilder::Kind kind; };
        for (const R &r : {R{"mwReply", "reply", tr("&Reply"), zmail::ReplyBuilder::Kind::Reply},
                           R{"mwReplyAll", "reply-all", tr("Reply &All"), zmail::ReplyBuilder::Kind::ReplyAll},
                           R{"mwForward", "forward", tr("&Forward"), zmail::ReplyBuilder::Kind::Forward}}) {
            const int kind = int(r.kind);
            QAction *a = menu->addAction(icon(QString::fromLatin1(r.iconName)), r.text, this,
                                         [this, kind, first]() { composeReply(kind, first); });
            a->setObjectName(QString::fromLatin1(r.obj));
            a->setEnabled(one);
        }
        menu->addSeparator();
        menu->addAction(icon(QStringLiteral("mail-open")), tr("Mark as R&ead"), this, [this]() { setCurrentRead(true); })
            ->setObjectName(QStringLiteral("mwMarkRead"));
        menu->addAction(icon(QStringLiteral("mail")), tr("Mark as &Unread"), this, [this]() { setCurrentRead(false); })
            ->setObjectName(QStringLiteral("mwMarkUnread"));
        menu->addMenu(buildFlagMenu(menu));
        if (m_transferMenu && !m_transferMenu->isEmpty()) {
            menu->addMenu(m_transferMenu);
        }
    }
    menu->addSeparator();
    menu->addAction(icon(QStringLiteral("trash")), tr("&Delete"), this, [this, ids]() { trashMessages(ids); })
        ->setObjectName(QStringLiteral("mwDelete"));
    return menu;
}

MailboxWindow *MainWindow::openMailboxWindow(const QString &keyIn)
{
    const QString key = keyIn.isEmpty() ? m_proxy->mailbox() : keyIn;
    if (key.isEmpty() || key == QLatin1String("Search")) {
        statusBar()->showMessage(tr("Choose a mailbox to open in its own window."), 5000);
        return nullptr;
    }
    // One window per mailbox: asking again brings it forward.
    for (MailboxWindow *open : mailboxWindows()) {
        if (open->mailbox() == key) {
            open->raise();
            open->activateWindow();
            return open;
        }
    }
    QString title = key;
    for (QTreeWidgetItemIterator it(m_mailboxes); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == key) {
            const QString full = (*it)->data(0, Qt::UserRole + 2).toString(); // "Alerts/Monitoring"
            title = full.isEmpty() ? (*it)->text(0) : full;
        }
    }
    auto *w = new MailboxWindow(m_model, key, title, this);
    w->setWindowFlag(Qt::Window, true);
    w->proxy()->setHideSpam(hideSpam());
    w->list()->setItemDelegate(new MessageRowDelegate(w->list()));
    w->list()->header()->restoreState(m_list->header()->saveState()); // the main list's columns
    styleMailboxWindow(w);
    m_mailboxWindows.removeAll(nullptr);
    m_mailboxWindows.append(w);
    connect(w, &MailboxWindow::openRequested, this, [this](const QString &id) { openMessageWindowFor(id); });
    connect(w, &MailboxWindow::deleteRequested, this, [this](const QStringList &ids) { trashMessages(ids); });
    connect(w, &MailboxWindow::showRequested, this, [this, w](const QString &id) { showMessageIn(w->preview(), id); });
    connect(w, &MailboxWindow::menuRequested, this, [this, w](const QStringList &ids, const QPoint &at) {
        QMenu *menu = buildMenuFor(ids, w);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(at);
    });
    connect(w->preview(), &MessageView::mailtoRequested, this, [this](const QUrl &u) { composeMailto(u); });
    const QString label = labelForMailbox(key);
    if (m_live && m_session && m_session->sync() && !label.isEmpty()) {
        m_session->sync()->ensureLabel(label); // its first page, if it was never opened
        connect(w, &MailboxWindow::moreRequested, this, [this, label]() {
            if (m_live && m_session && m_session->sync()) {
                m_session->sync()->fetchMore(label);
            }
        });
    }
    w->show();
    return w;
}

// ---- Mailbox and Transfer menus -----------------------------------------------

void MainWindow::openMailbox(const QString &key)
{
    for (QTreeWidgetItemIterator it(m_mailboxes); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == key) {
            m_mailboxes->setCurrentItem(*it); // as a click on it: loads the folder too
            m_list->setFocus();
            return;
        }
    }
    selectMailbox(key);
}

void MainWindow::transferSelected(const QString &target)
{
    if (target == QLatin1String("TRASH")) {
        trashSelected();
        return;
    }
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    if (ids.isEmpty()) {
        statusBar()->showMessage(tr("Select a message to transfer."), 5000);
        return;
    }
    if (!(m_live && m_session && m_session->sync())) {
        statusBar()->showMessage(tr("Sign in to Gmail to move mail."), 5000);
        return;
    }
    selectPastRemoved(ids); // the list moves on to the next message, as after Delete
    moveMessagesToLabel(ids, target);
}

// Rebuilt whenever the sidebar is: the same mailboxes, in the same order and
// nesting. Mailbox goes to one; Transfer moves the selection to one (In, a
// folder, or Trash).
void MainWindow::rebuildMailboxMenus()
{
    if (!m_mailboxMenu || !m_transferMenu || !m_mailboxes) {
        return;
    }
    m_mailboxMenu->clear();
    m_transferMenu->clear();
    QAction *own = m_mailboxMenu->addAction(tr("Open in &New Window"), this, [this]() { openMailboxWindow(); });
    own->setObjectName(QStringLiteral("actionMailboxWindow"));
    own->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    own->setToolTip(tr("Open the current mailbox in a window of its own"));
    m_mailboxMenu->addSeparator();
    const auto target = [](const QString &key) -> QString {
        if (key == QLatin1String("In")) {
            return QStringLiteral("INBOX");
        }
        if (key == QLatin1String("Trash")) {
            return QStringLiteral("TRASH");
        }
        if (key.startsWith(QLatin1String("gmail:Label_"))) {
            return key.mid(6);
        }
        return {};
    };
    std::function<void(QTreeWidgetItem *, QMenu *, QMenu *)> add = [&](QTreeWidgetItem *item, QMenu *go, QMenu *move) {
        const QString key = item->data(0, Qt::UserRole).toString();
        const QString text = item->text(0);
        const QString to = target(key);
        const auto goAction = [&](QMenu *menu) {
            QAction *a = menu->addAction(item->icon(0), text, this, [this, key]() { openMailbox(key); });
            a->setObjectName(QStringLiteral("mailbox_") + key);
            if (key == QLatin1String("In")) {
                a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1)); // as in Eudora
            } else if (key == QLatin1String("Out")) {
                a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2)); // Eudora's Ctrl+0 resets the zoom here
            }
        };
        const auto moveAction = [&](QMenu *menu) {
            QAction *a = menu->addAction(item->icon(0), text, this, [this, to]() { transferSelected(to); });
            a->setObjectName(QStringLiteral("transfer_") + to);
        };
        if (item->childCount() == 0) {
            if (!key.isEmpty()) {
                goAction(go);
            }
            if (!to.isEmpty()) {
                moveAction(move);
            }
            return;
        }
        // A folder of folders: a submenu, with the folder itself on top if it holds mail too.
        QMenu *goSub = go->addMenu(item->icon(0), text);
        QMenu *moveSub = move->addMenu(item->icon(0), text);
        if (!key.isEmpty()) {
            goAction(goSub);
            goSub->addSeparator();
        }
        if (!to.isEmpty()) {
            moveAction(moveSub);
            moveSub->addSeparator();
        }
        for (int i = 0; i < item->childCount(); ++i) {
            add(item->child(i), goSub, moveSub);
        }
        if (moveSub->isEmpty()) {
            move->removeAction(moveSub->menuAction()); // nothing in there takes mail
            moveSub->deleteLater();
        }
    };
    for (int i = 0; i < m_mailboxes->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = m_mailboxes->topLevelItem(i);
        if (top->data(0, Qt::UserRole + 1).toString() == QLatin1String("labels-root")) {
            // The folders themselves, not a "Folders" submenu to open first.
            m_mailboxMenu->addSeparator();
            m_transferMenu->addSeparator();
            for (int c = 0; c < top->childCount(); ++c) {
                add(top->child(c), m_mailboxMenu, m_transferMenu);
            }
            continue;
        }
        add(top, m_mailboxMenu, m_transferMenu);
    }
}

// ---- the queue (Send Later / Send Queued Messages) ---------------------------

int MainWindow::queuedCount() const
{
    return m_live && m_session && m_session->cache() ? int(m_session->cache()->queued().size()) : 0;
}

ComposeWindow *MainWindow::editQueued(const QString &queuedRowId)
{
    if (!(m_live && m_session && m_session->cache()) || !queuedRowId.startsWith(QLatin1String("queued:"))) {
        return nullptr;
    }
    const qint64 id = queuedRowId.mid(7).toLongLong();
    for (ComposeWindow *open : std::as_const(m_composers)) {
        if (open && open->queuedId() == id) { // already being edited
            open->raise();
            open->activateWindow();
            return open;
        }
    }
    const zmail::MailCache::QueuedMessage q = m_session->cache()->queuedMessage(id);
    if (q.id == 0) {
        return nullptr;
    }
    if (q.state.isEmpty()) {
        statusBar()->showMessage(tr("This message was queued by an older zmail and can't be reopened. Delete it and write it again."), 8000);
        return nullptr;
    }
    ComposeWindow *c = openCompose();
    if (!c->restoreState(q.state)) {
        statusBar()->showMessage(tr("Couldn't reopen the queued message."), 8000);
        c->close();
        return nullptr;
    }
    c->setQueuedId(id);
    return c;
}

void MainWindow::sendQueued()
{
    if (!(m_live && m_session && m_session->cache() && m_session->sender())) {
        statusBar()->showMessage(tr("Sign in to Gmail to send queued mail."), 5000);
        return;
    }
    if (m_sendingQueue) {
        return;
    }
    QList<qint64> ids;
    for (const zmail::MailCache::QueuedMessage &q : m_session->cache()->queued()) {
        ids.append(q.id);
    }
    if (ids.isEmpty()) {
        statusBar()->showMessage(tr("Nothing is queued."), 5000);
        return;
    }
    m_sendingQueue = true;
    sendNextQueued(0, 0, ids);
}

// One at a time, in the order they were queued.
void MainWindow::sendNextQueued(int sent, int failed, QList<qint64> left)
{
    if (left.isEmpty() || !m_session || !m_session->cache() || !m_session->sender()) {
        m_sendingQueue = false;
        QString text = sent == 1 ? tr("Sent 1 queued message.") : tr("Sent %1 queued messages.").arg(sent);
        if (failed > 0) {
            text += QLatin1Char(' ') + (failed == 1 ? tr("1 could not be sent and is still in Out.")
                                                    : tr("%1 could not be sent and are still in Out.").arg(failed));
        }
        statusBar()->showMessage(text, 8000);
        reloadFromCache();
        populateMailboxes();
        if (sent > 0 && m_session) {
            m_session->syncSoon(); // they show up as sent
        }
        return;
    }
    const qint64 id = left.takeFirst();
    const zmail::MailCache::QueuedMessage q = m_session->cache()->queuedMessage(id);
    if (q.id == 0) { // deleted from the queue meanwhile
        sendNextQueued(sent, failed, left);
        return;
    }
    statusBar()->showMessage(tr("Sending queued mail\u2026 %1 to go").arg(left.size() + 1));
    QPointer<MainWindow> guard(this);
    zmail::Sender *sender = m_session->sender();
    sender->send(q.mime, q.threadId, [guard, sender, id, q, sent, failed, left](const zmail::Sender::Result &r) {
        if (!guard || !guard->m_session || !guard->m_session->cache()) {
            return;
        }
        if (r.ok) {
            guard->m_session->cache()->removeQueued(id);
            if (!q.draftId.isEmpty()) {
                sender->deleteDraft(q.draftId, [](const zmail::Sender::Result &) {});
            }
            guard->sendNextQueued(sent + 1, failed, left);
        } else {
            guard->m_session->cache()->setQueuedError(
                id, r.err.message.isEmpty() ? tr("HTTP %1").arg(r.err.httpStatus) : r.err.message);
            guard->sendNextQueued(sent, failed + 1, left);
        }
    });
}

// ---- filters ----------------------------------------------------------------

void MainWindow::setRules(const QList<zmail::Rule> &rules)
{
    m_rules.rules = rules;
    if (!m_rules.save()) {
        statusBar()->showMessage(tr("Couldn't save the filters to %1.").arg(zmail::Rules::defaultPath()), 8000);
    }
    reloadFromCache(); // row colours
}

RulesDialog *MainWindow::showRulesDialog(const zmail::Rule *add)
{
    QList<RulesDialog::Folder> folders;
    if (m_live && m_session && m_session->cache()) {
        for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
            if (l.type == QLatin1String("user")) {
                folders.append({l.id, l.name});
            }
        }
        std::sort(folders.begin(), folders.end(),
                  [](const auto &a, const auto &b) { return a.second.compare(b.second, Qt::CaseInsensitive) < 0; });
    }
    auto *dlg = new RulesDialog(m_rules.rules, folders, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    if (add) {
        dlg->addRule(*add);
    }
    connect(dlg, &RulesDialog::previewSound, this, [this](const QString &sound) {
        if (sound == QLatin1String(zmail::kRuleSoundNone)) {
            return;
        }
        sound.isEmpty() ? m_sound->playPreview() : m_sound->playFile(sound, /*preview=*/true);
    });
    connect(dlg, &QDialog::accepted, this, [this, dlg]() { setRules(dlg->rules()); });
    if (!m_embedding) {
        dlg->show();
    }
    return dlg;
}

bool MainWindow::applyRule(const zmail::Rule &rule, const QString &messageId)
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || messageId.isEmpty() || !rule.hasArrivalActions()) {
        return false;
    }
    if (!rule.flag.isEmpty()) {
        sync->setFlag(messageId, rule.flag);
    }
    if (rule.markRead) {
        sync->markRead(messageId);
    }
    if (!rule.moveTo.isEmpty()) {
        sync->moveToLabel(messageId, rule.moveTo);
    }
    return true;
}

namespace {
zmail::RuleMessage ruleMessage(const zmail::CachedMessage &c)
{
    zmail::RuleMessage m;
    m.from = c.fromName.isEmpty() ? c.fromAddr : QStringLiteral("%1 <%2>").arg(c.fromName, c.fromAddr);
    m.to = c.to;
    m.subject = c.subject;
    return m;
}
} // namespace

void MainWindow::applyRulesToNewMail(const QStringList &ids)
{
    // One sound for the batch: the first message a filter has a say about
    // decides; otherwise the usual one.
    QString sound;
    if (m_live && m_session && m_session->cache()) {
        for (const QString &id : ids) {
            const zmail::CachedMessage c = m_session->cache()->summary(id);
            if (c.id.isEmpty()) {
                continue;
            }
            const zmail::Rule *rule = m_rules.match(ruleMessage(c));
            if (!rule) {
                continue;
            }
            if (sound.isEmpty()) {
                sound = rule->sound;
            }
            applyRule(*rule, id);
        }
    }
    if (sound == QLatin1String(zmail::kRuleSoundNone)) {
        return; // a filter asked for quiet: no sound, no notification
    }
    sound.isEmpty() ? m_sound->play() : m_sound->playFile(sound);
    notifyNewMail(ids);
}

void MainWindow::setNotifyOn(bool on)
{
    m_notify = on;
    QSettings().setValue(QStringLiteral("notify/desktop"), on);
    if (QAction *a = findChild<QAction *>(QStringLiteral("actionNotify")); a && a->isChecked() != on) {
        const QSignalBlocker block(a);
        a->setChecked(on);
    }
}

void MainWindow::notifyNewMail(const QStringList &ids)
{
    // Not when you are looking at zmail already.
    if (!m_notify || ids.isEmpty() || QApplication::activeWindow()) {
        return;
    }
    QStringList lines;
    if (m_live && m_session && m_session->cache()) {
        for (const QString &id : ids.mid(0, 3)) {
            const zmail::CachedMessage c = m_session->cache()->summary(id);
            if (!c.id.isEmpty()) {
                lines << QStringLiteral("%1: %2").arg(c.fromName.isEmpty() ? c.fromAddr : c.fromName,
                                                      c.subject.isEmpty() ? tr("(no subject)") : c.subject);
            }
        }
    }
    if (ids.size() > 3) {
        lines << tr("and %1 more").arg(ids.size() - 3);
    }
    const QString summary = ids.size() == 1 ? tr("1 new message") : tr("%1 new messages").arg(ids.size());
    if (m_notifySink) {
        m_notifySink(summary, lines.join(QLatin1Char('\n')));
        return;
    }
    // Never from a test run or a headless one: this is the user's real desktop.
    if (QStandardPaths::isTestModeEnabled() || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        return;
    }
    // org.freedesktop.Notifications.Notify, sent without waiting for an answer.
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                       QStringLiteral("/org/freedesktop/Notifications"),
                                                       QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    call << QStringLiteral("zmail") << uint(0) << QStringLiteral("zmail") << summary << lines.join(QLatin1Char('\n'))
         << QStringList() << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("zmail")}} << int(-1);
    QDBusConnection::sessionBus().asyncCall(call);
}

void MainWindow::updateTitle()
{
    int unread = 0;
    if (m_live) {
        for (const MailItem &m : m_model->items()) {
            unread += m.status == MailStatus::Unread && m.mailboxes.contains(QStringLiteral("In")) &&
                      !m.mailboxes.contains(QStringLiteral("Trash")) && !m.mailboxes.contains(QStringLiteral("Junk"));
        }
    }
    const QString title = unread > 0 ? QStringLiteral("(%1) %2").arg(unread).arg(baseTitle()) : baseTitle();
    if (windowTitle() != title) {
        setWindowTitle(title);
    }
}

void MainWindow::filterSelected()
{
    if (!(m_live && m_session && m_session->cache())) {
        statusBar()->showMessage(tr("Sign in to Gmail to filter messages."), 5000);
        return;
    }
    int changed = 0;
    for (const QString &id : selectedMessageIds()) {
        const zmail::CachedMessage c = m_session->cache()->summary(id);
        const zmail::Rule *rule = c.id.isEmpty() ? nullptr : m_rules.match(ruleMessage(c));
        if (rule && applyRule(*rule, id)) {
            ++changed;
        }
    }
    statusBar()->showMessage(changed ? (changed == 1 ? tr("Filtered 1 message.") : tr("Filtered %1 messages.").arg(changed)) : tr("No filter had anything to do."),
                             5000);
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
    if (!m_embedding) {
        dlg->show();
    }
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
    for (MailboxWindow *w : mailboxWindows()) {
        styleMailboxWindow(w);
    }
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
    menu->insertMenu(findChild<QAction *>(QStringLiteral("menuActionDelete")), buildFlagMenu(menu));
    if (m_transferMenu && !m_transferMenu->isEmpty()) {
        menu->insertMenu(findChild<QAction *>(QStringLiteral("menuActionDelete")), m_transferMenu); // the menu bar's own
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
            // Offered only for a sender who isn't a contact yet (a hidden
            // contact is still a contact).
            zmail::ContactStore *contacts = m_live && m_session ? m_session->contacts() : nullptr;
            if (contacts && contacts->isOpen() && !contacts->hasEmail(m.address)) {
                const QString name = m.who;
                const QString address = m.address;
                QAction *add = menu->addAction(tr("Add %1 to Contacts").arg(address), menu,
                                               [this, name, address]() { addToContacts(name, address); });
                add->setObjectName(QStringLiteral("actionAddToContacts"));
            }
            {
                // Make Filter: a new rule for this sender, ready to finish.
                zmail::Rule fromSender;
                fromSender.name = m.who.isEmpty() ? m.address : m.who;
                fromSender.conditions.append({QStringLiteral("from"), QStringLiteral("contains"), m.address});
                QAction *make = menu->addAction(tr("Make Filter\u2026"), menu,
                                                [this, fromSender]() { showRulesDialog(&fromSender); });
                make->setObjectName(QStringLiteral("actionMakeFilter"));
            }
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
