#include "SyncEngine.h"

#include "GmailClient.h"
#include "Log.h"
#include "MessageParser.h"

#include <algorithm>
#include <utility>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>

namespace zmail {

struct HistoryRun
{
    int generation = 0;
    qint64 newestHistoryId = 0;
    QStringList added;           // message ids to fetch
    QSet<QString> addedInbox;    // added with INBOX+UNREAD (candidates for "new mail")
    bool changed = false;
};

namespace {
qint64 monoMs()
{
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t.elapsed();
}
const QString kInbox = QStringLiteral("INBOX");
} // namespace

SyncEngine::SyncEngine(GmailClient *api, MailCache *cache, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_cache(cache)
    , m_poll(new QTimer(this))
    , m_labelsRefreshSoon(new QTimer(this))
{
    m_poll->setInterval(30000);
    connect(m_poll, &QTimer::timeout, this, [this] { pollNow(true); });
    m_labelsRefreshSoon->setSingleShot(true);
    m_labelsRefreshSoon->setInterval(kLabelRefreshSoonMs);
    connect(m_labelsRefreshSoon, &QTimer::timeout, this, [this] {
        if (m_running) {
            refreshLabels(); // one under way? it runs again when that finishes
        }
    });
}

SyncEngine::~SyncEngine() = default;

void SyncEngine::setPollInterval(int ms)
{
    m_poll->setInterval(ms);
}

void SyncEngine::setBusy(bool b, const QString &status)
{
    m_busy = b;
    if (!status.isEmpty()) {
        emit statusChanged(status);
    }
    if (!b) {
        emit idle();
    }
}

void SyncEngine::reportError(const ApiError &e, const QString &what)
{
    const QString msg = tr("%1 failed: %2").arg(what, e.message.isEmpty() ? QString::number(e.httpStatus) : e.message);
    qCWarning(lcSync) << what << "failed with HTTP" << e.httpStatus << e.reason;
    emit syncError(msg);
    emit statusChanged(msg);
}

void SyncEngine::start()
{
    m_running = true;
    m_poll->start();
    if (m_cache->historyId() == 0 || m_cache->count(kInbox) == 0) {
        fullSync(tr("first sync"));
    } else {
        refreshLabels([this] { pollNow(true); });
    }
}

void SyncEngine::stop()
{
    m_running = false;
    ++m_generation;
    m_poll->stop();
    m_labelsRefreshSoon->stop();
    m_fetchQueue.clear();
    m_loadingLabels.clear(); // their callbacks are dropped with the old generation
    m_busy = false;
    m_labelsRefreshing = false;
    m_labelsRefreshAgain = false;
}

void SyncEngine::refreshLabels(std::function<void()> then, bool force)
{
    const int gen = m_generation;
    if (!force && m_lastLabelsRefreshMs != 0
        && monoMs() - m_lastLabelsRefreshMs < kLabelRefreshMinIntervalMs) {
        if (then) {
            then();
        }
        return;
    }
    // Coalesce overlapping refreshes (startup + history can race). A forced
    // one follows a change the running refresh may have listed too early
    // (folder created / renamed / deleted, a move), so it runs again after.
    if (m_labelsRefreshing) {
        if (force) {
            m_labelsRefreshAgain = true;
        }
        if (then) {
            then();
        }
        return;
    }
    m_labelsRefreshing = true;
    m_lastLabelsRefreshMs = monoMs();

    // labels.list returns id/name/type/color only on real Gmail. Message
    // counts (messagesTotal / messagesUnread) require labels.get per label.
    // Fetch counts with low concurrency (kLabelGetConcurrency) so we never
    // enqueue an N-wide parallel flood into the client — that storm + 429
    // retries busy-spun the UI thread on vertex.
    m_api->listLabels([this, gen, then](const QJsonObject &json, const ApiError &err) {
        auto finishRefresh = [this, gen, then] {
            if (gen == m_generation) {
                m_labelsRefreshing = false;
            }
            if (then) {
                then();
            }
            if (gen == m_generation && std::exchange(m_labelsRefreshAgain, false)) {
                QTimer::singleShot(0, this, [this] {
                    if (m_running) {
                        refreshLabels();
                    }
                });
            }
        };
        if (gen != m_generation) {
            return;
        }
        if (err.isError) {
            reportError(err, tr("Listing labels"));
            finishRefresh();
            return;
        }
        auto labels = std::make_shared<QList<CachedLabel>>();
        for (const auto &v : json.value(QStringLiteral("labels")).toArray()) {
            const QJsonObject o = v.toObject();
            CachedLabel l;
            l.id = o.value(QStringLiteral("id")).toString();
            l.name = o.value(QStringLiteral("name")).toString();
            l.type = o.value(QStringLiteral("type")).toString();
            // Prefer any counts the list happened to include (mock / future API).
            l.unread = o.value(QStringLiteral("messagesUnread")).toInt();
            l.total = o.value(QStringLiteral("messagesTotal")).toInt();
            l.color = o.value(QStringLiteral("color")).toObject().value(QStringLiteral("backgroundColor")).toString();
            if (!l.id.isEmpty()) {
                labels->append(l);
            }
        }
        if (labels->isEmpty()) {
            m_cache->replaceLabels(*labels);
            emit labelsChanged();
            finishRefresh();
            return;
        }

        // Publish names/colors immediately; counts fill in as gets complete.
        m_cache->replaceLabels(*labels);
        emit labelsChanged();

        auto next = std::make_shared<int>(0);
        auto inFlight = std::make_shared<int>(0);
        auto completed = std::make_shared<int>(0);
        const int total = labels->size();
        auto pumpGets = std::make_shared<std::function<void()>>();
        // Weak: a function that owned itself would never be freed. The
        // callbacks of the gets in flight keep it alive.
        const std::weak_ptr<std::function<void()>> weakPump = pumpGets;
        *pumpGets = [this, gen, labels, next, inFlight, completed, total, finishRefresh, weakPump]() {
            const auto pumpGets = weakPump.lock();
            if (!pumpGets || gen != m_generation) {
                return;
            }
            while (*inFlight < kLabelGetConcurrency && *next < total) {
                const int i = (*next)++;
                ++(*inFlight);
                const QString id = labels->at(i).id;
                m_api->getLabel(id, [this, gen, labels, i, inFlight, completed, total, finishRefresh,
                                     pumpGets](const QJsonObject &o, const ApiError &getErr) {
                    if (gen != m_generation) {
                        return;
                    }
                    if (!getErr.isError) {
                        CachedLabel &l = (*labels)[i];
                        l.unread = o.value(QStringLiteral("messagesUnread")).toInt();
                        l.total = o.value(QStringLiteral("messagesTotal")).toInt();
                        const QString color = o.value(QStringLiteral("color"))
                                                  .toObject()
                                                  .value(QStringLiteral("backgroundColor"))
                                                  .toString();
                        if (!color.isEmpty()) {
                            l.color = color;
                        }
                        if (!o.value(QStringLiteral("name")).toString().isEmpty()) {
                            l.name = o.value(QStringLiteral("name")).toString();
                        }
                        if (!o.value(QStringLiteral("type")).toString().isEmpty()) {
                            l.type = o.value(QStringLiteral("type")).toString();
                        }
                    }
                    // A single get failure leaves that label's counts at the list
                    // values (usually 0); the rest of the tree still refreshes.
                    --(*inFlight);
                    ++(*completed);
                    if (*completed == total) {
                        m_cache->replaceLabels(*labels);
                        emit labelsChanged();
                        finishRefresh();
                        return;
                    }
                    (*pumpGets)();
                });
            }
        };
        (*pumpGets)();
    });
}

void SyncEngine::fullSync(const QString &reason)
{
    ++m_fullSyncs;
    ++m_generation;
    m_labelsRefreshing = false;
    m_labelsRefreshAgain = false;
    const int gen = m_generation;
    m_fetchQueue.clear();
    m_loadingLabels.clear(); // their callbacks are dropped with the old generation
    setBusy(true, tr("Syncing Inbox (%1)\u2026").arg(reason));
    qCInfo(lcSync) << "Full sync:" << reason;
    m_api->getProfile([this, gen](const QJsonObject &profile, const ApiError &err) {
        if (gen != m_generation) {
            return;
        }
        if (err.isError) {
            reportError(err, tr("Reading the Gmail profile"));
            setBusy(false);
            return;
        }
        // Take historyId *before* listing so nothing between is missed.
        const qint64 startHistory = profile.value(QStringLiteral("historyId")).toString().toLongLong();
        // Snoozes are local-only: keep their rows (and messages) across the wipe.
        m_cache->clearMessages(/*keepSnoozed=*/true);
        m_cache->setMeta(QStringLiteral("pageToken:") + kInbox, {});
        m_cache->setMeta(QStringLiteral("synced:") + kInbox, {});
        emit messagesChanged();
        refreshLabels([this, gen, startHistory] {
            if (gen != m_generation) {
                return;
            }
            listPage(kInbox, {}, m_initialCount, [this, gen, startHistory](bool ok) {
                if (gen != m_generation) {
                    return;
                }
                if (!ok) {
                    setBusy(false);
                    return;
                }
                // Snoozed mail has left INBOX, so the listing above didn't see
                // it: refresh the kept rows (a 404 drops the row and its snooze).
                QStringList snoozed;
                for (const MailCache::SnoozeRow &row : m_cache->snoozes(false)) {
                    snoozed.append(row.messageId);
                }
                fetchMetadata(snoozed, [this, gen, startHistory](bool) {
                    if (gen != m_generation) {
                        return;
                    }
                    m_cache->setHistoryId(startHistory);
                    m_lastPollMs = monoMs();
                    emit messagesChanged();
                    setBusy(false, tr("Inbox synced: %n message(s)", nullptr, m_cache->count(kInbox)));
                    // Anything that changed while we were listing comes in via history.
                    QTimer::singleShot(0, this, [this] { pollNow(true); });
                }, /*force=*/true);
            });
        });
    });
}

void SyncEngine::listPage(const QString &labelId, const QString &pageToken, int remaining,
                          std::function<void(bool)> done)
{
    const int gen = m_generation;
    const int n = std::min(500, std::max(1, remaining));
    m_api->listMessages(labelId, n, pageToken,
                        [this, gen, labelId, remaining, done](const QJsonObject &json, const ApiError &err) {
        if (gen != m_generation) {
            return;
        }
        if (err.isError) {
            reportError(err, tr("Listing %1").arg(labelId));
            done(false);
            return;
        }
        QStringList ids;
        for (const auto &v : json.value(QStringLiteral("messages")).toArray()) {
            ids.append(v.toObject().value(QStringLiteral("id")).toString());
        }
        const QString next = json.value(QStringLiteral("nextPageToken")).toString();
        m_cache->setMeta(QStringLiteral("pageToken:") + labelId, next.isEmpty() ? QStringLiteral("-") : next);
        m_cache->setMeta(QStringLiteral("synced:") + labelId, QStringLiteral("1"));
        const int left = remaining - int(ids.size());
        fetchMetadata(ids, [this, gen, labelId, next, left, done](bool) {
            if (gen != m_generation) {
                return;
            }
            emit messagesChanged();
            if (!next.isEmpty() && left > 0) {
                listPage(labelId, next, left, done);
            } else {
                done(true);
            }
        });
    });
}

bool SyncEngine::hasMore(const QString &labelId) const
{
    return m_cache->meta(QStringLiteral("pageToken:") + labelId) != QLatin1String("-");
}

void SyncEngine::fetchMore(const QString &labelId)
{
    if (!m_running || m_loadingLabels.contains(labelId)) {
        return;
    }
    const QString token = m_cache->meta(QStringLiteral("pageToken:") + labelId);
    if (token == QLatin1String("-")) {
        return;
    }
    m_loadingLabels.insert(labelId);
    emit statusChanged(tr("Loading more\u2026"));
    listPage(labelId, token, m_pageSize, [this, labelId](bool) {
        m_loadingLabels.remove(labelId);
        emit statusChanged(tr("%n message(s) cached", nullptr, m_cache->count()));
    });
}

void SyncEngine::ensureLabel(const QString &labelId)
{
    if (!m_running || m_cache->meta(QStringLiteral("synced:") + labelId) == QLatin1String("1") ||
        m_loadingLabels.contains(labelId)) {
        return;
    }
    m_loadingLabels.insert(labelId);
    listPage(labelId, {}, m_pageSize, [this, labelId](bool) { m_loadingLabels.remove(labelId); });
}

void SyncEngine::fetchMetadata(const QStringList &ids, std::function<void(bool)> done, bool force)
{
    QStringList todo;
    for (const QString &id : ids) {
        if (force || !m_cache->contains(id)) {
            todo.append(id);
        }
    }
    if (todo.isEmpty()) {
        done(true);
        return;
    }
    auto pending = std::make_shared<int>(int(todo.size()));
    auto failed = std::make_shared<int>(0);
    for (const QString &id : todo) {
        m_fetchQueue.append({id, pending, failed, done});
    }
    drain();
}

void SyncEngine::drain()
{
    while (m_inFlight < m_maxInFlight && !m_fetchQueue.isEmpty()) {
        Fetch f = m_fetchQueue.takeFirst();
        ++m_inFlight;
        const int gen = m_generation;
        m_api->getMessageMetadata(f.id, [this, f, gen](const QJsonObject &json, const ApiError &err) {
            --m_inFlight;
            if (gen != m_generation) {
                drain();
                return;
            }
            if (!err.isError) {
                m_cache->upsert(MessageParser::fromMetadata(json));
            } else if (err.httpStatus == 404) {
                m_cache->remove(f.id); // deleted meanwhile (a forced refresh may still hold the row)
            } else {
                ++*f.failed;
                reportError(err, tr("Fetching a message"));
            }
            if (--*f.pending == 0) {
                f.done(*f.failed == 0);
            } else if (*f.pending % 50 == 0) {
                emit messagesChanged();
            }
            drain();
        });
    }
}

void SyncEngine::pollNow(bool force)
{
    if (!m_running || m_busy) {
        return;
    }
    if (!force && monoMs() - m_lastPollMs < 5000) {
        return;
    }
    const qint64 start = m_cache->historyId();
    if (start == 0) {
        fullSync(tr("no history id"));
        return;
    }
    m_lastPollMs = monoMs();
    m_busy = true;
    auto run = std::make_shared<HistoryRun>();
    run->generation = m_generation;
    run->newestHistoryId = start;
    historyPage(start, {}, run);
}

void SyncEngine::historyPage(qint64 start, const QString &pageToken, std::shared_ptr<HistoryRun> run)
{
    m_api->listHistory(QString::number(start), pageToken, [this, start, run](const QJsonObject &json, const ApiError &err) {
        if (run->generation != m_generation) {
            return;
        }
        if (err.isError) {
            m_busy = false;
            if (err.httpStatus == 404) {
                // startHistoryId is too old (Gmail keeps roughly a week): start over.
                const QString reason = tr("history too old");
                emit fullResyncStarted(reason);
                fullSync(reason);
                return;
            }
            reportError(err, tr("Checking for new mail"));
            setBusy(false);
            return;
        }
        const MailCache::Batch batch(*m_cache); // one commit per history page
        for (const auto &hv : json.value(QStringLiteral("history")).toArray()) {
            const QJsonObject h = hv.toObject();
            for (const auto &a : h.value(QStringLiteral("messagesAdded")).toArray()) {
                const QJsonObject msg = a.toObject().value(QStringLiteral("message")).toObject();
                const QString id = msg.value(QStringLiteral("id")).toString();
                const QStringList labels = labelIds(msg);
                if (!run->added.contains(id)) {
                    run->added.append(id);
                }
                if (labels.contains(kInbox) && labels.contains(QStringLiteral("UNREAD")) &&
                    !labels.contains(QStringLiteral("DRAFT")) && !labels.contains(QStringLiteral("SENT"))) {
                    run->addedInbox.insert(id);
                }
                run->changed = true;
            }
            for (const auto &d : h.value(QStringLiteral("messagesDeleted")).toArray()) {
                const QString id = d.toObject().value(QStringLiteral("message")).toObject().value(QStringLiteral("id")).toString();
                m_cache->remove(id);
                run->added.removeAll(id);
                run->addedInbox.remove(id);
                run->changed = true;
            }
            for (const char *key : {"labelsAdded", "labelsRemoved"}) {
                const bool adding = qstrcmp(key, "labelsAdded") == 0;
                for (const auto &lv : h.value(QLatin1String(key)).toArray()) {
                    const QJsonObject lo = lv.toObject();
                    const QString id = lo.value(QStringLiteral("message")).toObject().value(QStringLiteral("id")).toString();
                    const QStringList ids = labelIds(lo);
                    if (adding) {
                        m_cache->modifyLabels(id, ids, {});
                    } else {
                        m_cache->modifyLabels(id, {}, ids);
                        if (ids.contains(QStringLiteral("UNREAD")) || ids.contains(kInbox)) {
                            run->addedInbox.remove(id);
                        }
                    }
                    run->changed = true;
                }
            }
        }
        run->newestHistoryId =
            std::max(run->newestHistoryId, json.value(QStringLiteral("historyId")).toString().toLongLong());
        const QString next = json.value(QStringLiteral("nextPageToken")).toString();
        if (!next.isEmpty()) {
            historyPage(start, next, run);
        } else {
            finishHistory(run);
        }
    });
}

void SyncEngine::finishHistory(std::shared_ptr<HistoryRun> run)
{
    QStringList fresh;
    for (const QString &id : run->added) {
        if (run->addedInbox.contains(id) && !m_cache->contains(id)) {
            fresh.append(id);
        }
    }
    fetchMetadata(run->added, [this, run, fresh](bool ok) {
        if (run->generation != m_generation) {
            return;
        }
        if (ok) {
            m_cache->setHistoryId(run->newestHistoryId);
        } else {
            // Leave historyId where it was: the next poll replays this range
            // and fetches what's still missing (everything else is idempotent).
            qCWarning(lcSync) << "History poll incomplete; will replay from" << m_cache->historyId();
        }
        auto finish = [this, run, fresh] {
            m_busy = false;
            if (run->changed) {
                emit messagesChanged();
            }
            QStringList stillNew;
            for (const QString &id : fresh) {
                const CachedMessage m = m_cache->summary(id);
                if (m.labels.contains(kInbox) && m.unread()) {
                    stillNew.append(id);
                }
            }
            if (!stillNew.isEmpty()) {
                qCInfo(lcSync) << stillNew.size() << "new INBOX message(s)";
                emit newMail(stillNew);
            }
            emit idle();
        };
        if (run->changed) {
            // Debounced: history polls must not re-stampede labels.get for every
            // tiny change (that was a major source of 429 storms on vertex).
            refreshLabels(finish, /*force=*/false);
        } else {
            finish();
        }
    });
}

void SyncEngine::fetchBody(const QString &id, MessageCb cb)
{
    const CachedMessage cached = m_cache->message(id);
    // Bodies cached by 0.2.0 lack Message-ID/References; refetch for replies.
    if (cached.hasBody && !cached.messageIdHeader.isEmpty()) {
        cb(cached, {});
        return;
    }
    m_api->getMessageFull(id, [this, id, cb](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            cb(m_cache->message(id), err.message.isEmpty() ? tr("HTTP %1").arg(err.httpStatus) : err.message);
            return;
        }
        CachedMessage meta = MessageParser::fromMetadata(json);
        const MessageParser::Body body = MessageParser::bodyFromFull(json);
        meta.hasAttachment = !body.attachments.isEmpty();
        {
            const MailCache::Batch batch(*m_cache);
            m_cache->upsert(meta);
            m_cache->setBody(id, body.text, body.html, body.attachments);
        }
        cb(m_cache->message(id), {});
    });
}

QStringList SyncEngine::labelIds(const QJsonObject &message)
{
    QStringList labels;
    for (const auto &l : message.value(QStringLiteral("labelIds")).toArray()) {
        labels.append(l.toString());
    }
    return labels;
}

void SyncEngine::modifyOptimistic(const QString &id, const QStringList &add, const QStringList &remove,
                                  const QString &what, std::function<void(const ApiError &)> after, bool announce)
{
    // What this edit really changes, so a refusal undoes exactly that and
    // keeps any label change that arrived (history, another edit) meanwhile.
    const QStringList before = m_cache->summary(id).labels;
    QStringList added, removed;
    for (const QString &a : add) {
        if (!before.contains(a) && !added.contains(a)) {
            added.append(a);
        }
    }
    for (const QString &r : remove) {
        if (before.contains(r) && !removed.contains(r)) {
            removed.append(r);
        }
    }
    m_cache->modifyLabels(id, add, remove);
    if (announce) {
        emit messagesChanged();
    }
    m_api->modifyLabels(id, add, remove,
                        [this, id, added, removed, what, after, announce](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            m_cache->modifyLabels(id, removed, added);
            reportError(err, what);
            if (after) {
                after(err);
            }
            emit messagesChanged();
            return;
        }
        const QStringList labels = labelIds(json);
        if (!labels.isEmpty()) {
            m_cache->setLabels(id, labels);
            if (announce) {
                emit messagesChanged();
            }
        }
        if (after) {
            after(err);
        }
    });
}

