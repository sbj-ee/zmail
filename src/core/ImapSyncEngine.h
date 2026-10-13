#pragma once

#include "AppPasswordAccount.h"
#include "ImapClient.h"
#include "ServerLimits.h"
#include "SyncEngine.h"

#include <deque>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

class QTimer;

namespace zmail {

// The IMAP backend of SyncEngine, for "Gmail (app password)" accounts.
//  - folders: LIST with Gmail's special-use attributes (\Sent, \Trash, \All,
//    ...) mapped to the API's system label ids; other folders are user labels
//  - counts: STATUS (MESSAGES UNSEEN) per folder; All Mail's is the mailbox total
//  - listing: newest N by sequence number, metadata only (headers, FLAGS,
//    X-GM-LABELS, X-GM-MSGID, X-GM-THRID, INTERNALDATE, RFC822.SIZE)
//  - ids: hex X-GM-MSGID / X-GM-THRID, the same as the Gmail API's
//  - bodies: BODY.PEEK[] found in All Mail by X-GM-MSGID (never sets \Seen)
//  - staying current: IDLE on INBOX (re-issued every 5 min: well under Gmail's
//    29, and soon enough to notice a connection that died without a word);
//    on a wake-up, new UIDs past the last one seen, CONDSTORE CHANGEDSINCE for
//    flag / label changes, and UID SEARCH for what was expunged. Servers
//    without IDLE are polled with the same incremental sync.
//  - changing mail: messages are found by X-GM-MSGID in All Mail (then Trash,
//    then Spam), then UID STORE +/-FLAGS (\Seen, \Flagged), +/-X-GM-LABELS
//    (Inbox, Important, folders), and UID MOVE into / out of Trash and Spam.
//    The cache changes first; a refusal undoes exactly that change.
//  - search: X-GM-RAW (Gmail's own search syntax) in All Mail
// Snooze arrives in a later update.
class ImapSyncEngine : public SyncEngine
{
    Q_OBJECT

public:
    // How the engine gets the app password when it (re)connects: from the
    // system keyring. It is used for LOGIN and not kept.
    using PasswordProvider = std::function<void(std::function<void(bool ok, const QString &password, const QString &error)>)>;

    ImapSyncEngine(const AppPasswordAccount &account, PasswordProvider password, MailCache *cache,
                   QObject *parent = nullptr);
    ~ImapSyncEngine() override;

    void setPlaintextForTests(bool on) { m_imap.setPlaintextForTests(on); }
    void setIdleRenewMs(int ms) { m_idleRenewMs = ms; }
    // A poll asks every folder for its counts at most this often; in between,
    // only the folders it read.
    void setAllCountsEveryMs(int ms) { m_allCountsEveryMs = ms; }
    // Reconnect / throttle backoff (ServerLimits::backoffMs), shrunk for tests.
    void setBackoffScaleForTests(double scale) { m_backoffScale = scale; }
    ServerLimits::Kind lastLimit() const { return m_lastLimit; }
    bool authFailed() const { return m_authFailed; }
    // Milliseconds until the next reconnect attempt; -1 when none is planned.
    int retryInMs() const { return m_retry->isActive() ? m_retry->remainingTime() : -1; }
    ImapClient *client() { return &m_imap; }
    bool idling() const { return m_imap.idling(); }
    int incrementalSyncs() const { return m_incrementals; }

    void setInitialCount(int n) override { m_initialCount = n; }
    void setPageSize(int n) override { m_pageSize = n; }
    void setPollInterval(int ms) override;
    void setMaxInFlight(int) override {}

    void start() override;
    void stop() override;
    bool isRunning() const override { return m_running; }
    bool isBusy() const override { return m_busy; }

    void pollNow(bool force = false) override;
    void fetchMore(const QString &labelId) override;
    void ensureLabel(const QString &labelId) override;
    bool hasMore(const QString &labelId) const override;
    void loadAll(const QString &labelId, const QString &displayName = {}) override;
    void stopLoadAll() override { m_loadAllStop = true; }
    QString loadingAll() const override { return m_loadAllLabel; }

