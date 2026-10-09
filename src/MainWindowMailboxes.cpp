// MainWindow: the mailbox tree — building it, counts, selecting a mailbox,
// its context menu and the Gmail label (folder) actions.
#include <QDateTime>
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
#include "core/SnoozeTimes.h"
#include "ui/ComposeWindow.h"
#include "ui/ConnectDialog.h"
#include "ui/ContactsWindow.h"
#include "ui/MailboxWindow.h"
#include "ui/Flags.h"
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
#include <QHash>
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
#include "MainWindowDetail.h"

void MainWindow::populateMailboxes()
{
    const QSignalBlocker block(m_mailboxes);
    m_mailboxes->clear();
    // Folder-style counts: total messages in the mailbox/label (not unread).
    // Unread emphasis (bold name) is kept when Gmail reports unread > 0.
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
    const QPalette palEarly = QApplication::palette();
    auto add = [&](QTreeWidgetItem *parent, const QString &name, const QIcon &ic, const QString &key,
                   int forcedTotal = -1, int unreadHint = 0) {
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_mailboxes);
        it->setText(0, name);
        it->setIcon(0, ic);
        it->setData(0, Qt::UserRole, key);
        const int n = forcedTotal >= 0 ? forcedTotal : key.isEmpty() ? 0 : countFor(key, false);
        setMailboxCount(it, n, unreadHint, m_mailboxes->font(), palEarly.color(QPalette::PlaceholderText));
        return it;
    };
    const QPalette pal = QApplication::palette();
    if (m_live && m_session->cache()) {
        // Real Gmail labels: system ones map onto Eudora's mailboxes, user
        // labels (nested on "/") go under "Folders".
        // messagesTotal comes from labels.get (list omits counts on real Gmail).
        QHash<QString, zmail::CachedLabel> byId;
        for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
            byId.insert(l.id, l);
        }
        auto total = [&](const QString &id) { return byId.contains(id) ? byId.value(id).total : 0; };
        auto unread = [&](const QString &id) { return byId.contains(id) ? byId.value(id).unread : 0; };
        add(nullptr, tr("In"), icon(QStringLiteral("inbox")), QStringLiteral("In"), total(QStringLiteral("INBOX")),
            unread(QStringLiteral("INBOX")));
        const int waiting = int(m_session->cache()->queued().size());
        QTreeWidgetItem *out = add(nullptr, tr("Out"), icon(QStringLiteral("send")), QStringLiteral("Out"),
                                   total(QStringLiteral("SENT")) + waiting, waiting);
        out->setToolTip(0, waiting == 0   ? tr("Sent mail (Gmail SENT)")
                           : waiting == 1 ? tr("Sent mail, and 1 message queued to send")
                                          : tr("Sent mail, and %1 messages queued to send").arg(waiting));
        add(nullptr, tr("Snoozed"), icon(QStringLiteral("clock")), QStringLiteral("Snoozed"),
            m_session->cache() ? int(m_session->cache()->snoozes(true).size()) : 0);
        if (showJunkFolder) {
            QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                        icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                        QStringLiteral("Junk"), total(QStringLiteral("SPAM")),
                                        unread(QStringLiteral("SPAM")));
            junk->setForeground(0, suspiciousForeground(pal));
            junk->setToolTip(0, tr("Gmail Spam"));
        }
        if (m_proxy->mailbox() == QLatin1String("Search") || !m_proxy->searchIds().isEmpty()
            || (m_search && !m_search->text().trimmed().isEmpty())) {
            add(nullptr, tr("Search"), icon(QStringLiteral("search")), QStringLiteral("Search"));
        }
        // Less what Empty Trash has removed from zmail (Gmail still counts those).
        add(nullptr, tr("Trash"), icon(QStringLiteral("trash")), QStringLiteral("Trash"),
            std::max(0, total(QStringLiteral("TRASH")) - int(m_session->cache()->purged().size())));

        auto *root = add(nullptr, tr("Folders"), icon(QStringLiteral("folder-open")), QString());
        root->setFlags(root->flags() & ~Qt::ItemIsSelectable);
        root->setData(0, Qt::UserRole + 1, QStringLiteral("labels-root"));
        struct S { const char *id; QString name; const char *icon; };
        for (const S &sys : {S{"STARRED", tr("Flagged"), "flag"}, S{"IMPORTANT", tr("Important"), "star"},
                             S{"DRAFT", tr("Drafts"), "square-pen"}}) {
            const QString id = QString::fromLatin1(sys.id);
            if (byId.contains(id)) {
                add(root, sys.name, icon(QString::fromLatin1(sys.icon)), QStringLiteral("gmail:") + id,
                    total(id), unread(id));
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
                    f->setData(0, Qt::UserRole + 1, QStringLiteral("folder-prefix:") + path);
                    folders.insert(path, f);
                }
                parent = folders.value(path);
            }
            const QColor c = l.color.isEmpty() ? pal.color(QPalette::Mid) : QColor(l.color);
            QTreeWidgetItem *it = add(parent, parts.last(), swatch(c, 14), QStringLiteral("gmail:") + l.id, l.total,
                                      l.unread);
            it->setData(0, Qt::UserRole + 2, l.name); // full Gmail label name for rename
            it->setToolTip(0, tr("Folder · drag mail here to move (leaves Inbox)"));
            folders.insert(l.name, it);
        }
        m_mailboxes->expandAll();
        rebuildMailboxMenus();
        return;
    }
    add(nullptr, tr("In"), icon(QStringLiteral("inbox")), QStringLiteral("In"), -1, countFor(QStringLiteral("In"), true));
    QTreeWidgetItem *out = add(nullptr, tr("Out"), icon(QStringLiteral("send")), QStringLiteral("Out"));
    out->setToolTip(0, tr("Queued and sent mail"));
    add(nullptr, tr("Snoozed"), icon(QStringLiteral("clock")), QStringLiteral("Snoozed"));
    if (showJunkFolder) {
        QTreeWidgetItem *junk = add(nullptr, tr("Junk / Suspicious"),
                                    icon(QStringLiteral("shield-alert"), suspiciousForeground(pal)),
                                    QStringLiteral("Junk"));
        junk->setForeground(0, suspiciousForeground(pal));
        junk->setToolTip(0, tr("Gmail Spam"));
    }
    if (m_proxy->mailbox() == QLatin1String("Search") || !m_proxy->searchIds().isEmpty()
        || (m_search && !m_search->text().trimmed().isEmpty())) {
        add(nullptr, tr("Search"), icon(QStringLiteral("search")), QStringLiteral("Search"));
    }
    add(nullptr, tr("Trash"), icon(QStringLiteral("trash")), QStringLiteral("Trash"));

    auto *labels = add(nullptr, tr("Folders"), icon(QStringLiteral("folder-open")), QString());
    labels->setFlags(labels->flags() & ~Qt::ItemIsSelectable);
    labels->setData(0, Qt::UserRole + 1, QStringLiteral("labels-root"));
    struct L { QString name; QColor color; };
    for (const L &l : {L{tr("Family"), QColor(0x00, 0x89, 0x7b)}, L{tr("Work"), QColor(0x7b, 0x3f, 0xb5)},
                       L{tr("Receipts"), QColor(0xe0, 0x8e, 0x0b)}, L{tr("Travel"), QColor(0x1e, 0x6f, 0xd9)},
                       L{tr("Newsletters"), QColor(0x78, 0x80, 0x88)}}) {
        add(labels, l.name, swatch(l.color, 14), QStringLiteral("label:") + l.name);
    }
    m_mailboxes->expandAll();
    rebuildMailboxMenus();
}

