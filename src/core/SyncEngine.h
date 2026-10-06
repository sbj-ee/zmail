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

    using MessageCb = std::function<void(const CachedMessage &m, const QString &error)>;
    void fetchBody(const QString &id, MessageCb cb);
    void markRead(const QString &id);
    void markUnread(const QString &id); // messages.modify addLabelIds: ["UNREAD"]
    // Move to Gmail Spam: add SPAM, remove INBOX (optimistic, rolled back on error).
    void markJunk(const QString &id);
    // Leave Spam: remove SPAM, add INBOX (optimistic, rolled back on error).
    void markNotJunk(const QString &id);
    // Move to Gmail's Trash (users.messages.trash): optimistic cache update,
    // rolled back if the call fails.
    void trash(const QString &id);
    // Undo a trash() from this session: users.messages.untrash, then put
    // back any labels the message had before (INBOX, UNREAD, ...) that
    // untrash didn't restore. Optimistic, rolled back if Gmail refuses.
    // Returns false if the message wasn't trashed by this engine.
    bool untrash(const QString &id);

    int fullSyncs() const { return m_fullSyncs; }

signals:
    void labelsChanged();
    void messagesChanged();
    void newMail(const QStringList &ids);
    void statusChanged(const QString &status);
    void syncError(const QString &message);
    void fullResyncStarted(const QString &reason);
    void idle();

private:
    void fullSync(const QString &reason);
    void refreshLabels(std::function<void()> then = {});
    void listPage(const QString &labelId, const QString &pageToken, int remaining, std::function<void(bool ok)> done);
    void historyPage(qint64 start, const QString &pageToken, std::shared_ptr<struct HistoryRun> run);
    void finishHistory(std::shared_ptr<struct HistoryRun> run);
    void fetchMetadata(const QStringList &ids, std::function<void()> done);
    void drain();
    void setBusy(bool b, const QString &status = {});
    void reportError(const ApiError &e, const QString &what);
    void sendUntrash(const QString &id, const QStringList &before, const QJsonObject &trashedJson);

    GmailClient *m_api;
    QHash<QString, QStringList> m_labelsBeforeTrash; // for untrash()
    QSet<QString> m_trashInFlight;                   // trash() sent, no answer yet
    QSet<QString> m_untrashQueued;                   // ... and already undone
    MailCache *m_cache;
    QTimer *m_poll;
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
        std::function<void()> done;
    };
    QList<Fetch> m_fetchQueue;
    int m_inFlight = 0;
    QSet<QString> m_loadingLabels;
};

} // namespace zmail
