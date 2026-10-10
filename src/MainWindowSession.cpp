// MainWindow: the signed-in session — attaching it, reloading the list from
// the cache, showing a live message, the sync / account status and the
// sign-in banner.
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
        populateMailboxes(); // the account's folders, not the sample ones, without waiting for a label refresh
        if (const int n = queuedCount(); n > 0) {
            statusBar()->showMessage(n == 1 ? tr("1 message is waiting in Queue. File \u203a Send Queued Messages sends it.")
                                            : tr("%1 messages are waiting in Queue. File \u203a Send Queued Messages sends them.").arg(n),
                                     10000);
        }
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
        applyRulesToNewMail(ids); // each one's filter, and the sound
        statusBar()->showMessage((ids.size() == 1 ? tr("1 new message") : tr("%1 new messages").arg(ids.size())), 8000);
    });
    connect(sync, &zmail::SyncEngine::statusChanged, this, [this](const QString &s) { updateSyncLabel(s); });
    connect(sync, &zmail::SyncEngine::idle, this, [this]() {
        m_lastSync = QLocale(QLocale::English).toString(QTime::currentTime(), QStringLiteral("h:mm AP"));
        // syncError is followed by setBusy(false) -> idle; keep the red circle
        // until a later idle that was not paired with an error.
        if (m_keepSyncErrorAcrossIdle) {
            m_keepSyncErrorAcrossIdle = false;
        } else {
            m_syncError.clear();
        }
        updateSyncLabel();
    });
    connect(sync, &zmail::SyncEngine::syncError, this, [this](const QString &e) {
        m_syncError = e;
        m_keepSyncErrorAcrossIdle = true;
        // Temporary notice (same as before); permanent row + red circle remain
        // underneath and reappear when the message clears.
        statusBar()->showMessage(e, 10000);
        updateSyncLabel();
    });
    connect(sync, &zmail::SyncEngine::trashFailed, this, [this](const QString &id) { onTrashFailed(id); });
    connect(sync, &zmail::SyncEngine::trashSucceeded, this, [this](const QString &id) { m_pendingTrash.remove(id); });
    connect(sync, &zmail::SyncEngine::trashEmptied, this, [this](int n) {
        const QString what = n < 0 ? tr("Couldn't empty the Trash: Gmail didn't answer.")
                                   : (n == 1 ? tr("Trash emptied: 1 message.") : tr("Trash emptied: %1 messages.").arg(n));
        statusBar()->showMessage(what, 6000);
        if (n <= 0) {
            return;
        }
        const QSet<QString> before = m_purgedBeforeEmpty;
        offerUndo(what, tr("&Undo Empty Trash"), [this, before]() {
            if (!(m_live && m_session && m_session->cache())) {
                return;
            }
            m_session->cache()->setPurged(QStringList(before.begin(), before.end())); // hidden as it was before
            reloadFromCache();
            populateMailboxes();
            statusBar()->showMessage(tr("The Trash is back as it was."), 4000);
        });
    });
    connect(sync, &zmail::SyncEngine::countsChanged, this, &MainWindow::updateMailboxCounts);
    connect(sync, &zmail::SyncEngine::loadAllFinished, this, [this](const QString &, int loaded, bool complete) {
        rebuildMailboxMenus(); // back to "Load All Messages"
        statusBar()->showMessage(complete ? (loaded == 1 ? tr("All loaded: 1 message.") : tr("All loaded: %1 messages.").arg(loaded))
                                          : tr("Stopped: %1 messages loaded so far.").arg(loaded),
                                 8000);
    });
    connect(sync, &zmail::SyncEngine::importantCleared, this, [this](int messages, bool ok) {
        const QString what = messages == 0 ? tr("No filed mail was marked Important.")
                             : messages == 1 ? tr("Important cleared from 1 filed message.")
                                             : tr("Important cleared from %1 filed messages.").arg(messages);
        statusBar()->showMessage(ok ? what : tr("Gmail refused part of it. %1").arg(what), 8000);
    });
    connect(sync, &zmail::SyncEngine::labelEmptied, this, [this](const QString &labelId, const QStringList &ids) {
        if (ids.isEmpty()) {
            return;
        }
        const QString what = ids.size() == 1 ? tr("Folder emptied: 1 message to Trash.")
                                             : tr("Folder emptied: %1 messages to Trash.").arg(ids.size());
        statusBar()->showMessage(what, 6000);
        offerUndo(what, tr("&Undo Empty Folder"), [this, labelId, ids]() {
            if (m_live && m_session && m_session->sync()) {
                m_session->sync()->unemptyLabel(labelId, ids);
                statusBar()->showMessage(tr("Putting the folder's mail back\u2026"), 4000);
            }
        });
    });
    connect(sync, &zmail::SyncEngine::snoozesWoke, this, [this](const QStringList &ids) {
        statusBar()->showMessage((ids.size() == 1 ? tr("1 snoozed message returned to the Inbox.")
                                                  : tr("%1 snoozed messages returned to the Inbox.").arg(ids.size())), 8000);
        if (m_reloadTimer) {
            m_reloadTimer->start();
        }
    });
    checkSnoozeWakes();
    sendDueQueued(); // a Send Later whose time passed while zmail was closed
    m_snoozeTimer->start();
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
    const QHash<QString, QString> flags = cache->flags();
    const QSet<QString> purged = cache->purged(); // Empty Trash: gone from zmail, though Gmail still has them
    // Metadata only, snoozes joined in: bodies are read when a message is shown.
    for (const zmail::MailCache::Listed &row : cache->listing(20000)) {
        const zmail::CachedMessage &c = row.message;
        if (!purged.isEmpty() && purged.contains(c.id) && c.labels.contains(QStringLiteral("TRASH"))) {
            continue;
        }
        MailItem m;
        m.id = c.id;
        if (c.labels.contains(QStringLiteral("STARRED"))) {
            m.flag = flags.value(c.id, QString::fromLatin1(kStarredFlag));
        }
        if (!m_rules.rules.isEmpty()) {
            zmail::RuleMessage rm;
            rm.from = c.fromName.isEmpty() ? c.fromAddr : QStringLiteral("%1 <%2>").arg(c.fromName, c.fromAddr);
            rm.to = c.to;
            rm.subject = c.subject;
            if (const zmail::Rule *rule = m_rules.match(rm); rule && !rule->color.isEmpty()) {
                m.ruleColor = QColor(rule->color);
            }
        }
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
        if (!c.labels.contains(QStringLiteral("TRASH")) && !c.labels.contains(QStringLiteral("SPAM"))) {
            m.mailboxes << QStringLiteral("All"); // All Mail, as Gmail has it
        }
        for (const QString &id : c.labels) {
            if (id == QLatin1String("INBOX")) m.mailboxes << QStringLiteral("In");
            else if (id == QLatin1String("SENT")) m.mailboxes << QStringLiteral("Out");
            else if (id == QLatin1String("SPAM")) m.mailboxes << QStringLiteral("Junk");
            else if (id == QLatin1String("TRASH")) m.mailboxes << QStringLiteral("Trash");
            m.mailboxes << QStringLiteral("gmail:") + id;
        }
        if (row.snoozeWakeMs > 0) {
            m.snoozeWakeMs = row.snoozeWakeMs;
            m.mailboxes << QStringLiteral("Snoozed");
        }
        m.snoozeBadge = row.snoozeBadge;
        m.preview = c.snippet;
        m.attachments = c.attachments;
        items.append(std::move(m));
    }
    // The queue: written, set aside with Send Later, waiting in Queue.
    for (const zmail::MailCache::QueuedMessage &q : cache->queued()) {
        MailItem m;
        m.id = QStringLiteral("queued:%1").arg(q.id);
        m.status = MailStatus::Queued;
        const auto first = zmail::MessageParser::splitAddress(q.to.section(QLatin1Char(','), 0, 0));
        m.who = first.first.isEmpty() ? first.second : first.first;
        m.address = first.second;
        m.to = q.to;
        // A timed Send Later shows when it will go.
        m.date = QDateTime::fromMSecsSinceEpoch(q.sendAtMs > 0 ? q.sendAtMs : q.createdMs).toLocalTime();
        m.sizeBytes = q.size;
        m.subject = q.subject.isEmpty() ? tr("(no subject)") : q.subject;
        m.mailboxes = {QStringLiteral("Queue")};
        m.preview = q.text;
        items.append(std::move(m));
    }
    const QString reselect = std::exchange(m_reselectAfterReload, QString());
    const QString keep = m_shownId;
    const int fallbackRow = std::exchange(m_selectRowAfterReload, -1);
    const int scroll = m_list->verticalScrollBar()->value();
    m_model->setItems(std::move(items));
    const int back = reselect.isEmpty() ? -1 : m_model->rowForId(reselect);
    const QModelIndex backIndex = back >= 0 ? m_proxy->mapFromSource(m_model->index(back, 0)) : QModelIndex();
    const int row = keep.isEmpty() ? -1 : m_model->rowForId(keep);
    const QModelIndex pi = row >= 0 ? m_proxy->mapFromSource(m_model->index(row, 0)) : QModelIndex();
    if (backIndex.isValid()) {
        // A failed Delete: the message is back, so select and show it again.
        m_list->setCurrentIndex(backIndex);
    } else if (pi.isValid()) {
        // Still current and selected (the list was updated in place): leave
        // the selection alone, so several selected messages stay selected.
        if (m_list->currentIndex() != pi || !m_list->selectionModel()->isRowSelected(pi.row(), QModelIndex())) {
            const QSignalBlocker block(m_list->selectionModel());
            m_list->setCurrentIndex(pi);
        }
        m_shownId = keep;
    } else if (fallbackRow >= 0 && m_proxy->rowCount() > 0) {
        // The neighbour picked by selectPastRemoved() went too: same position.
        m_list->setCurrentIndex(m_proxy->index(std::min(fallbackRow, m_proxy->rowCount() - 1), 0));
    }
    m_list->verticalScrollBar()->setValue(scroll);
    updateCounts();
    updateMailboxCounts(); // the folders' counts move with the mail
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
    if (id.startsWith(QLatin1String("queued:"))) {
        // Not in Gmail yet: shown from the queue.
        const zmail::MailCache::QueuedMessage q = cache->queuedMessage(id.mid(7).toLongLong());
        ViewMessage v;
        v.id = id;
        v.from = m_session->fromHeader();
        v.to = q.to;
        v.cc = q.cc;
        v.subject = q.subject;
        v.date = QDateTime::fromMSecsSinceEpoch(q.createdMs);
        v.bodyText = q.text;
        if (!q.error.isEmpty()) {
            v.warning = tr("<b>This message is still queued.</b> The last attempt to send it failed: %1").arg(q.error.toHtmlEscaped());
        } else {
            v.warning = tr("<b>Queued.</b> File \u203a Send Queued Messages sends it. Double-click to change it; Delete takes it out of the queue.");
        }
        m_view->setMessage(v);
        return;
    }
    if (cache->snooze(id).badge) {
        cache->clearSnoozeBadge(id);
        // Soft refresh so the clock icon drops without a full resync.
        m_reloadTimer->start();
    }
    auto render = [this](const zmail::CachedMessage &c, bool loading, const QString &error) {
        m_view->setMessage(detail::liveViewMessage(c, loading, error));
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
        m_syncLabel->setText(tr("Offline sample data \u00b7 not signed in"));
        updateAccountStatus();
    }
}