void SyncEngine::markRead(const QString &id)
{
    if (!m_cache->summary(id).unread()) {
        return;
    }
    // Quiet: the list already shows it read (messages.modify removeLabelIds: ["UNREAD"]).
    modifyOptimistic(id, {}, {QStringLiteral("UNREAD")}, tr("Marking as read"), {}, /*announce=*/false);
}

void SyncEngine::markJunk(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || m.labels.contains(QStringLiteral("SPAM"))) {
        return;
    }
    modifyOptimistic(id, {QStringLiteral("SPAM")}, {QStringLiteral("INBOX")}, tr("Marking as Junk"));
}

void SyncEngine::markNotJunk(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || !m.labels.contains(QStringLiteral("SPAM"))) {
        return;
    }
    modifyOptimistic(id, {QStringLiteral("INBOX")}, {QStringLiteral("SPAM")}, tr("Marking as Not Junk"));
}

void SyncEngine::snooze(const QString &id, qint64 wakeMs)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || wakeMs <= 0) {
        return;
    }
    const bool hadInbox = m.labels.contains(QStringLiteral("INBOX"));
    m_cache->setSnooze(id, wakeMs, hadInbox);
    if (!hadInbox) {
        emit messagesChanged();
        return;
    }
    modifyOptimistic(id, {}, {QStringLiteral("INBOX")}, tr("Snoozing"), [this, id](const ApiError &err) {
        if (err.isError) {
            m_cache->clearSnooze(id); // INBOX is back, so the user still sees it
        }
    });
}