    void fetchBody(const QString &id, MessageCb cb) override;
    void fetchFull(const QString &id, FullCb cb) override;
    bool supportsServerSearch() const override { return true; }
    void serverSearch(const QString &query, SearchCb cb) override;
    static constexpr int kServerSearchLimit = 200;
    void markRead(const QString &id) override;
    void markUnread(const QString &id) override;
    void markJunk(const QString &id) override;
    void markNotJunk(const QString &id) override;
    void snooze(const QString &id, qint64 wakeMs) override;
    void unsnooze(const QString &id) override;
    int wakeDue(qint64 nowMs = 0) override;
    void trash(const QString &id) override;
    void trashMany(const QStringList &ids) override;
    bool untrash(const QString &id) override;
    void setFlag(const QString &id, const QString &color) override;
    void emptyTrash() override;
    void createLabel(const QString &name, const QString &backgroundColor = {},
                     std::function<void(const QString &id)> then = {}) override;
    void renameLabel(const QString &id, const QString &newName) override;
    void deleteLabel(const QString &id) override;
    void emptyLabel(const QString &id, int round = 0, std::shared_ptr<QStringList> moved = {}) override;
    void unemptyLabel(const QString &id, const QStringList &messageIds) override;
    void clearImportantFromFiled() override;
    void restoreFolders(const QString &messageId, const QStringList &before) override;
    void moveToLabel(const QString &messageId, const QString &targetLabelId) override;
    void touchCounts(const QStringList &labelIds) override;

    // Drafts for SmtpSender: APPEND to [Gmail]/Drafts (\Draft \Seen), read
    // back the new X-GM-MSGID (APPENDUID), then remove the draft it replaces.
    // The id is the message's hex X-GM-MSGID, which also serves as the draft id.
    using DraftCb = std::function<void(bool ok, const QString &id, const QString &threadId, const QString &error)>;
    void appendDraft(const QByteArray &mime, const QString &replaceId, DraftCb cb);
    void deleteDraft(const QString &id, std::function<void(bool ok, const QString &error)> cb);
    int fullSyncs() const override { return m_fullSyncs; }

    // Gmail's X-GM-LABELS value / special-use attribute -> API label id.
    static QString labelIdForGmLabel(const QString &gmLabel);
    static QString labelIdForAttributes(const QStringList &attributes, const QString &name);
    // INTERNALDATE ("17-Jul-1996 02:44:25 -0700") -> epoch ms; 0 if unreadable.
    static qint64 parseInternalDate(const QString &s);
    // One FETCH response -> the cache's row. folderLabel is added (Gmail
    // leaves the selected folder's own label out of X-GM-LABELS).
    static CachedMessage messageFromFetch(const ImapValue &attrs, const QString &folderLabel);
    // UIDs as IMAP sequence sets ("3:9,12,20:31"), each short enough for one
    // command line. Given `present` (every UID in the folder), a run also
    // spans the gaps between neighbours: no message has those UIDs.
    static QList<QByteArray> uidSets(QList<qint64> uids, QList<qint64> present = {}, int maxBytes = 1000);

private:
    struct Folder
    {
        QString name;          // decoded, e.g. "[Gmail]/Sent Mail", "Receipts/2026"
        QByteArray wire;       // as sent on the wire (modified UTF-7, quoted)
        QString labelId;       // "INBOX", "SENT", "ALL", "Receipts/2026"
        QString type;          // system | user
        bool synced = false;
        qint64 uidValidity = 0;
        qint64 lastUid = 0;    // highest UID seen
        qint64 highestModSeq = 0;
        qint64 lowestSeq = 0;  // oldest sequence number listed so far (1 = all)
        qint64 exists = 0;
        QHash<qint64, QString> uidToId;
    };
    using Done = std::function<void()>;
    using Op = std::function<void(Done)>;
    // aborted: called instead of (or part-way through) run when the connection
    // goes, so the op can undo its change or answer whoever is waiting.
    struct QueuedOp
    {
        Op run;
        std::function<void()> aborted;
    };

