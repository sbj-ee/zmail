#include "ImapSyncEngine.h"

#include "Log.h"
#include "MessageParser.h"
#include "RawMime.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QTimer>

namespace zmail {

namespace {

const QByteArray kMetaItems =
    "UID X-GM-MSGID X-GM-THRID X-GM-LABELS FLAGS INTERNALDATE RFC822.SIZE "
    "BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID REFERENCES REPLY-TO CONTENT-TYPE)]";

QString hexId(const ImapValue *v)
{
    if (!v) {
        return {};
    }
    bool ok = false;
    const quint64 n = v->text.toULongLong(&ok);
    return ok ? QString::number(n, 16) : QString();
}

QList<qint64> searchResults(const ImapClient::Reply &r)
{
    QList<qint64> out;
    for (const ImapUntagged &u : r.untagged) {
        if (u.keyword == "SEARCH") {
            for (int i = 1; i < u.values.size(); ++i) {
                if (u.values.at(i).type == ImapValue::Type::Atom) {
                    bool ok = false;
                    const qint64 n = u.values.at(i).text.toLongLong(&ok);
                    if (ok) {
                        out.append(n);
                    }
                }
            }
        }
    }
    return out;
}

const ImapValue *fetchAttrs(const ImapUntagged &u)
{
    if (u.keyword != "FETCH" || u.values.size() < 3 || !u.values.at(2).isList()) {
        return nullptr;
    }
    return &u.values.at(2);
}

} // namespace

ImapSyncEngine::ImapSyncEngine(const AppPasswordAccount &account, PasswordProvider password, MailCache *cache,
                               QObject *parent)
    : SyncEngine(parent)
    , m_account(account)
    , m_password(std::move(password))
    , m_cache(cache)
    , m_poll(new QTimer(this))
    , m_idleRenew(new QTimer(this))
    , m_wake(new QTimer(this))
{
    m_poll->setInterval(15000);
    connect(m_poll, &QTimer::timeout, this, [this] { pollNow(true); });
    m_idleRenew->setSingleShot(true);
    connect(m_idleRenew, &QTimer::timeout, this, [this] {
        // Gmail ends an IDLE after about 29 minutes: leave it and start again.
        enqueue([this](Done done) { m_imap.command("NOOP", [done](const ImapClient::Reply &) { done(); }); });
    });
    m_wake->setSingleShot(true);
    m_wake->setInterval(300);
    connect(m_wake, &QTimer::timeout, this, [this] { pollNow(true); });
    connect(&m_imap, &ImapClient::untagged, this, &ImapSyncEngine::handleUntagged);
    connect(&m_imap, &ImapClient::connectionLost, this, [this](const QString &reason) {
        if (!m_running) {
            return;
        }
        qCInfo(lcSync) << "IMAP connection lost:" << reason;
        m_connected = false;
        m_connecting = false;
        ++m_generation;
        m_ops.clear();
        m_opRunning = false;
        m_idleRenew->stop();
        setBusy(false);
        emit statusChanged(tr("Lost the connection to %1; reconnecting\u2026").arg(m_account.imapHost));
        const int gen = m_generation;
        QTimer::singleShot(m_reconnectDelayMs, this, [this, gen] {
            if (m_running && gen == m_generation && !m_connected && !m_connecting) {
                connectAndSync(false);
            }
        });
        m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
    });
}

ImapSyncEngine::~ImapSyncEngine()
{
    stop();
}

void ImapSyncEngine::setPollInterval(int ms)
{
    m_poll->setInterval(std::max(1000, ms));
}

void ImapSyncEngine::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    connectAndSync(true);
}

void ImapSyncEngine::stop()
{
    m_running = false;
    ++m_generation;
    m_ops.clear();
    m_opRunning = false;
    m_connected = false;
    m_connecting = false;
    m_poll->stop();
    m_idleRenew->stop();
    m_wake->stop();
    m_imap.close();
    m_busy = false;
}

void ImapSyncEngine::setBusy(bool b, const QString &status)
{
    m_busy = b;
    if (!status.isEmpty()) {
        emit statusChanged(status);
    }
}

ImapSyncEngine::Folder *ImapSyncEngine::folder(const QString &labelId)
{
    for (Folder &f : m_folders) {
        if (f.labelId == labelId) {
            return &f;
        }
    }
    return nullptr;
}

QByteArray ImapSyncEngine::selectCmd(const Folder &f) const
{
    QByteArray cmd = "SELECT " + f.wire;
    if (m_imap.hasCapability("CONDSTORE")) {
        cmd += " (CONDSTORE)";
    }
    return cmd;
}