// "3 / 212": unread of total, in bold while something is unread; "212" when
// all of it has been read; nothing for an empty mailbox.
void MainWindow::setMailboxCount(QTreeWidgetItem *item, int total, int unread, const QFont &base, const QColor &dim)
{
    unread = std::clamp(unread, 0, std::max(0, total));
    item->setText(1, total <= 0 ? QString() : unread > 0 ? QStringLiteral("%1 / %2").arg(unread).arg(total) : QString::number(total));
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    QFont f = base;
    f.setBold(unread > 0);
    item->setFont(0, f);
    item->setFont(1, f);
    item->setForeground(1, unread > 0 ? QBrush() : QBrush(dim));
    if (!item->data(0, Qt::UserRole).toString().isEmpty()) {
        item->setToolTip(1, total <= 0     ? tr("No messages")
                            : unread > 0 ? tr("%1 unread of %2").arg(unread).arg(total)
                                         : (total == 1 ? tr("1 message, read") : tr("%1 messages, all read").arg(total)));
    }
}

void MainWindow::updateMailboxCounts()
{
    if (!(m_live && m_session && m_session->cache()) || !m_mailboxes) {
        return; // the sample mailboxes count their own rows when they are built
    }
    QHash<QString, zmail::CachedLabel> byId;
    for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
        byId.insert(l.id, l);
    }
    const QColor dim = QApplication::palette().color(QPalette::PlaceholderText);
    const int queued = int(m_session->cache()->queued().size());
    const int purged = int(m_session->cache()->purged().size());
    for (QTreeWidgetItemIterator it(m_mailboxes); *it; ++it) {
        const QString key = (*it)->data(0, Qt::UserRole).toString();
        if (key == QLatin1String("Snoozed")) {
            setMailboxCount(*it, int(m_session->cache()->snoozes(true).size()), 0, m_mailboxes->font(), dim);
            continue;
        }
        const QString label = labelForMailbox(key);
        if (label.isEmpty() || !byId.contains(label)) {
            continue; // headings, Search
        }
        const zmail::CachedLabel &l = byId[label];
        int total = l.total;
        int unread = l.unread;
        if (key == QLatin1String("Out")) {
            total += queued; // waiting to go
            unread = queued;
        } else if (key == QLatin1String("Trash")) {
            total = std::max(0, total - purged); // less what Empty Trash removed from zmail
            unread = 0;
        }
        setMailboxCount(*it, total, unread, m_mailboxes->font(), dim);
    }
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
    updateTitle();
    if (m_emptyHint) {
        const QString key = m_proxy->mailbox();
        const bool searching = key == QLatin1String("Search") || (m_search && !m_search->text().trimmed().isEmpty());
        m_emptyHint->setText(searching                          ? tr("No messages match your search.")
                             : key == QLatin1String("Trash")    ? tr("Trash is empty.")
                             : key == QLatin1String("Out")      ? tr("Nothing sent or queued.")
                             : key == QLatin1String("Snoozed")  ? tr("Nothing is snoozed.")
                             : key == QLatin1String("Junk")     ? tr("No junk mail.")
                                                                : tr("No messages in %1.").arg(box));
        m_emptyHint->setVisible(total == 0);
    }
    // "queued" only when something is.
    QString text = tr("%1: %2, %3 unread, %4")
                       .arg(box, total == 1 ? tr("1 message") : tr("%1 messages").arg(total))
                       .arg(unread)
                       .arg(zmail::formatSize(bytes));
    if (queued > 0) {
        text += QStringLiteral("  \u00b7  ") + tr("%1 queued").arg(queued);
    }
    m_countLabel->setText(text + QLatin1Char(' '));
}

