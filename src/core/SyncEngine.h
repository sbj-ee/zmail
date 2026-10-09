#pragma once

#include "MailCache.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <functional>
#include <memory>

class QTimer;

namespace zmail {

class GmailClient;
struct ApiError;

// Keeps a MailCache in step with Gmail:
//  - full sync: profile historyId, labels, newest N INBOX messages (metadata)
//  - incremental: history.list every pollInterval and on focus
//  - historyId too old (404) -> clear and full resync
//  - more pages per label on demand (scroll / mailbox selection)
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    SyncEngine(GmailClient *api, MailCache *cache, QObject *parent = nullptr);
    ~SyncEngine() override;

    void setInitialCount(int n) { m_initialCount = n; }
    void setPageSize(int n) { m_pageSize = n; }
    void setPollInterval(int ms);
    void setMaxInFlight(int n) { m_maxInFlight = n; }

    void start();
    void stop();
    bool isRunning() const { return m_running; }
    bool isBusy() const { return m_busy; }

    // Incremental poll now (window focus); ignored if one ran < 5 s ago.
    void pollNow(bool force = false);
    // Fetch the next page of a label (scrolled to the bottom / background).
    void fetchMore(const QString &labelId);
    // First page of a label that hasn't been synced yet (mailbox selected).
    void ensureLabel(const QString &labelId);
    bool hasMore(const QString &labelId) const;
    // Every message of a label, not just the pages scrolled to so far: keeps
    // fetching, 500 at a time, until there are no more (or stopLoadAll()).
    // Progress comes as statusChanged(); loadAllFinished() says how it ended.
    void loadAll(const QString &labelId, const QString &displayName = {});
    void stopLoadAll();
    QString loadingAll() const { return m_loadAllLabel; } // the label being loaded, or empty

    using MessageCb = std::function<void(const CachedMessage &m, const QString &error)>;
    void fetchBody(const QString &id, MessageCb cb);
    void markRead(const QString &id);
    void markUnread(const QString &id); // messages.modify addLabelIds: ["UNREAD"]
    // Move to Gmail Spam: add SPAM, remove INBOX (optimistic, rolled back on error).
    void markJunk(const QString &id);
    // Leave Spam: remove SPAM, add INBOX (optimistic, rolled back on error).
    void markNotJunk(const QString &id);
    // Local snooze: store wake time in the cache DB, remove INBOX on Gmail so
    // other clients agree the mail left the inbox, restore INBOX on wake.
    // Snooze state itself is not a Gmail label (avoids fighting history sync).
    void snooze(const QString &id, qint64 wakeMs);
    void unsnooze(const QString &id); // cancel: restore INBOX if it had it, drop row
    // Wake any due snoozes (startup + timer). Returns how many woke.
    int wakeDue(qint64 nowMs = 0);
    // Move to Gmail's Trash (users.messages.trash): optimistic cache update,
    // rolled back if the call fails.
    void trash(const QString &id);
    // Many at once (hundreds, thousands): one request per thousand messages
    // instead of one each. The same result as trash() on every id, and the
    // same Undo; if Gmail refuses a batch, those are trashed one by one.
    void trashMany(const QStringList &ids);
    // Undo a trash() from this session: users.messages.untrash, then put
    // back any labels the message had before (INBOX, UNREAD, ...) that
    // untrash didn't restore. Optimistic, rolled back if Gmail refuses.
    // trash() also takes the message out of its folder (its user labels):
    // one folder per message, and Trash is one. untrash() puts it back.
    // Returns false if the message wasn't trashed by this engine.
    bool untrash(const QString &id);

    // Flag a message in a colour (ui/Flags.h), or clear its flag with an
    // empty one. The colour is local; flagged or not is Gmail's STARRED.
    void setFlag(const QString &id, const QString &color);
    // Empty Trash: everything in Trash now leaves zmail (MailCache::purged).
    // emptied(n) reports how many, or -1 if Gmail couldn't be asked.
    void emptyTrash();

    // Labels as folders (users.labels.*). Create/rename use Gmail's "/" nesting
    // (e.g. "Projects/zmail"). deleteLabel removes the label only — messages
    // keep their other labels and are never trashed.
    void createLabel(const QString &name, const QString &backgroundColor = {});
    void renameLabel(const QString &id, const QString &newName);
    void deleteLabel(const QString &id);
    // Empty a folder: every message with the label goes to Trash and out of
    // the folder (and the Inbox), cached or not, 500 at a time.
    void emptyLabel(const QString &id, int round = 0, std::shared_ptr<QStringList> moved = {});
    // Undo of emptyLabel: the messages come out of Trash and back into the
    // folder. (Those that were also in the Inbox return to the folder only.)
    void unemptyLabel(const QString &id, const QStringList &messageIds);
    // Undo of a move: put a message's folders (user labels) and INBOX back as
    // they were in `before`, leaving its other labels as they are now.
    void restoreFolders(const QString &messageId, const QStringList &before);
    // A message lives in one folder. Moving it onto a user label adds that
    // label and removes INBOX and every other user label it had; moving it
    // onto INBOX puts it back in the Inbox and removes its user labels.
    // System labels other than INBOX (UNREAD, STARRED, ...) are left alone.
    void moveToLabel(const QString &messageId, const QString &targetLabelId);

