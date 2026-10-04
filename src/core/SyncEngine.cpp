#include "SyncEngine.h"

#include "GmailClient.h"
#include "Log.h"
#include "MessageParser.h"

#include <algorithm>
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
{
    m_poll->setInterval(30000);
    connect(m_poll, &QTimer::timeout, this, [this] { pollNow(true); });
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
    m_fetchQueue.clear();
    m_busy = false;
}

void SyncEngine::refreshLabels(std::function<void()> then)
{
    const int gen = m_generation;
    m_api->listLabels([this, gen, then](const QJsonObject &json, const ApiError &err) {
        if (gen != m_generation) {
            return;
        }
        if (err.isError) {
            reportError(err, tr("Listing labels"));
        } else {
            QList<CachedLabel> labels;
            for (const auto &v : json.value(QStringLiteral("labels")).toArray()) {
                const QJsonObject o = v.toObject();
                CachedLabel l;
                l.id = o.value(QStringLiteral("id")).toString();
                l.name = o.value(QStringLiteral("name")).toString();
                l.type = o.value(QStringLiteral("type")).toString();
                l.unread = o.value(QStringLiteral("messagesUnread")).toInt();
                l.total = o.value(QStringLiteral("messagesTotal")).toInt();
                l.color = o.value(QStringLiteral("color")).toObject().value(QStringLiteral("backgroundColor")).toString();
                labels.append(l);
            }
            m_cache->replaceLabels(labels);
            emit labelsChanged();
        }
        if (then) {
            then();
        }
    });
}

void SyncEngine::fullSync(const QString &reason)
{
    ++m_fullSyncs;
    ++m_generation;
    const int gen = m_generation;
    m_fetchQueue.clear();
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
        m_cache->clearMessages();
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
                if (ok) {
                    m_cache->setHistoryId(startHistory);
                    m_lastPollMs = monoMs();
                }
                setBusy(false, ok ? tr("Inbox synced: %n message(s)", nullptr, m_cache->count(kInbox)) : QString());
                // Anything that changed while we were listing comes in via history.
                if (ok) {
                    QTimer::singleShot(0, this, [this] { pollNow(true); });
                }
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
        fetchMetadata(ids, [this, gen, labelId, next, left, done] {
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

void SyncEngine::fetchMetadata(const QStringList &ids, std::function<void()> done)
{
    QStringList todo;
    for (const QString &id : ids) {
        if (!m_cache->contains(id)) {
            todo.append(id);
        }
    }
    if (todo.isEmpty()) {
        done();
        return;
    }
    auto pending = std::make_shared<int>(int(todo.size()));
    for (const QString &id : todo) {
        m_fetchQueue.append({id, pending, done});
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
            } else if (err.httpStatus != 404) { // 404: deleted meanwhile
                reportError(err, tr("Fetching a message"));
            }
            if (--*f.pending == 0) {
                f.done();
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
        for (const auto &hv : json.value(QStringLiteral("history")).toArray()) {
            const QJsonObject h = hv.toObject();
            for (const auto &a : h.value(QStringLiteral("messagesAdded")).toArray()) {
                const QJsonObject msg = a.toObject().value(QStringLiteral("message")).toObject();
                const QString id = msg.value(QStringLiteral("id")).toString();
                QStringList labels;
                for (const auto &l : msg.value(QStringLiteral("labelIds")).toArray()) {
                    labels.append(l.toString());
                }
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
                    QStringList ids;
                    for (const auto &l : lo.value(QStringLiteral("labelIds")).toArray()) {
                        ids.append(l.toString());
                    }
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
    fetchMetadata(run->added, [this, run, fresh] {
        if (run->generation != m_generation) {
            return;
        }
        m_cache->setHistoryId(run->newestHistoryId);
        auto finish = [this, run, fresh] {
            m_busy = false;
            if (run->changed) {
                emit messagesChanged();
            }
            QStringList stillNew;
            for (const QString &id : fresh) {
                const CachedMessage m = m_cache->message(id);
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
            refreshLabels(finish); // unread counts
        } else {
            finish();
        }
    });
}

void SyncEngine::fetchBody(const QString &id, MessageCb cb)
{
    const CachedMessage cached = m_cache->message(id);
    if (cached.hasBody) {
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
        m_cache->upsert(meta);
        m_cache->setBody(id, body.text, body.html, body.attachments);
        cb(m_cache->message(id), {});
    });
}

void SyncEngine::markRead(const QString &id)
{
    const CachedMessage m = m_cache->message(id);
    if (!m.unread()) {
        return;
    }
    m_cache->modifyLabels(id, {}, {QStringLiteral("UNREAD")}); // optimistic
    m_api->modifyLabels(id, {}, {QStringLiteral("UNREAD")}, [this, id](const QJsonObject &json, const ApiError &err) {
        if (err.isError) {
            m_cache->modifyLabels(id, {QStringLiteral("UNREAD")}, {});
            reportError(err, tr("Marking as read"));
            emit messagesChanged();
            return;
        }
        QStringList labels;
        for (const auto &l : json.value(QStringLiteral("labelIds")).toArray()) {
            labels.append(l.toString());
        }
        if (!labels.isEmpty()) {
            m_cache->setLabels(id, labels);
        }
    });
}

} // namespace zmail