void MainWindow::updateSyncLabel(const QString &status)
{
    if (!m_syncLabel) {
        return;
    }
    using State = zmail::MailSession::State;
    if (!m_session) {
        m_syncLabel->setText(tr("Offline sample data \u00b7 not signed in \u00b7 last sync: never"));
        updateAccountStatus();
        return;
    }
    switch (m_session->state()) {
    case State::NeedsClient:
        m_syncLabel->setText(tr("Sample data \u00b7 not connected \u00b7 File \u2192 Sign In to Gmail\u2026 to set up"));
        updateAccountStatus();
        return;
    case State::SignedOut:
        m_syncError.clear();
        m_syncLabel->setText(tr("Sample data \u00b7 signed out \u00b7 last sync: never"));
        updateAccountStatus();
        return;
    case State::Restoring:
        m_syncLabel->setText(tr("Restoring sign-in\u2026"));
        updateAccountStatus();
        return;
    case State::SigningIn:
        m_syncLabel->setText(tr("Waiting for Google sign-in in your browser\u2026"));
        updateAccountStatus();
        return;
    case State::SignedIn:
        break;
    }
    QString text = m_session->account();
    if (!status.isEmpty()) {
        text += QStringLiteral(" \u00b7 ") + status;
    } else if (!m_syncError.isEmpty()) {
        text += QStringLiteral(" \u00b7 ") + m_syncError;
    }
    text += QStringLiteral(" \u00b7 ") +
            (m_lastSync.isEmpty() ? tr("last sync: never")
                                  : tr("last sync %1 %2").arg(m_lastSync, QDateTime::currentDateTime().timeZoneAbbreviation()));
    m_syncLabel->setText(text);
    updateAccountStatus();
}

void MainWindow::updateAccountStatus()
{
    if (!m_accountStatus) {
        return;
    }
    using State = zmail::MailSession::State;
    const QPalette pal = QApplication::palette();
    QColor color = disconnectedForeground(pal);
    QString tip = tr("Disconnected");
    if (m_session && m_session->state() == State::SignedIn) {
        if (m_syncError.isEmpty()) {
            color = connectedForeground(pal);
            tip = tr("Connected");
        } else {
            color = suspiciousForeground(pal);
            tip = tr("Sync error: %1").arg(m_syncError);
        }
    } else if (m_session && m_session->state() == State::SigningIn) {
        tip = tr("Signing in\u2026");
    } else if (m_session && m_session->state() == State::Restoring) {
        tip = tr("Restoring sign-in\u2026");
    } else if (m_session && m_session->state() == State::NeedsClient) {
        tip = tr("Not connected");
    }
    QPalette lp = m_accountStatus->palette();
    lp.setColor(QPalette::WindowText, color);
    lp.setColor(QPalette::Text, color);
    m_accountStatus->setPalette(lp);
    m_accountStatus->setToolTip(tip);
    m_accountStatus->setAccessibleName(tip);
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