void SyncEngine::unsnooze(const QString &id)
{
    const auto row = m_cache->snooze(id);
    if (row.messageId.isEmpty()) {
        return;
    }
    const bool restore = row.hadInbox || row.wakeMs > 0;
    m_cache->clearSnooze(id);
    if (!restore || m_cache->summary(id).labels.contains(QStringLiteral("INBOX"))) {
        emit messagesChanged();
        return;
    }
    modifyOptimistic(id, {QStringLiteral("INBOX")}, {}, tr("Unsnoozing"));
}

int SyncEngine::wakeDue(qint64 nowMs)
{
    if (nowMs <= 0) {
        nowMs = QDateTime::currentMSecsSinceEpoch();
    }
    const QStringList due = m_cache->dueSnoozes(nowMs);
    if (due.isEmpty()) {
        return 0;
    }
    for (const QString &id : due) {
        const auto row = m_cache->snooze(id);
        if (m_cache->summary(id).labels.contains(QStringLiteral("TRASH"))) {
            m_cache->clearSnooze(id); // deleted while snoozed: stays in Trash
            continue;
        }
        m_cache->markSnoozeWoke(id);
        if (row.hadInbox && !m_cache->summary(id).labels.contains(QStringLiteral("INBOX"))) {
            modifyOptimistic(id, {QStringLiteral("INBOX")}, {}, tr("Waking a snoozed message"),
                             [this, id, row](const ApiError &err) {
                if (!err.isError) {
                    return;
                }
                if (err.httpStatus == 404) {
                    m_cache->remove(id); // deleted elsewhere while it was snoozed
                } else {
                    // Gmail still has it out of INBOX: stay snoozed and due, so
                    // the next wake check tries again.
                    m_cache->setSnooze(id, row.wakeMs, row.hadInbox);
                }
            });
        }
    }
    emit messagesChanged();
    emit snoozesWoke(due);
    return due.size();
}