void MainWindow::selectMailbox(const QString &key)
{
    const QString prev = m_proxy->mailbox();
    if (prev != key) {
        m_autoLoadMailbox.clear(); // a fresh look at this mailbox may load more
        m_autoLoadRows = -1;
    }
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

QString MainWindow::labelForMailbox(const QString &key) const
{
    if (key == QLatin1String("In")) return QStringLiteral("INBOX");
    if (key == QLatin1String("Out")) return QStringLiteral("SENT");
    if (key == QLatin1String("Junk")) return QStringLiteral("SPAM");
    if (key == QLatin1String("Trash")) return QStringLiteral("TRASH");
    if (key.startsWith(QLatin1String("gmail:"))) return key.mid(6);
    return {};
}

QMenu *MainWindow::buildMailboxMenu(QTreeWidgetItem *item)
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("mailboxMenu"));
    const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
    const QString meta = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    const bool live = m_live && m_session && m_session->sync();
    const bool userLabel = key.startsWith(QLatin1String("gmail:")) && key.mid(6).startsWith(QLatin1String("Label_"));
    const bool labelsRoot = meta == QLatin1String("labels-root");
    const bool folderPrefix = meta.startsWith(QLatin1String("folder-prefix:"));

    if (live && (userLabel || labelsRoot || folderPrefix)) {
        QString prefix;
        if (folderPrefix) {
            prefix = meta.mid(QStringLiteral("folder-prefix:").size()) + QLatin1Char('/');
        } else if (userLabel) {
            const QString full = item->data(0, Qt::UserRole + 2).toString();
            prefix = full.isEmpty() ? QString() : full + QLatin1Char('/');
        }
        QAction *neu = menu->addAction(icon(QStringLiteral("folder")), tr("&New Folder…"), menu, [this, prefix]() {
            newLabelFolder(prefix);
        });
        neu->setObjectName(QStringLiteral("actionNewFolder"));
        if (userLabel) {
            const QString id = key.mid(6);
            const QString fullName = item->data(0, Qt::UserRole + 2).toString();
            QAction *ren = menu->addAction(tr("&Rename Folder…"), menu, [this, id, fullName]() {
                renameLabelFolder(id, fullName);
            });
            ren->setObjectName(QStringLiteral("actionRenameFolder"));
            const QString shownName = fullName.isEmpty() ? item->text(0) : fullName;
            QAction *empty = menu->addAction(tr("&Empty Folder…"), menu,
                                             [this, id, shownName]() { emptyLabelFolder(id, shownName); });
            empty->setObjectName(QStringLiteral("actionEmptyFolder"));
            QAction *del = menu->addAction(icon(QStringLiteral("trash")), tr("&Delete Folder…"), menu,
                                           [this, id, shownName]() { deleteLabelFolder(id, shownName); }); // not `item`: the tree may be rebuilt before the click
            del->setObjectName(QStringLiteral("actionDeleteFolder"));
        } else if (labelsRoot || folderPrefix) {
            // New Folder is enough on the group headers.
        }
        menu->addSeparator();
    }

    if (!key.isEmpty() && key != QLatin1String("Search")) {
        QAction *own = menu->addAction(tr("Open in New &Window"), menu, [this, key]() { openMailboxWindow(key); });
        own->setObjectName(QStringLiteral("actionOpenMailboxWindow"));
        menu->addSeparator();
    }
    if (live && !labelForMailbox(key).isEmpty()) {
        const bool loading = !m_session->sync()->loadingAll().isEmpty();
        QAction *all = menu->addAction(loading ? tr("Stop &Loading") : tr("&Load All Messages"), menu,
                                       [this, key]() { loadAllMessages(key); });
        all->setObjectName(QStringLiteral("actionLoadAllHere"));
        all->setToolTip(tr("Fetch every message in this mailbox, not only the ones scrolled to so far"));
    }
    if (live && key == QLatin1String("Trash")) {
        QAction *empty = menu->addAction(icon(QStringLiteral("trash")), tr("&Empty Trash\u2026"), menu,
                                         [this]() { emptyTrash(); });
        empty->setObjectName(QStringLiteral("actionEmptyTrash"));
        menu->addSeparator();
    }
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
    // The menu is about the row that was clicked. It doesn't go there: you
    // keep your place, and can empty the Trash or mark a folder read from
    // wherever you are. (It used to select the row first, which changed the
    // mailbox on show and could rebuild the tree under the menu.)
    QTreeWidgetItem *item = m_mailboxes->itemAt(pos);
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
        // Sample mail: totals stay; only clear unread emphasis (bold name).
        for (QTreeWidgetItemIterator it(m_mailboxes); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == key) {
                (*it)->setFont(0, m_mailboxes->font());
                (*it)->setFont(1, m_mailboxes->font());
            }
        }
    }
    updateCounts();
    statusBar()->showMessage(n == 1 ? tr("Marked 1 message as read.") : tr("Marked %1 messages as read.").arg(n), 5000);
}