    void connectAndSync(bool full);
    void enqueue(Op op, bool front = false, std::function<void()> aborted = {});
    void runNext();
    void afterOps();          // queue empty: back to INBOX and IDLE
    void listFolders(Done then);
    void refreshCounts(Done then, const QStringList &only = {}); // only: these labels' folders
    void checkConnection();
    void syncFolder(const QString &labelId, bool full, Done then);
    // anyUid: a UID FETCH of named messages, not "what is new" (which skips UIDs already seen).
    void fetchRange(Folder *f, const QByteArray &range, bool byUid, std::function<void(QStringList newIds)> then,
                    bool anyUid = false);
    void handleUntagged(const ImapUntagged &u);
    void scheduleIncremental();
    void setBusy(bool b, const QString &status = {});
    void notYet(const QString &what);
    Folder *folder(const QString &labelId);
    QByteArray selectCmd(const Folder &f) const;
    void select(const Folder &f, ImapClient::ReplyCb cb);
    void applySelect(Folder *f, const ImapClient::Reply &r, bool *uidValidityChanged);
    void loadAllStep();
    using Located = QHash<QString, QHash<QString, qint64>>; // folder label -> id -> UID
    void locate(const QStringList &ids, std::function<void(const Located &)> then);
    void searchIn(const QString &labelId, const QStringList &ids, std::function<void(const QHash<QString, qint64> &)> then);
    void selectFolder(const QString &labelId, std::function<void(bool ok)> then);
    void fetchRaw(const QString &id, std::function<void(const QByteArray &raw, const QString &error)> cb);
    QJsonObject fullJson(const QString &id, const QByteArray &raw) const;
    QByteArray gmLabel(const QString &labelId) const;
    QByteArray moveCmd(const QByteArray &set, const QByteArray &destWire) const;
    void applyLabels(const QStringList &ids, const QStringList &add, const QStringList &remove,
                     std::function<void(bool ok, const QString &error)> done);
    void modifyOptimistic(const QStringList &ids, const QStringList &add, const QStringList &remove, const QString &what,
                          std::function<void(bool ok)> after = {}, bool announce = true);
    QStringList userLabels(const QStringList &labels) const;
    void idsInFolder(const QString &labelId, std::function<void(bool ok, const QHash<qint64, QString> &uidToId,
                                                                const QHash<qint64, QStringList> &gmLabels)> then);
    void dropAndRetry(const QString &reason);
    void scheduleRetry(const QString &reason);
    void removeDraft(const QString &id, std::function<void(bool ok, const QString &error)> then);
    QHash<QString, QStringList> m_labelsBeforeTrash; // for untrash()
    // After start(): cached from an earlier run and not yet shown by the server
    // again. What the Inbox doesn't vouch for is set aside once it is read.
    QSet<QString> m_unverified;
    bool m_sweepPending = false; // start() has yet to set the unverified aside

    AppPasswordAccount m_account;
    PasswordProvider m_password;
    MailCache *m_cache;
    ImapClient m_imap;
    QList<Folder> m_folders;
    std::deque<QueuedOp> m_ops;
    std::function<void()> m_runningAbort; // the running op's, until it is done
    bool m_opRunning = false;
    bool m_running = false;
    bool m_busy = false;
    bool m_connected = false;
    bool m_connecting = false;
    int m_generation = 0;
    int m_initialCount = 500;
    int m_pageSize = 100;
    int m_fullSyncs = 0;
    int m_incrementals = 0;
    int m_idleRenewMs = 5 * 60 * 1000;
    int m_allCountsEveryMs = 60 * 1000;
    qint64 m_lastPollMs = 0;
    qint64 m_lastAllCountsMs = 0;
    qint64 m_lastTickMs = 0;
    bool m_listed = false;   // the last LIST was answered
    QString m_loadAllLabel;
    QString m_loadAllName;
    bool m_loadAllStop = false;
    int m_loadAllCount = 0;
    QTimer *m_poll;          // fallback polling when the server has no IDLE; reconnect backoff
    QTimer *m_idleRenew;
    QTimer *m_wake;          // debounces IDLE notifications
    QTimer *m_clock;         // notices the computer having slept
    QTimer *m_retry = new QTimer(this);
    int m_retryAttempt = 0;
    double m_backoffScale = 1.0;
    bool m_authFailed = false;
    ServerLimits::Kind m_lastLimit = ServerLimits::Kind::None;
};

} // namespace zmail