void SyncEngine::trash(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty()) {
        return;
    }
    const QStringList before = m.labels;
    m_labelsBeforeTrash.insert(id, before);
    m_trashInFlight.insert(id);
    m_cache->modifyLabels(id, {QStringLiteral("TRASH")}, {QStringLiteral("INBOX")}); // optimistic
    emit messagesChanged();
    m_api->trashMessage(id, [this, id, before](const QJsonObject &json, const ApiError &err) {
        m_trashInFlight.remove(id);
        const bool undone = m_untrashQueued.remove(id); // Undo pressed before Gmail answered
        if (err.isError) {
            m_cache->setLabels(id, before);
            m_labelsBeforeTrash.remove(id);
            if (!undone) {
                reportError(err, tr("Moving to Trash"));
                emit trashFailed(id, err.message);
            }
            emit messagesChanged();
            return;
        }
        if (undone) {
            sendUntrash(id, before, json);
            return;
        }
        emit trashSucceeded(id);
        const QStringList labels = labelIds(json);
        if (!labels.isEmpty()) {
            m_cache->setLabels(id, labels);
            emit messagesChanged();
        }
    });
}

bool SyncEngine::untrash(const QString &id)
{
    if (!m_labelsBeforeTrash.contains(id)) {
        return false;
    }
    const QStringList before = m_labelsBeforeTrash.take(id);
    const QJsonObject trashedJson{{QStringLiteral("labelIds"), QJsonArray::fromStringList(m_cache->summary(id).labels)}};
    m_cache->setLabels(id, before); // optimistic: back where it was
    emit messagesChanged();
    if (m_trashInFlight.contains(id)) {
        // The trash call hasn't come back; untrash once it has, so the two
        // can't cross on the wire.
        m_untrashQueued.insert(id);
        return true;
    }
    sendUntrash(id, before, trashedJson);
    return true;
}

