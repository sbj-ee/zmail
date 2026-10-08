// MainWindow: what the user does to messages — delete with undo, junk /
// not junk, snooze, read state — and the selection helpers they share.
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

void MainWindow::trashMessage(QString id) // by value: Message window / single-id callers
{
    if (!id.isEmpty()) {
        trashMessages({id});
    }
}

void MainWindow::trashSelected()
{
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    trashMessages(ids);
}

void MainWindow::trashMessages(const QStringList &idsIn)
{
    QStringList ids;
    int unqueued = 0;
    for (const QString &id : idsIn) {
        if (id.startsWith(QLatin1String("queued:"))) {
            // Not mail yet: Delete takes it out of the queue, and that is all.
            if (m_live && m_session && m_session->cache()) {
                m_session->cache()->removeQueued(id.mid(7).toLongLong());
                ++unqueued;
            }
            continue;
        }
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    if (unqueued > 0) {
        if (idsIn.contains(m_shownId)) {
            m_view->clear();
            m_shownId.clear();
        }
        reloadFromCache();
        populateMailboxes();
        statusBar()->showMessage(unqueued == 1 ? tr("Removed 1 message from the queue.")
                                               : tr("Removed %1 messages from the queue.").arg(unqueued),
                                 5000);
        if (ids.isEmpty()) {
            return;
        }
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || ids.isEmpty()) {
        statusBar()->showMessage(m_live ? tr("Select a message to delete.") : tr("Sign in to Gmail to delete mail."),
                                 5000);
        return;
    }
    const QString currentId = currentListId();
    const bool currentInBatch = ids.contains(currentId);
    selectPastRemoved(ids);
    if (currentInBatch) {
        // One pending entry is enough to reselect the neighbour if Gmail refuses.
        m_pendingTrash.insert(currentId, {m_proxy->mailbox(), currentListId()});
    }
    for (const QString &id : ids) {
        sync->trash(id); // optimistic; rolled back (onTrashFailed) if Gmail refuses
    }
    if (ids.contains(m_shownId)) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
    }
    offerUndoDelete(ids);
}

QString MainWindow::currentListId() const
{
    const QModelIndex cur = m_list->currentIndex();
    return cur.isValid() ? m_model->item(m_proxy->mapToSource(cur).row()).id : QString();
}