QString ImapSyncEngine::labelIdForGmLabel(const QString &l)
{
    static const QHash<QString, QString> sys = {
        {QStringLiteral("\\Inbox"), QStringLiteral("INBOX")},     {QStringLiteral("\\Sent"), QStringLiteral("SENT")},
        {QStringLiteral("\\Draft"), QStringLiteral("DRAFT")},     {QStringLiteral("\\Drafts"), QStringLiteral("DRAFT")},
        {QStringLiteral("\\Important"), QStringLiteral("IMPORTANT")}, {QStringLiteral("\\Starred"), QStringLiteral("STARRED")},
        {QStringLiteral("\\Trash"), QStringLiteral("TRASH")},     {QStringLiteral("\\Spam"), QStringLiteral("SPAM")},
        {QStringLiteral("\\Junk"), QStringLiteral("SPAM")},
    };
    const auto it = sys.constFind(l);
    if (it != sys.constEnd()) {
        return *it;
    }
    if (l.compare(QLatin1String("INBOX"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("INBOX");
    }
    return l;
}

QString ImapSyncEngine::labelIdForAttributes(const QStringList &attrs, const QString &name)
{
    if (name.compare(QLatin1String("INBOX"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("INBOX");
    }
    static const QList<QPair<QString, QString>> map = {
        {QStringLiteral("\\All"), QString::fromLatin1(kAllMailLabel)},
        {QStringLiteral("\\Sent"), QStringLiteral("SENT")},
        {QStringLiteral("\\Drafts"), QStringLiteral("DRAFT")},
        {QStringLiteral("\\Trash"), QStringLiteral("TRASH")},
        {QStringLiteral("\\Junk"), QStringLiteral("SPAM")},
        {QStringLiteral("\\Flagged"), QStringLiteral("STARRED")},
        {QStringLiteral("\\Important"), QStringLiteral("IMPORTANT")},
    };
    for (const auto &m : map) {
        for (const QString &a : attrs) {
            if (a.compare(m.first, Qt::CaseInsensitive) == 0) {
                return m.second;
            }
        }
    }
    return name;
}

qint64 ImapSyncEngine::parseInternalDate(const QString &s)
{
    // dd-Mon-yyyy hh:mm:ss +zzzz (the day may be space-padded)
    static const QStringList months = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                       QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                       QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                       QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    const QStringList parts = s.simplified().split(QLatin1Char(' '));
    if (parts.size() != 3) {
        return 0;
    }
    const QStringList dmy = parts.at(0).split(QLatin1Char('-'));
    const QStringList hms = parts.at(1).split(QLatin1Char(':'));
    if (dmy.size() != 3 || hms.size() != 3 || parts.at(2).size() != 5) {
        return 0;
    }
    const int month = int(months.indexOf(dmy.at(1).toLower())) + 1;
    const QDate date(dmy.at(2).toInt(), month, dmy.at(0).toInt());
    const QTime time(hms.at(0).toInt(), hms.at(1).toInt(), hms.at(2).toInt());
    if (month < 1 || !date.isValid() || !time.isValid()) {
        return 0;
    }
    const QString z = parts.at(2);
    const int sign = z.startsWith(QLatin1Char('-')) ? -1 : 1;
    const int offset = sign * (z.mid(1, 2).toInt() * 3600 + z.mid(3, 2).toInt() * 60);
    return QDateTime(date, time, QTimeZone(offset)).toMSecsSinceEpoch();
}

CachedMessage ImapSyncEngine::messageFromFetch(const ImapValue &attrs, const QString &folderLabel)
{
    QStringList labels;
    auto add = [&labels](const QString &l) {
        if (!l.isEmpty() && !labels.contains(l)) {
            labels.append(l);
        }
    };
    if (const ImapValue *gm = attrs.get("X-GM-LABELS"); gm && gm->isList()) {
        for (const ImapValue &l : gm->items) {
            add(labelIdForGmLabel(ImapClient::decodeMailboxName(l.text)));
        }
    }
    bool seen = false;
    if (const ImapValue *flags = attrs.get("FLAGS"); flags && flags->isList()) {
        for (const ImapValue &fl : flags->items) {
            const QByteArray t = fl.text.toLower();
            if (t == "\\seen") {
                seen = true;
            } else if (t == "\\flagged") {
                add(QStringLiteral("STARRED"));
            } else if (t == "\\draft") {
                add(QStringLiteral("DRAFT"));
            }
        }
    }
    if (!seen) {
        add(QStringLiteral("UNREAD"));
    }
    if (folderLabel != QLatin1String(kAllMailLabel)) {
        add(folderLabel);
    }
    QByteArray headerBlock;
    for (int i = 0; i + 1 < attrs.items.size(); i += 2) {
        if (attrs.items.at(i).text.toUpper().startsWith("BODY[")) {
            headerBlock = attrs.items.at(i + 1).text;
        }
    }
    const QJsonArray headers = RawMime::parseHeaders(headerBlock);
    QString ctype;
    for (const auto &h : headers) {
        if (h.toObject().value(QStringLiteral("name")).toString().compare(QLatin1String("Content-Type"), Qt::CaseInsensitive) == 0) {
            ctype = h.toObject().value(QStringLiteral("value")).toString();
        }
    }
    QJsonObject json;
    json.insert(QStringLiteral("id"), hexId(attrs.get("X-GM-MSGID")));
    json.insert(QStringLiteral("threadId"), hexId(attrs.get("X-GM-THRID")));
    json.insert(QStringLiteral("labelIds"), QJsonArray::fromStringList(labels));
    const ImapValue *date = attrs.get("INTERNALDATE");
    json.insert(QStringLiteral("internalDate"), QString::number(date ? parseInternalDate(date->str()) : 0));
    const ImapValue *size = attrs.get("RFC822.SIZE");
    json.insert(QStringLiteral("sizeEstimate"), size ? size->num() : 0);
    json.insert(QStringLiteral("snippet"), QString());
    json.insert(QStringLiteral("payload"),
                QJsonObject{{QStringLiteral("mimeType"), ctype.section(QLatin1Char(';'), 0, 0).trimmed().toLower()},
                            {QStringLiteral("headers"), headers}});
    return MessageParser::fromMetadata(json);
}

void ImapSyncEngine::connectAndSync(bool full)
{
    if (m_connecting || !m_running) {
        return;
    }
    m_connecting = true;
    setBusy(true, tr("Connecting to %1\u2026").arg(m_account.imapHost));
    const int gen = m_generation;
    m_password([this, gen, full](bool ok, const QString &password, const QString &error) {
        if (gen != m_generation || !m_running) {
            return;
        }
        if (!ok || password.isEmpty()) {
            m_connecting = false;
            setBusy(false);
            emit syncError(error.isEmpty() ? tr("No app password for %1 in the system keyring. Enter it again in "
                                                "the account dialog.").arg(m_account.email)
                                           : error);
            return;
        }
        m_imap.open(m_account.imapHost, m_account.imapPort, m_account.email, password,
                    [this, gen, full](bool opened, const QString &err) {
                        if (gen != m_generation || !m_running) {
                            return;
                        }
                        m_connecting = false;
                        if (!opened) {
                            setBusy(false);
                            emit syncError(err);
                            return; // connectionLost schedules the retry
                        }
                        m_connected = true;
                        m_reconnectDelayMs = 2000;
                        enqueue([this, full](Done done) {
                            listFolders([this, full, done] {
                                refreshCounts([this, full, done] {
                                    if (full) {
                                        ++m_fullSyncs;
                                        m_cache->clearMessages(/*keepSnoozed=*/true);
                                        for (Folder &f : m_folders) {
                                            f.synced = false;
                                            f.uidToId.clear();
                                        }
                                    }
                                    syncFolder(QStringLiteral("INBOX"), full, [this, done] {
                                        // After a reconnect: catch up the other folders already shown.
                                        QStringList others;
                                        for (const Folder &f : m_folders) {
                                            if (f.synced && f.labelId != QLatin1String("INBOX")) {
                                                others.append(f.labelId);
                                            }
                                        }
                                        auto step = std::make_shared<std::function<void()>>();
                                        *step = [this, others, done, step]() mutable {
                                            if (others.isEmpty()) {
                                                emit messagesChanged();
                                                done();
                                                *step = nullptr;
                                                return;
                                            }
                                            const QString next = others.takeFirst();
                                            syncFolder(next, false, [step] { (*step)(); });
                                        };
                                        (*step)();
                                    });
                                });
                            });
                        });
                    });
    });
}

void ImapSyncEngine::enqueue(Op op, bool front)
{
    if (front) {
        m_ops.push_front(std::move(op));
    } else {
        m_ops.push_back(std::move(op));
    }
    runNext();
}

void ImapSyncEngine::runNext()
{
    if (m_opRunning || !m_connected) {
        return;
    }
    if (m_ops.empty()) {
        afterOps();
        return;
    }
    m_opRunning = true;
    m_idleRenew->stop();
    m_poll->stop();
    m_busy = true;
    Op op = std::move(m_ops.front());
    m_ops.pop_front();
    const int gen = m_generation;
    op([this, gen] {
        if (gen != m_generation) {
            return;
        }
        m_opRunning = false;
        QTimer::singleShot(0, this, [this, gen] {
            if (gen == m_generation) {
                runNext();
            }
        });
    });
}

void ImapSyncEngine::afterOps()
{
    Folder *inbox = folder(QStringLiteral("INBOX"));
    const int gen = m_generation;
    auto goIdle = [this, gen] {
        if (gen != m_generation || m_opRunning || !m_ops.empty() || !m_connected) {
            return;
        }
        m_busy = false;
        if (m_imap.hasCapability("IDLE")) {
            m_imap.idle();
            m_idleRenew->start(m_idleRenewMs);
        } else {
            m_poll->start();
        }
        emit statusChanged(QString());
        emit idle();
    };
    if (inbox && m_imap.selected() != inbox->name) {
        m_imap.command(selectCmd(*inbox), [this, gen, goIdle](const ImapClient::Reply &r) {
            if (gen != m_generation) {
                return;
            }
            if (Folder *f = folder(QStringLiteral("INBOX")); f && r.ok) {
                m_imap.setSelected(f->name);
                bool changed = false;
                applySelect(f, r, &changed);
            }
            goIdle();
        });
        return;
    }
    goIdle();
}

void ImapSyncEngine::handleUntagged(const ImapUntagged &u)
{
    if (!m_imap.idling()) {
        return;
    }
    if (u.keyword == "EXISTS" || u.keyword == "EXPUNGE" || u.keyword == "FETCH" || u.keyword == "VANISHED") {
        m_wake->start();
    }
}

void ImapSyncEngine::scheduleIncremental()
{
    m_wake->start();
}

void ImapSyncEngine::listFolders(Done then)
{
    const int gen = m_generation;
    m_imap.command("LIST \"\" \"*\"", [this, gen, then](const ImapClient::Reply &r) {
        if (gen != m_generation) {
            return;
        }
        if (!r.ok) {
            emit syncError(tr("Couldn't list the IMAP folders: %1").arg(r.text.left(200)));
            then();
            return;
        }
        QList<Folder> next;
        QList<CachedLabel> labels;
        for (const ImapUntagged &u : r.untagged) {
            if (u.keyword != "LIST" || u.values.size() < 4 || !u.values.at(1).isList()) {
                continue;
            }
            QStringList attrs;
            for (const ImapValue &a : u.values.at(1).items) {
                attrs.append(a.str());
            }
            if (attrs.contains(QStringLiteral("\\Noselect"), Qt::CaseInsensitive)
                || attrs.contains(QStringLiteral("\\NonExistent"), Qt::CaseInsensitive)) {
                continue;
            }
            const QByteArray wireName = u.values.at(3).text;
            const QString name = ImapClient::decodeMailboxName(wireName);
            Folder f;
            if (const Folder *old = [&]() -> const Folder * {
                    for (const Folder &o : m_folders) {
                        if (o.name == name) {
                            return &o;
                        }
                    }
                    return nullptr;
                }()) {
                f = *old;
            }
            f.name = name;
            f.wire = ImapClient::quote(QString::fromLatin1(wireName));
            f.labelId = labelIdForAttributes(attrs, name);
            f.type = f.labelId == name && f.labelId != QLatin1String("INBOX") ? QStringLiteral("user")
                                                                              : QStringLiteral("system");
            next.append(f);
            if (f.labelId != QLatin1String(kAllMailLabel)) {
                CachedLabel l;
                l.id = f.labelId;
                l.name = f.labelId == QLatin1String("INBOX") ? QStringLiteral("INBOX") : (f.type == QLatin1String("user") ? name : f.labelId);
                l.type = f.type;
                labels.append(l);
            }
        }
        // Gmail's UNREAD isn't a folder, but the windows count it like one.
        m_folders = next;
        m_cache->replaceLabels(labels);
        emit labelsChanged();
        then();
    });
}

void ImapSyncEngine::refreshCounts(Done then)
{
    if (m_folders.isEmpty()) {
        then();
        return;
    }
    const int gen = m_generation;
    auto pending = std::make_shared<int>(int(m_folders.size()));
    for (const Folder &f : m_folders) {
        const QString labelId = f.labelId;
        m_imap.command("STATUS " + f.wire + " (MESSAGES UNSEEN)", [this, gen, labelId, pending, then](const ImapClient::Reply &r) {
            if (gen != m_generation) {
                return;
            }
            for (const ImapUntagged &u : r.untagged) {
                if (u.keyword != "STATUS" || u.values.size() < 3 || !u.values.at(2).isList()) {
                    continue;
                }
                const ImapValue &kv = u.values.at(2);
                const int total = kv.get("MESSAGES") ? int(kv.get("MESSAGES")->num()) : 0;
                const int unread = kv.get("UNSEEN") ? int(kv.get("UNSEEN")->num()) : 0;
                if (labelId == QLatin1String(kAllMailLabel)) {
                    m_cache->setMailboxTotal(total);
                } else {
                    m_cache->setLabelCounts(labelId, total, unread);
                }
            }
            if (--*pending == 0) {
                emit countsChanged();
                then();
            }
        });
    }
}

void ImapSyncEngine::applySelect(Folder *f, const ImapClient::Reply &r, bool *uidValidityChanged)
{
    *uidValidityChanged = false;
    for (const ImapUntagged &u : r.untagged) {
        if (u.keyword == "EXISTS") {
            f->exists = u.number;
        }
        const QByteArray code = u.code();
        if (code.startsWith("UIDVALIDITY ")) {
            const qint64 v = code.mid(12).toLongLong();
            *uidValidityChanged = f->uidValidity != 0 && v != f->uidValidity;
            f->uidValidity = v;
        } else if (code.startsWith("HIGHESTMODSEQ ")) {
            f->highestModSeq = code.mid(14).toLongLong();
        }
    }
}

void ImapSyncEngine::fetchRange(Folder *fIn, const QByteArray &range, bool byUid, std::function<void(QStringList)> then)
{
    const QString labelId = fIn->labelId;
    const int gen = m_generation;
    m_imap.command((byUid ? "UID FETCH " : "FETCH ") + range + " (" + kMetaItems + ")",
                   [this, gen, labelId, byUid, then](const ImapClient::Reply &r) {
                       if (gen != m_generation) {
                           return;
                       }
                       Folder *f = folder(labelId);
                       QStringList fresh;
                       if (!f) {
                           then(fresh);
                           return;
                       }
                       if (!r.ok) {
                           emit syncError(tr("Couldn't read %1: %2").arg(f->name, r.text.left(200)));
                       }
                       const qint64 before = f->lastUid;
                       {
                           const MailCache::Batch batch(*m_cache);
                           for (const ImapUntagged &u : r.untagged) {
                               const ImapValue *attrs = fetchAttrs(u);
                               if (!attrs || !attrs->get("X-GM-MSGID")) {
                                   continue;
                               }
                               const qint64 uid = attrs->get("UID") ? attrs->get("UID")->num() : 0;
                               if (byUid && uid <= before) {
                                   continue; // "n:*" always answers with the last message
                               }
                               const CachedMessage m = messageFromFetch(*attrs, labelId);
                               if (m.id.isEmpty()) {
                                   continue;
                               }
                               if (!m_cache->contains(m.id)) {
                                   fresh.append(m.id);
                               }
                               m_cache->upsert(m);
                               if (uid > 0) {
                                   f->uidToId.insert(uid, m.id);
                                   f->lastUid = std::max(f->lastUid, uid);
                               }
                           }
                       }
                       then(fresh);
                   });
}

void ImapSyncEngine::syncFolder(const QString &labelId, bool full, Done then)
{
    Folder *f = folder(labelId);
    if (!f) {
        then();
        return;
    }
    const int gen = m_generation;
    const qint64 oldModSeq = f->highestModSeq;
    const qint64 oldLastUid = f->lastUid;
    m_imap.command(selectCmd(*f), [this, gen, labelId, full, then, oldModSeq, oldLastUid](const ImapClient::Reply &r) {
        if (gen != m_generation) {
            return;
        }
        Folder *f = folder(labelId);
        if (!f || !r.ok) {
            if (f) {
                emit syncError(tr("Couldn't open %1: %2").arg(f->name, r.text.left(200)));
            }
            then();
            return;
        }
        m_imap.setSelected(f->name);
        bool validityChanged = false;
        applySelect(f, r, &validityChanged);
        if (full || !f->synced || validityChanged) {
            if (validityChanged) {
                emit fullResyncStarted(tr("%1 changed on the server").arg(f->name));
            }
            f->uidToId.clear();
            f->lastUid = 0;
            if (f->exists == 0) {
                f->synced = true;
                f->lowestSeq = 1;
                then();
                return;
            }
            const qint64 lowest = std::max<qint64>(1, f->exists - m_initialCount + 1);
            fetchRange(f, QByteArray::number(lowest) + ":*", false, [this, gen, labelId, lowest, then](const QStringList &) {
                if (gen != m_generation) {
                    return;
                }
                if (Folder *ff = folder(labelId)) {
                    ff->synced = true;
                    ff->lowestSeq = lowest;
                }
                emit messagesChanged();
                then();
            });
            return;
        }
        // Incremental: new mail, then changed flags / labels, then what left.
        fetchRange(f, QByteArray::number(oldLastUid + 1) + ":*", true,
                   [this, gen, labelId, then, oldModSeq, oldLastUid](const QStringList &fresh) {
            if (gen != m_generation) {
                return;
            }
            auto finish = [this, gen, labelId, then, fresh] {
                if (gen != m_generation) {
                    return;
                }
                Folder *f = folder(labelId);
                if (!f) {
                    then();
                    return;
                }
                // Expunged (or moved out of this folder): UIDs we know that aren't there now.
                qint64 minUid = 0;
                for (auto it = f->uidToId.constBegin(); it != f->uidToId.constEnd(); ++it) {
                    minUid = minUid == 0 ? it.key() : std::min(minUid, it.key());
                }
                if (minUid == 0) {
                    then();
                    return;
                }
                m_imap.command("UID SEARCH UID " + QByteArray::number(minUid) + ":*",
                               [this, gen, labelId, then, fresh](const ImapClient::Reply &r) {
                    if (gen != m_generation) {
                        return;
                    }
                    Folder *f = folder(labelId);
                    if (f && r.ok) {
                        const QList<qint64> found = searchResults(r);
                        const QSet<qint64> present(found.begin(), found.end());
                        const MailCache::Batch batch(*m_cache);
                        for (auto it = f->uidToId.begin(); it != f->uidToId.end();) {
                            if (!present.contains(it.key())) {
                                if (f->labelId == QLatin1String(kAllMailLabel)) {
                                    m_cache->remove(it.value());
                                } else {
                                    m_cache->modifyLabels(it.value(), {}, {f->labelId});
                                }
                                it = f->uidToId.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    if (labelId == QLatin1String("INBOX")) {
                        QStringList unread;
                        for (const QString &id : fresh) {
                            if (m_cache->summary(id).unread()) {
                                unread.append(id);
                            }
                        }
                        if (!unread.isEmpty()) {
                            emit newMail(unread);
                        }
                    }
                    emit messagesChanged();
                    then();
                });
            };
            Folder *f = folder(labelId);
            if (!f || oldModSeq <= 0 || oldLastUid <= 0 || !m_imap.hasCapability("CONDSTORE")
                || f->highestModSeq == oldModSeq) {
                finish();
                return;
            }
            m_imap.command("UID FETCH 1:" + QByteArray::number(oldLastUid)
                               + " (UID FLAGS X-GM-LABELS X-GM-MSGID) (CHANGEDSINCE " + QByteArray::number(oldModSeq) + ")",
                           [this, gen, labelId, finish](const ImapClient::Reply &r) {
                if (gen != m_generation) {
                    return;
                }
                const MailCache::Batch batch(*m_cache);
                for (const ImapUntagged &u : r.untagged) {
                    const ImapValue *attrs = fetchAttrs(u);
                    if (!attrs) {
                        continue;
                    }
                    const CachedMessage m = messageFromFetch(*attrs, labelId);
                    if (!m.id.isEmpty() && m_cache->contains(m.id)) {
                        m_cache->setLabels(m.id, m.labels);
                    }
                }
                finish();
            });
        });
    });
}

void ImapSyncEngine::pollNow(bool force)
{
    if (!m_running) {
        return;
    }
    if (!m_connected) {
        if (!m_connecting) {
            connectAndSync(false);
        }
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_lastPollMs < 5000) {
        return;
    }
    m_lastPollMs = now;
    ++m_incrementals;
    enqueue([this](Done done) {
        QStringList labels{QStringLiteral("INBOX")};
        for (const Folder &f : m_folders) {
            if (f.synced && f.labelId != QLatin1String("INBOX")) {
                labels.append(f.labelId);
            }
        }
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, labels, done, step]() mutable {
            if (labels.isEmpty()) {
                refreshCounts([done] { done(); });
                *step = nullptr;
                return;
            }
            const QString next = labels.takeFirst();
            syncFolder(next, false, [step] { (*step)(); });
        };
        (*step)();
    });
}

void ImapSyncEngine::ensureLabel(const QString &labelId)
{
    Folder *f = folder(labelId);
    if (!f || f->synced) {
        return;
    }
    enqueue([this, labelId](Done done) { syncFolder(labelId, false, done); });
}

bool ImapSyncEngine::hasMore(const QString &labelId) const
{
    for (const Folder &f : m_folders) {
        if (f.labelId == labelId) {
            return f.synced && f.lowestSeq > 1;
        }
    }
    return false;
}

void ImapSyncEngine::fetchMore(const QString &labelId)
{
    if (!hasMore(labelId)) {
        return;
    }
    enqueue([this, labelId](Done done) {
        Folder *f = folder(labelId);
        if (!f) {
            done();
            return;
        }
        const int gen = m_generation;
        m_imap.command(selectCmd(*f), [this, gen, labelId, done](const ImapClient::Reply &r) {
            if (gen != m_generation) {
                return;
            }
            Folder *f = folder(labelId);
            if (!f || !r.ok) {
                done();
                return;
            }
            m_imap.setSelected(f->name);
            bool changed = false;
            applySelect(f, r, &changed);
            const int page = m_loadAllLabel == labelId ? 500 : m_pageSize;
            const qint64 high = std::min(f->lowestSeq - 1, f->exists);
            if (high < 1) {
                f->lowestSeq = 1;
                done();
                return;
            }
            const qint64 low = std::max<qint64>(1, high - page + 1);
            fetchRange(f, QByteArray::number(low) + ':' + QByteArray::number(high), false,
                       [this, gen, labelId, low, done](const QStringList &fresh) {
                if (gen != m_generation) {
                    return;
                }
                if (Folder *ff = folder(labelId)) {
                    ff->lowestSeq = low;
                }
                m_loadAllCount += int(fresh.size());
                emit messagesChanged();
                done();
            });
        });
    });
}

void ImapSyncEngine::loadAll(const QString &labelId, const QString &displayName)
{
    m_loadAllLabel = labelId;
    m_loadAllName = displayName.isEmpty() ? labelId : displayName;
    m_loadAllStop = false;
    m_loadAllCount = 0;
    if (Folder *f = folder(labelId); f && !f->synced) {
        enqueue([this, labelId](Done done) { syncFolder(labelId, false, done); });
    }
    enqueue([this](Done done) {
        loadAllStep();
        done();
    });
}

void ImapSyncEngine::loadAllStep()
{
    const QString label = m_loadAllLabel;
    if (label.isEmpty()) {
        return;
    }
    if (m_loadAllStop || !hasMore(label)) {
        const bool complete = !hasMore(label);
        m_loadAllLabel.clear();
        emit loadAllFinished(label, m_loadAllCount, complete);
        return;
    }
    emit statusChanged(tr("Loading all of %1\u2026 %2 so far").arg(m_loadAllName).arg(m_loadAllCount));
    fetchMore(label);
    enqueue([this](Done done) {
        loadAllStep();
        done();
    });
}

void ImapSyncEngine::fetchBody(const QString &id, MessageCb cb)
{
    const CachedMessage cached = m_cache->message(id);
    if (cached.hasBody && !cached.messageIdHeader.isEmpty() && cached.extrasVersion >= MailCache::kExtrasVersion) {
        cb(cached, {});
        return;
    }
    bool ok = false;
    const quint64 gmId = id.toULongLong(&ok, 16);
    if (!ok) {
        cb(cached, tr("Not an IMAP message id."));
        return;
    }
    // In front of any sync: the user is waiting for this one.
    enqueue([this, id, gmId, cb](Done done) {
        Folder *f = folder(QString::fromLatin1(kAllMailLabel));
        if (!f) {
            f = folder(QStringLiteral("INBOX"));
        }
        if (!f) {
            cb(m_cache->message(id), tr("No folder to read it from."));
            done();
            return;
        }
        const int gen = m_generation;
        const QString folderLabel = f->labelId;
        auto search = [this, gen, id, gmId, cb, done] {
            m_imap.command("UID SEARCH X-GM-MSGID " + QByteArray::number(gmId), [this, gen, id, cb, done](const ImapClient::Reply &r) {
                if (gen != m_generation) {
                    cb(m_cache->message(id), tr("Disconnected."));
                    return;
                }
                const QList<qint64> uids = searchResults(r);
                if (!r.ok || uids.isEmpty()) {
                    cb(m_cache->message(id), r.ok ? tr("That message is no longer on the server.") : r.text);
                    done();
                    return;
                }
                m_imap.command("UID FETCH " + QByteArray::number(uids.first()) + " (UID X-GM-MSGID BODY.PEEK[])",
                               [this, gen, id, cb, done](const ImapClient::Reply &fr) {
                    if (gen != m_generation) {
                        cb(m_cache->message(id), tr("Disconnected."));
                        return;
                    }
                    QByteArray raw;
                    for (const ImapUntagged &u : fr.untagged) {
                        if (const ImapValue *attrs = fetchAttrs(u)) {
                            for (int i = 0; i + 1 < attrs->items.size(); i += 2) {
                                if (attrs->items.at(i).text.toUpper() == "BODY[]") {
                                    raw = attrs->items.at(i + 1).text;
                                }
                            }
                        }
                    }
                    if (raw.isEmpty()) {
                        cb(m_cache->message(id), fr.ok ? tr("The server sent an empty message.") : fr.text);
                        done();
                        return;
                    }
                    const CachedMessage before = m_cache->summary(id);
                    QJsonObject json;
                    json.insert(QStringLiteral("id"), id);
                    json.insert(QStringLiteral("threadId"), before.threadId);
                    json.insert(QStringLiteral("labelIds"), QJsonArray::fromStringList(before.labels));
                    json.insert(QStringLiteral("internalDate"), QString::number(before.internalDateMs));
                    json.insert(QStringLiteral("sizeEstimate"), before.size ? before.size : raw.size());
                    json.insert(QStringLiteral("payload"), RawMime::toGmailPayload(raw));
                    CachedMessage meta = MessageParser::fromMetadata(json);
                    const MessageParser::Body body = MessageParser::bodyFromFull(json);
                    meta.hasAttachment = !body.attachments.isEmpty();
                    if (meta.snippet.isEmpty()) {
                        meta.snippet = body.text.simplified().left(200);
                    }
                    {
                        const MailCache::Batch batch(*m_cache);
                        m_cache->upsert(meta);
                        m_cache->setBody(id, body.text, body.html, body.attachments,
                                         {body.listUnsubscribe, body.listUnsubscribePost, body.calendar});
                    }
                    cb(m_cache->message(id), {});
                    done();
                });
            });
        };
        if (m_imap.selected() == f->name) {
            search();
            return;
        }
        m_imap.command(selectCmd(*f), [this, gen, folderLabel, search, id, cb, done](const ImapClient::Reply &r) {
            if (gen != m_generation) {
                cb(m_cache->message(id), tr("Disconnected."));
                return;
            }
            if (Folder *ff = folder(folderLabel); ff && r.ok) {
                m_imap.setSelected(ff->name);
                bool changed = false;
                applySelect(ff, r, &changed);
            }
            search();
        });
    }, /*front=*/true);
}

void ImapSyncEngine::notYet(const QString &what)
{
    emit statusChanged(tr("%1 isn't available for app-password accounts yet; it comes in the next zmail update.").arg(what));
}

// Changing mail over IMAP (X-GM-LABELS STORE, UID MOVE, \Seen, ...) is the
// next update. Reading a message doesn't mark it read on the server meanwhile
// (BODY.PEEK), so these do nothing rather than claim success.
void ImapSyncEngine::markRead(const QString &) {}
void ImapSyncEngine::markUnread(const QString &) { notYet(tr("Mark as unread")); }
void ImapSyncEngine::markJunk(const QString &) { notYet(tr("Junk")); }
void ImapSyncEngine::markNotJunk(const QString &) { notYet(tr("Not junk")); }
void ImapSyncEngine::snooze(const QString &, qint64) { notYet(tr("Snooze")); }
void ImapSyncEngine::unsnooze(const QString &) { notYet(tr("Snooze")); }
int ImapSyncEngine::wakeDue(qint64) { return 0; }
void ImapSyncEngine::trash(const QString &id) { emit trashFailed(id, tr("Delete isn't available for app-password accounts yet.")); emit messagesChanged(); }
void ImapSyncEngine::trashMany(const QStringList &ids)
{
    for (const QString &id : ids) {
        emit trashFailed(id, tr("Delete isn't available for app-password accounts yet."));
    }
    emit messagesChanged();
}
bool ImapSyncEngine::untrash(const QString &) { return false; }
void ImapSyncEngine::setFlag(const QString &, const QString &) { notYet(tr("Flags")); }
void ImapSyncEngine::emptyTrash() { notYet(tr("Empty Trash")); emit trashEmptied(-1); }
void ImapSyncEngine::createLabel(const QString &, const QString &, std::function<void(const QString &)> then)
{
    notYet(tr("New folder"));
    if (then) {
        then({});
    }
}
void ImapSyncEngine::renameLabel(const QString &, const QString &) { notYet(tr("Rename folder")); }
void ImapSyncEngine::deleteLabel(const QString &) { notYet(tr("Delete folder")); }
void ImapSyncEngine::emptyLabel(const QString &id, int, std::shared_ptr<QStringList>)
{
    notYet(tr("Empty folder"));
    emit labelEmptied(id, {});
}
void ImapSyncEngine::unemptyLabel(const QString &, const QStringList &) { notYet(tr("Undo")); }
void ImapSyncEngine::clearImportantFromFiled() { emit importantCleared(0, false); }
void ImapSyncEngine::restoreFolders(const QString &, const QStringList &) { notYet(tr("Undo")); }
void ImapSyncEngine::moveToLabel(const QString &, const QString &) { notYet(tr("Moving mail")); }
void ImapSyncEngine::touchCounts(const QStringList &)
{
    if (m_connected) {
        enqueue([this](Done done) { refreshCounts(done); });
    }
}

} // namespace zmail