    int fullSyncs() const { return m_fullSyncs; }

signals:
    void labelsChanged();
    void trashEmptied(int messages);
    void labelEmptied(const QString &labelId, const QStringList &messageIds); // emptyLabel finished
    // loadAll() ended: everything is in (complete), it was stopped, or a page failed.
    void loadAllFinished(const QString &labelId, int loaded, bool complete);
    void messagesChanged();
    void newMail(const QStringList &ids);
    void snoozesWoke(const QStringList &ids);
    void statusChanged(const QString &status);
    void syncError(const QString &message);
    // trash() was refused and rolled back (not emitted if Undo was already
    // pressed). Emitted before the messagesChanged() that brings the row back.
    void trashFailed(const QString &id, const QString &message);
    void trashSucceeded(const QString &id); // Gmail confirmed a trash()
    void fullResyncStarted(const QString &reason);
    void idle();

private:
    void fullSync(const QString &reason);
    // force=true: always re-fetch counts (startup / full sync / folder CRUD).
    // force=false: skip if a full label-count refresh ran recently (history poll).
    void refreshLabels(std::function<void()> then = {}, bool force = true);
    void listPage(const QString &labelId, const QString &pageToken, int remaining, std::function<void(bool ok)> done);
    void historyPage(qint64 start, const QString &pageToken, std::shared_ptr<struct HistoryRun> run);
    void finishHistory(std::shared_ptr<struct HistoryRun> run);
    // done(ok): ok is false if any fetch failed for good (404 doesn't count:
    // the message is gone). force re-fetches ids that are already cached.
    void fetchMetadata(const QStringList &ids, std::function<void(bool ok)> done, bool force = false);
    void drain();
    void setBusy(bool b, const QString &status = {});
    void reportError(const ApiError &e, const QString &what);
    void sendUntrash(const QString &id, const QStringList &before, const QJsonObject &trashedJson);
    QStringList userLabels(const QStringList &labels) const; // the ones that are folders
    void loadAllStep(bool retried);
    QString m_loadAllLabel;
    QString m_loadAllName;
    bool m_loadAllStop = false;
    // Every message id in Gmail's Trash, a page at a time; ok=false if a page failed.
    void listTrash(std::function<void(bool ok, const QStringList &ids)> done, const QString &pageToken = {},
                   std::shared_ptr<QStringList> sofar = {});
    void prunePurged(); // forget purged ids Gmail has since erased or restored
    static constexpr int kEmptyFolderRounds = 20; // x 500 messages per Empty Folder
    static QStringList labelIds(const QJsonObject &message); // a message resource's labelIds
    // users.messages.modify with the cache updated first. If Gmail refuses,
    // exactly what this edit changed is undone (label changes that arrived
    // meanwhile are kept) and `what` is reported; on success Gmail's label
    // list is adopted. after(err) runs either way, before the last
    // messagesChanged(). announce=false: no messagesChanged() unless undone.
    void modifyOptimistic(const QString &id, const QStringList &add, const QStringList &remove, const QString &what,
                          std::function<void(const ApiError &err)> after = {}, bool announce = true);

    GmailClient *m_api;
    QHash<QString, QStringList> m_labelsBeforeTrash; // for untrash()
    QSet<QString> m_trashInFlight;                   // trash() sent, no answer yet
    QSet<QString> m_untrashQueued;                   // ... and already undone
    MailCache *m_cache;
    QTimer *m_poll;
    QTimer *m_labelsRefreshSoon; // one label refresh after a burst of moves
    int m_initialCount = 500;
    int m_pageSize = 100;
    int m_maxInFlight = 8;
    bool m_running = false;
    bool m_busy = false;
    int m_fullSyncs = 0;
    qint64 m_lastPollMs = 0;
    int m_generation = 0; // bumps on stop()/full resync to drop stale callbacks

    struct Fetch
    {
        QString id;
        std::shared_ptr<int> pending;
        std::shared_ptr<int> failed;
        std::function<void(bool ok)> done;
    };
    QList<Fetch> m_fetchQueue;
    int m_inFlight = 0;
    QSet<QString> m_loadingLabels;
    qint64 m_lastLabelsRefreshMs = 0;
    bool m_labelsRefreshing = false;
    bool m_labelsRefreshAgain = false; // a forced refresh was asked for during one
    static constexpr qint64 kLabelRefreshMinIntervalMs = 60000;
    static constexpr int kLabelGetConcurrency = 2;
    static constexpr int kLabelRefreshSoonMs = 400; // restarted by each move that lands
};

} // namespace zmail