QList<int> MainWindow::selectedSourceRows() const
{
    QList<int> rows;
    if (!m_actOnIds.isEmpty()) { // a mailbox window's menu: its messages
        for (const QString &id : m_actOnIds) {
            if (const int row = m_model ? m_model->rowForId(id) : -1; row >= 0) {
                rows.append(row);
            }
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }
    if (!m_list || !m_list->selectionModel() || !m_proxy || !m_model) {
        return rows;
    }
    QSet<int> seen;
    for (const QModelIndex &pi : m_list->selectionModel()->selectedRows(0)) {
        if (!pi.isValid()) {
            continue;
        }
        const int sr = m_proxy->mapToSource(pi).row();
        if (sr < 0 || seen.contains(sr)) {
            continue;
        }
        seen.insert(sr);
        rows.append(sr);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QStringList MainWindow::selectedMessageIds() const
{
    QStringList ids;
    for (int r : selectedSourceRows()) {
        const QString id = m_model->item(r).id;
        if (!id.isEmpty()) {
            ids.append(id);
        }
    }
    return ids;
}

void MainWindow::onTrashFailed(const QString &id)
{
    // The row comes back with the rollback's refresh; the error itself is
    // already in the status bar (syncError). Nothing is left to undo.
    if (m_lastTrashed.contains(id)) {
        m_lastTrashed.removeAll(id);
        if (m_lastTrashed.isEmpty()) {
            if (m_undoTimer) {
                m_undoTimer->stop();
            }
            if (m_undoBar) {
                m_undoBar->hide();
            }
            m_undoDeleteAction->setEnabled(false);
        }
    }
    const auto it = m_pendingTrash.constFind(id);
    if (it == m_pendingTrash.constEnd()) {
        return;
    }
    const PendingTrash p = *it;
    m_pendingTrash.erase(it);
    if (p.mailbox == m_proxy->mailbox() && p.neighbour == currentListId()) {
        m_reselectAfterReload = id;
    }
}

void MainWindow::junkMessage(QString id)
{
    junkSelectedIds(id.isEmpty() ? QStringList{} : QStringList{id});
}

void MainWindow::junkSelected()
{
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    junkSelectedIds(ids);
}

void MainWindow::junkSelectedIds(const QStringList &idsIn)
{
    QStringList ids;
    for (const QString &id : idsIn) {
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || ids.isEmpty()) {
        statusBar()->showMessage(
            m_live ? tr("Select a message to mark as Junk.") : tr("Sign in to Gmail to mark Junk."), 5000);
        return;
    }
    // Spam leaves In, and every view but Junk while Hide Spam is on.
    if (m_proxy->mailbox() == QLatin1String("In")
        || (m_proxy->hideSpam() && m_proxy->mailbox() != QLatin1String("Junk"))) {
        selectPastRemoved(ids);
    }
    for (const QString &id : ids) {
        sync->markJunk(id);
    }
    if (ids.contains(m_shownId)) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
    }
    statusBar()->showMessage(
        ids.size() == 1 ? tr("Moved to Spam.") : tr("Moved %1 messages to Spam.").arg(ids.size()), 5000);
}

void MainWindow::notJunkMessage(QString id)
{
    notJunkSelectedIds(id.isEmpty() ? QStringList{} : QStringList{id});
}

void MainWindow::notJunkSelected()
{
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    notJunkSelectedIds(ids);
}

void MainWindow::notJunkSelectedIds(const QStringList &idsIn)
{
    QStringList ids;
    for (const QString &id : idsIn) {
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || ids.isEmpty()) {
        statusBar()->showMessage(
            m_live ? tr("Select a message to mark as Not Junk.") : tr("Sign in to Gmail to mark Not Junk."), 5000);
        return;
    }
    if (m_proxy->mailbox() == QLatin1String("Junk")) {
        selectPastRemoved(ids); // no longer spam: gone from the Junk list
    }
    for (const QString &id : ids) {
        sync->markNotJunk(id);
    }
    statusBar()->showMessage(
        ids.size() == 1 ? tr("Moved out of Spam.") : tr("Moved %1 messages out of Spam.").arg(ids.size()), 5000);
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
    for (MailboxWindow *w : mailboxWindows()) {
        w->proxy()->setHideSpam(hide);
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

QMenu *MainWindow::buildSnoozeMenu(QWidget *parent)
{
    // Titled here, so every menu it is put in shows it: the message list's
    // context menu got it with no title, a blank row with an arrow (0.5.11).
    auto *menu = new QMenu(tr("S&nooze"), parent ? parent : this);
    menu->setObjectName(QStringLiteral("snoozePresetMenu"));
    menu->setIcon(icon(QStringLiteral("clock")));
    struct P { const char *id; const char *text; };
    for (const P &p : {P{"laterToday", QT_TR_NOOP("Later Today (+3 hours)")},
                       P{"tomorrow", QT_TR_NOOP("Tomorrow 8:00 AM")},
                       P{"weekend", QT_TR_NOOP("This Weekend (Sat 8:00 AM)")},
                       P{"nextWeek", QT_TR_NOOP("Next Week (Mon 8:00 AM)")}}) {
        QAction *a = menu->addAction(tr(p.text), this, [this, id = QByteArray(p.id)]() {
            const QDateTime wake = zmail::SnoozeTimes::wakeFor(id.constData(), QDateTime::currentDateTime());
            snoozeSelected(wake.toMSecsSinceEpoch());
        });
        a->setObjectName(QStringLiteral("snooze_") + QString::fromLatin1(p.id));
    }
    menu->addSeparator();
    QAction *custom = menu->addAction(tr("Custom…"), this, &MainWindow::customSnooze);
    custom->setObjectName(QStringLiteral("snooze_custom"));
    return menu;
}

void MainWindow::snoozeMessage(QString id, qint64 wakeMs)
{
    snoozeMessages(id.isEmpty() ? QStringList{} : QStringList{id}, wakeMs);
}

void MainWindow::snoozeSelected(qint64 wakeMs)
{
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    snoozeMessages(ids, wakeMs);
}

void MainWindow::snoozeMessages(const QStringList &idsIn, qint64 wakeMs)
{
    QStringList ids;
    for (const QString &id : idsIn) {
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || ids.isEmpty() || wakeMs <= 0) {
        statusBar()->showMessage(
            m_live ? tr("Select a message to snooze.") : tr("Sign in to Gmail to snooze mail."), 5000);
        return;
    }
    selectPastRemoved(ids); // they leave every view but Snoozed
    for (const QString &id : ids) {
        sync->snooze(id, wakeMs);
    }
    if (ids.contains(m_shownId)) {
        m_view->clear();
        m_shownId.clear();
        updateMessageActions();
    }
    const QString when = MessageListModel::formatDate(QDateTime::fromMSecsSinceEpoch(wakeMs).toLocalTime());
    statusBar()->showMessage(
        ids.size() == 1 ? tr("Snoozed until %1.").arg(when)
                        : tr("Snoozed %1 messages until %2.").arg(ids.size()).arg(when),
        5000);
}

void MainWindow::unsnoozeMessage(QString id)
{
    unsnoozeMessages(id.isEmpty() ? QStringList{} : QStringList{id});
}

void MainWindow::unsnoozeSelected()
{
    QStringList ids = selectedMessageIds();
    if (ids.isEmpty() && !m_shownId.isEmpty()) {
        ids << m_shownId;
    }
    unsnoozeMessages(ids);
}

void MainWindow::unsnoozeMessages(const QStringList &idsIn)
{
    QStringList ids;
    for (const QString &id : idsIn) {
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync || ids.isEmpty()) {
        statusBar()->showMessage(
            m_live ? tr("Select a snoozed message.") : tr("Sign in to Gmail to unsnooze."), 5000);
        return;
    }
    if (m_proxy->mailbox() == QLatin1String("Snoozed")) {
        selectPastRemoved(ids); // back to In: gone from the Snoozed list
    }
    for (const QString &id : ids) {
        sync->unsnooze(id);
    }
    statusBar()->showMessage(
        ids.size() == 1 ? tr("Unsnoozed.") : tr("Unsnoozed %1 messages.").arg(ids.size()), 5000);
}

void MainWindow::customSnooze()
{
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("customSnoozeDialog"));
    dlg.setWindowTitle(tr("Snooze until"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *edit = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600), &dlg);
    edit->setObjectName(QStringLiteral("snoozeDateTime"));
    edit->setCalendarPopup(true);
    edit->setDisplayFormat(QStringLiteral("MM/dd/yyyy h:mm AP")); // as in the message list
    edit->setMinimumDateTime(QDateTime::currentDateTime());
    lay->addWidget(edit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    snoozeSelected(edit->dateTime().toMSecsSinceEpoch());
}

void MainWindow::checkSnoozeWakes()
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    if (!sync) {
        return;
    }
    sync->wakeDue();
}

void MainWindow::setCurrentRead(bool read)
{
    QList<int> rows = selectedSourceRows();
    if (rows.isEmpty()) {
        const QModelIndex cur = m_list->currentIndex();
        if (!cur.isValid()) {
            return;
        }
        rows.append(m_proxy->mapToSource(cur).row());
    }
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    for (int row : rows) {
        const MailItem &m = m_model->item(row);
        if (sync && !m.id.isEmpty()) {
            read ? sync->markRead(m.id) : sync->markUnread(m.id);
        }
        m_model->setStatus(row, read ? MailStatus::Read : MailStatus::Unread);
    }
    updateCounts();
}

void MainWindow::offerUndoDelete(const QStringList &ids)
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
        // Appended: index 0 belongs to the status bar's own (non-permanent)
        // widgets, and Qt warned on every first Delete and appended anyway.
        statusBar()->addPermanentWidget(m_undoBar);
        m_undoTimer = new QTimer(this);
        m_undoTimer->setSingleShot(true);
        connect(m_undoTimer, &QTimer::timeout, this, [this]() {
            m_undoBar->hide();
            m_undoDeleteAction->setEnabled(false);
            m_lastTrashed.clear();
            m_undoOther = nullptr;
            m_undoDeleteAction->setText(tr("&Undo Delete"));
        });
    }
    m_lastTrashed = ids;
    m_undoOther = nullptr;
    m_undoDeleteAction->setText(tr("&Undo Delete"));
    m_undoLabel->setText(ids.size() <= 1 ? tr("Moved to Trash.")
                                         : tr("Moved %1 messages to Trash.").arg(ids.size()));
    m_undoBar->show();
    m_undoDeleteAction->setEnabled(true);
    m_undoTimer->start(kUndoDeleteMs);
}

// The same bar and the same Ctrl+Z for the other things that move mail in
// bulk: a transfer, Empty Folder, Empty Trash.
void MainWindow::offerUndo(const QString &what, const QString &menuText, std::function<void()> undo)
{
    offerUndoDelete({}); // builds the bar; no ids
    m_undoOther = std::move(undo);
    m_undoDeleteAction->setText(menuText);
    m_undoLabel->setText(what);
}

void MainWindow::undoDelete()
{
    zmail::SyncEngine *sync = m_live && m_session ? m_session->sync() : nullptr;
    const QStringList ids = m_lastTrashed;
    const std::function<void()> other = std::exchange(m_undoOther, nullptr);
    m_lastTrashed.clear();
    if (m_undoTimer) {
        m_undoTimer->stop();
    }
    if (m_undoBar) {
        m_undoBar->hide();
    }
    m_undoDeleteAction->setEnabled(false);
    m_undoDeleteAction->setText(tr("&Undo Delete"));
    if (other) {
        other();
        return;
    }
    if (!sync || ids.isEmpty()) {
        return;
    }
    int restored = 0;
    for (const QString &id : ids) {
        if (sync->untrash(id)) {
            ++restored;
        }
    }
    if (restored <= 0) {
        return;
    }
    statusBar()->showMessage(
        restored == 1 ? tr("Message restored.") : tr("%1 messages restored.").arg(restored), 4000);
}