void SyncEngine::sendUntrash(const QString &id, const QStringList &before, const QJsonObject &trashedJson)
{
    const QStringList trashed = labelIds(trashedJson);
    m_api->untrashMessage(id, [this, id, before, trashed](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            m_cache->setLabels(id, trashed);
            m_labelsBeforeTrash.insert(id, before);
            reportError(err, tr("Undoing the move to Trash"));
            emit messagesChanged();
            return;
        }
        // untrash only removes TRASH; INBOX (and UNREAD, user labels) may
        // need putting back.
        const QStringList now = labelIds(json);
        QStringList missing;
        for (const QString &l : before) {
            if (!now.contains(l) && l != QLatin1String("TRASH")) {
                missing << l;
            }
        }
        if (missing.isEmpty()) {
            if (!now.isEmpty()) {
                m_cache->setLabels(id, now);
                emit messagesChanged();
            }
            return;
        }
        m_api->modifyLabels(id, missing, {}, [this, id](const QJsonObject &json2, const ApiError &err2) {
            if (err2.isError) {
                reportError(err2, tr("Undoing the move to Trash"));
                return;
            }
            const QStringList labels = labelIds(json2);
            if (!labels.isEmpty()) {
                m_cache->setLabels(id, labels);
                emit messagesChanged();
            }
        });
    });
}

