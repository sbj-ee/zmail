#pragma once

#include "MailCache.h"

#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace zmail {

// What the windows ask of an account's mail, whatever carries it. Keeps a
// MailCache in step with the server and changes mail there. Two backends:
//   GmailSyncEngine  the Gmail API (OAuth accounts)
//   ImapSyncEngine   IMAP with Gmail's extensions (app-password accounts)
// Ids are Gmail's either way: the API's message / thread ids are the hex of
// IMAP's X-GM-MSGID / X-GM-THRID, and labels use the API's system ids
// (INBOX, SENT, UNREAD, ...), so the cache and windows don't care which.
// See GmailSyncEngine.h for what each call does in detail.
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~SyncEngine() override = default;

    virtual void setInitialCount(int n) = 0;
    virtual void setPageSize(int n) = 0;
    virtual void setPollInterval(int ms) = 0;
    virtual void setMaxInFlight(int n) = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool isBusy() const = 0;

    // Catch up now (window focus); may be ignored if one ran very recently.
    virtual void pollNow(bool force = false) = 0;
    virtual void fetchMore(const QString &labelId) = 0;
    virtual void ensureLabel(const QString &labelId) = 0;
    virtual bool hasMore(const QString &labelId) const = 0;
    virtual void loadAll(const QString &labelId, const QString &displayName = {}) = 0;
    virtual void stopLoadAll() = 0;
    virtual QString loadingAll() const = 0;

    using MessageCb = std::function<void(const CachedMessage &m, const QString &error)>;
    virtual void fetchBody(const QString &id, MessageCb cb) = 0;
    virtual void markRead(const QString &id) = 0;
    virtual void markUnread(const QString &id) = 0;
    virtual void markJunk(const QString &id) = 0;
    virtual void markNotJunk(const QString &id) = 0;
    virtual void snooze(const QString &id, qint64 wakeMs) = 0;
    virtual void unsnooze(const QString &id) = 0;
    virtual int wakeDue(qint64 nowMs = 0) = 0;
    virtual void trash(const QString &id) = 0;
    virtual void trashMany(const QStringList &ids) = 0;
    virtual bool untrash(const QString &id) = 0;
    virtual void setFlag(const QString &id, const QString &color) = 0;
    virtual void emptyTrash() = 0;

    virtual void createLabel(const QString &name, const QString &backgroundColor = {},
                             std::function<void(const QString &id)> then = {}) = 0;
    virtual void renameLabel(const QString &id, const QString &newName) = 0;
    virtual void deleteLabel(const QString &id) = 0;
    virtual void emptyLabel(const QString &id, int round = 0, std::shared_ptr<QStringList> moved = {}) = 0;
    virtual void unemptyLabel(const QString &id, const QStringList &messageIds) = 0;
    virtual void clearImportantFromFiled() = 0;
    virtual void restoreFolders(const QString &messageId, const QStringList &before) = 0;
    virtual void moveToLabel(const QString &messageId, const QString &targetLabelId) = 0;
    virtual void touchCounts(const QStringList &labelIds) = 0;

    virtual int fullSyncs() const = 0;

signals:
    void labelsChanged();
    void countsChanged(); // some labels' totals / unread counts are new (not their names)
    void trashEmptied(int messages);
    void labelEmptied(const QString &labelId, const QStringList &messageIds); // emptyLabel finished
    // clearImportantFromFiled() ended; ok=false if the server refused part of it.
    void importantCleared(int messages, bool ok);
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
    void trashSucceeded(const QString &id); // the server confirmed a trash()
    void fullResyncStarted(const QString &reason);
    void idle();
};

} // namespace zmail