void MainWindow::newLabelFolder(const QString &namePrefix)
{
    if (!(m_live && m_session && m_session->sync())) {
        return;
    }
    bool ok = false;
    const QString suggested = namePrefix.isEmpty() ? tr("New Folder") : namePrefix + tr("New Folder");
    const QString name = QInputDialog::getText(this, tr("New Folder"),
                                               tr("Folder name (use / for nested folders):"),
                                               QLineEdit::Normal, suggested, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    m_session->sync()->createLabel(name);
    statusBar()->showMessage(tr("Creating folder \"%1\"\u2026").arg(name), 4000);
}

void MainWindow::renameLabelFolder(const QString &labelId, const QString &currentName)
{
    if (!(m_live && m_session && m_session->sync()) || labelId.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Folder"),
                                               tr("Folder name (use / for nested folders):"),
                                               QLineEdit::Normal, currentName, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == currentName) {
        return;
    }
    m_session->sync()->renameLabel(labelId, name);
    statusBar()->showMessage(tr("Renaming folder\u2026"), 4000);
}

void MainWindow::deleteLabelFolder(const QString &labelId, const QString &displayName)
{
    if (!(m_live && m_session && m_session->sync()) || labelId.isEmpty()) {
        return;
    }
    const auto choice = QMessageBox::question(
        this, tr("Delete Folder"),
        tr("Delete the folder \"%1\"?\n\n"
           "Only the folder (Gmail label) is removed. Messages in it are not "
           "deleted or moved to Trash — they keep any other labels and stay in "
           "All Mail / Inbox as before.")
            .arg(displayName),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) {
        return;
    }
    // If this folder is selected, leave it before it disappears.
    if (m_proxy->mailbox() == QLatin1String("gmail:") + labelId) {
        selectMailbox(QStringLiteral("In"));
    }
    m_session->sync()->deleteLabel(labelId);
    statusBar()->showMessage(tr("Deleting folder \"%1\"\u2026").arg(displayName), 4000);
}

void MainWindow::emptyLabelFolder(const QString &labelId, const QString &displayName)
{
    if (!(m_live && m_session && m_session->sync()) || labelId.isEmpty()) {
        return;
    }
    int total = 0;
    for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
        if (l.id == labelId) {
            total = l.total;
        }
    }
    const auto choice = QMessageBox::question(
        this, tr("Empty Folder"),
        tr("Move every message in \"%1\" to Trash?\n\n"
           "%2 The folder itself stays. Undo puts them back for a few seconds; "
           "after that they are in Trash, where Gmail keeps them for 30 days.")
            .arg(displayName, total == 1 ? tr("It holds 1 message.") : tr("It holds %1 messages.").arg(total)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) {
        return;
    }
    if (m_proxy->mailbox() == QLatin1String("gmail:") + labelId) {
        m_view->clear(); // the message on show is about to go
    }
    m_session->sync()->emptyLabel(labelId);
    statusBar()->showMessage(tr("Emptying folder \"%1\"\u2026").arg(displayName), 4000);
}

void MainWindow::emptyTrash(bool confirm)
{
    if (!(m_live && m_session && m_session->sync())) {
        return;
    }
    if (confirm) {
        const auto choice = QMessageBox::question(
            this, tr("Empty Trash"),
            tr("Remove every message in Trash?\n\n"
               "They disappear from zmail (Undo brings them back for a few seconds). Gmail doesn't let zmail "
               "erase mail outright, so Gmail itself keeps them in its Trash until it purges them (up to 30 days)."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice != QMessageBox::Yes) {
            return;
        }
    }
    if (m_proxy->mailbox() == QLatin1String("Trash")) {
        m_view->clear();
    }
    m_purgedBeforeEmpty = m_session->cache()->purged(); // for Undo
    m_session->sync()->emptyTrash();
    statusBar()->showMessage(tr("Emptying the Trash\u2026"), 4000);
}

void MainWindow::setFlagOnSelected(const QString &color)
{
    if (!(m_live && m_session && m_session->sync())) {
        statusBar()->showMessage(tr("Sign in to Gmail to flag messages."), 5000);
        return;
    }
    const QStringList ids = selectedMessageIds();
    if (ids.isEmpty()) {
        return;
    }
    if (!color.isEmpty()) {
        QSettings().setValue(QStringLiteral("ui/lastFlag"), color); // what a click on the flag column uses
    }
    for (const QString &id : ids) {
        m_session->sync()->setFlag(id, color);
    }
}

QMenu *MainWindow::buildFlagMenu(QWidget *parent)
{
    auto *menu = new QMenu(tr("Fla&g"), parent);
    menu->setObjectName(QStringLiteral("flagMenu"));
    menu->setIcon(icon(QStringLiteral("flag")));
    for (const FlagColor &f : kFlagColors) {
        const QString id = QString::fromLatin1(f.id);
        QAction *a = menu->addAction(icon(QStringLiteral("flag"), flagColor(id)), flagName(id), this,
                                     [this, id]() { setFlagOnSelected(id); });
        a->setObjectName(QStringLiteral("actionFlag_") + id);
    }
    menu->addSeparator();
    QAction *clear = menu->addAction(tr("&Clear Flag"), this, [this]() { setFlagOnSelected({}); });
    clear->setObjectName(QStringLiteral("actionClearFlag"));
    return menu;
}

void MainWindow::moveMessagesToLabel(const QStringList &messageIds, const QString &targetLabelId)
{
    if (!(m_live && m_session && m_session->sync()) || messageIds.isEmpty() || targetLabelId.isEmpty()) {
        return;
    }
    zmail::SyncEngine *sync = m_session->sync();
    // Where each one was, for Undo.
    QList<QPair<QString, QStringList>> before;
    for (const QString &id : messageIds) {
        const zmail::CachedMessage c = m_session->cache()->summary(id);
        if (!c.id.isEmpty()) {
            before.append({id, c.labels});
        }
        sync->moveToLabel(id, targetLabelId);
    }
    QString where = tr("In");
    for (const zmail::CachedLabel &l : m_session->cache()->labels()) {
        if (l.id == targetLabelId && targetLabelId != QLatin1String("INBOX")) {
            where = l.name;
        }
    }
    const int n = int(messageIds.size());
    const QString what = n == 1 ? tr("Moved to %1.").arg(where) : tr("Moved %1 messages to %2.").arg(n).arg(where);
    statusBar()->showMessage(what, 5000);
    if (before.isEmpty()) {
        return;
    }
    offerUndo(what, tr("&Undo Move"), [this, before]() {
        if (!(m_live && m_session && m_session->sync())) {
            return;
        }
        for (const auto &b : before) {
            m_session->sync()->restoreFolders(b.first, b.second);
        }
        statusBar()->showMessage(before.size() == 1 ? tr("Message moved back.")
                                                    : tr("%1 messages moved back.").arg(before.size()),
                                 4000);
    });
}