void SyncEngine::markUnread(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || m.unread()) {
        return;
    }
    modifyOptimistic(id, {QStringLiteral("UNREAD")}, {}, tr("Marking as unread")); // messages.modify addLabelIds
}


void SyncEngine::createLabel(const QString &name, const QString &backgroundColor)
{
    if (!m_running || name.trimmed().isEmpty()) {
        return;
    }
    const QString trimmed = name.trimmed();
    m_api->createLabel(trimmed, backgroundColor, [this, trimmed](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            reportError(err, tr("Creating folder \"%1\"").arg(trimmed));
            return;
        }
        // Refresh so the new label (with counts) appears under Gmail Labels.
        Q_UNUSED(json);
        refreshLabels();
    });
}

void SyncEngine::renameLabel(const QString &id, const QString &newName)
{
    if (!m_running || id.isEmpty() || newName.trimmed().isEmpty()) {
        return;
    }
    const QString trimmed = newName.trimmed();
    QString color;
    for (const CachedLabel &l : m_cache->labels()) {
        if (l.id == id) {
            color = l.color;
            break;
        }
    }
    m_api->updateLabel(id, trimmed, color, [this, id, trimmed](const QJsonObject &, const ApiError &err) {
        if (err.isError) {
            reportError(err, tr("Renaming folder"));
            return;
        }
        refreshLabels();
    });
}

void SyncEngine::deleteLabel(const QString &id)
{
    if (!m_running || id.isEmpty()) {
        return;
    }
    // users.labels.delete removes the label from every message; it does not
    // trash or delete the messages themselves.
    m_api->deleteLabel(id, [this, id](const QJsonObject &, const ApiError &err) {
        if (err.isError) {
            reportError(err, tr("Deleting folder"));
            return;
        }
        // Strip the label from the local cache so open views update immediately.
        {
            const MailCache::Batch batch(*m_cache);
            for (const QString &messageId : m_cache->messageIds(id)) {
                m_cache->modifyLabels(messageId, {}, {id});
            }
        }
        refreshLabels();
        emit messagesChanged();
    });
}

void SyncEngine::moveToLabel(const QString &messageId, const QString &targetLabelId)
{
    if (!m_running || messageId.isEmpty() || targetLabelId.isEmpty()) {
        return;
    }
    const CachedMessage before = m_cache->summary(messageId);
    if (before.id.isEmpty()) {
        return;
    }
    // One folder per message: leaving for the target takes it out of the
    // Inbox and out of every other folder (user label) it was in.
    const QString inbox = QStringLiteral("INBOX");
    QStringList add;
    QStringList remove;
    if (!before.labels.contains(targetLabelId)) {
        add.append(targetLabelId);
    }
    if (targetLabelId != inbox && before.labels.contains(inbox)) {
        remove.append(inbox);
    }
    for (const CachedLabel &l : m_cache->labels()) {
        if (l.type == QLatin1String("user") && l.id != targetLabelId && before.labels.contains(l.id)) {
            remove.append(l.id);
        }
    }
    if (add.isEmpty() && remove.isEmpty()) {
        return; // already there, and nowhere else
    }

    modifyOptimistic(messageId, add, remove, tr("Moving to folder"), [this](const ApiError &err) {
        if (!err.isError) {
            // Counts on the sidebar are stale until the next label refresh;
            // nudge them without a full sync: once, after the last move of a
            // burst has landed.
            m_labelsRefreshSoon->start();
        }
    });
}

} // namespace zmail
