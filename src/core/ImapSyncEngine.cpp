#include "ImapSyncEngine.h"

#include "Log.h"
#include "MessageParser.h"
#include "RawMime.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
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

QJsonObject ImapSyncEngine::fullJson(const QString &id, const QByteArray &raw) const
{
    const CachedMessage before = m_cache->summary(id);
    QJsonObject json;
    json.insert(QStringLiteral("id"), id);
    json.insert(QStringLiteral("threadId"), before.threadId);
    json.insert(QStringLiteral("labelIds"), QJsonArray::fromStringList(before.labels));
    json.insert(QStringLiteral("internalDate"), QString::number(before.internalDateMs));
    json.insert(QStringLiteral("sizeEstimate"), before.size ? before.size : raw.size());
    json.insert(QStringLiteral("payload"), RawMime::toGmailPayload(raw));
    return json;
}

void ImapSyncEngine::fetchRaw(const QString &id, std::function<void(const QByteArray &raw, const QString &error)> cb)
{
    // In front of any sync: the user is waiting for this one.
    enqueue([this, id, cb](Done done) {
        locate({id}, [this, id, cb, done](const Located &where) {
            QString folderLabel;
            qint64 uid = 0;
            for (auto it = where.constBegin(); it != where.constEnd(); ++it) {
                if (it.value().contains(id)) {
                    folderLabel = it.key();
                    uid = it.value().value(id);
                }
            }
            Folder *f = folder(folderLabel);
            if (!f || uid <= 0) {
                cb({}, tr("That message is no longer on the server."));
                done();
                return;
            }
            const int gen = m_generation;
            m_imap.command("UID FETCH " + QByteArray::number(uid) + " (UID X-GM-MSGID BODY.PEEK[])",
                           [this, gen, cb, done](const ImapClient::Reply &fr) {
                if (gen != m_generation) {
                    cb({}, tr("Disconnected."));
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
                cb(raw, raw.isEmpty() ? (fr.ok ? tr("The server sent an empty message.") : fr.text) : QString());
                done();
            });
        });
    }, /*front=*/true);
}

void ImapSyncEngine::fetchFull(const QString &id, FullCb cb)
{
    QPointer<ImapSyncEngine> self(this);
    fetchRaw(id, [self, id, cb](const QByteArray &raw, const QString &error) {
        if (!self) {
            return;
        }
        cb(raw.isEmpty() ? QJsonObject() : self->fullJson(id, raw), error);
    });
}

void ImapSyncEngine::fetchBody(const QString &id, MessageCb cb)
{
    const CachedMessage cached = m_cache->message(id);
    if (cached.hasBody && !cached.messageIdHeader.isEmpty() && cached.extrasVersion >= MailCache::kExtrasVersion) {
        cb(cached, {});
        return;
    }
    fetchRaw(id, [this, id, cb](const QByteArray &raw, const QString &error) {
        if (raw.isEmpty()) {
            cb(m_cache->message(id), error);
            return;
        }
        const QJsonObject json = fullJson(id, raw);
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
    });
}

// ---- Finding messages ------------------------------------------------------

void ImapSyncEngine::locate(const QStringList &ids, std::function<void(const Located &)> then)
{
    // Gmail keeps everything but Trash and Spam in All Mail; look there first.
    QStringList order{QString::fromLatin1(kAllMailLabel), QStringLiteral("TRASH"), QStringLiteral("SPAM")};
    if (!folder(QString::fromLatin1(kAllMailLabel))) {
        order.prepend(QStringLiteral("INBOX"));
    }
    auto result = std::make_shared<Located>();
    auto left = std::make_shared<QStringList>(ids);
    auto step = std::make_shared<std::function<void()>>();
    const int gen = m_generation;
    *step = [this, gen, order, result, left, then, step]() mutable {
        if (gen != m_generation) {
            *step = nullptr;
            return;
        }
        if (left->isEmpty() || order.isEmpty()) {
            then(*result);
            *step = nullptr;
            return;
        }
        const QString label = order.takeFirst();
        Folder *f = folder(label);
        if (!f) {
            (*step)();
            return;
        }
        searchIn(label, *left, [result, left, label, step](const QHash<QString, qint64> &found) {
            if (!found.isEmpty()) {
                (*result)[label] = found;
                for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
                    left->removeAll(it.key());
                }
            }
            if (*step) {
                (*step)();
            }
        });
    };
    (*step)();
}

void ImapSyncEngine::selectFolder(const QString &labelId, std::function<void(bool ok)> then)
{
    Folder *f = folder(labelId);
    if (!f) {
        then(false);
        return;
    }
    if (m_imap.selected() == f->name) {
        then(true);
        return;
    }
    const int gen = m_generation;
    m_imap.command(selectCmd(*f), [this, gen, labelId, then](const ImapClient::Reply &r) {
        if (gen != m_generation) {
            return;
        }
        Folder *ff = folder(labelId);
        if (ff && r.ok) {
            m_imap.setSelected(ff->name);
            bool changed = false;
            applySelect(ff, r, &changed);
        } else {
            m_imap.setSelected({});
        }
        then(ff && r.ok);
    });
}

void ImapSyncEngine::searchIn(const QString &labelId, const QStringList &ids,
                              std::function<void(const QHash<QString, qint64> &)> then)
{
    const int gen = m_generation;
    selectFolder(labelId, [this, gen, ids, then](bool ok) {
        if (!ok) {
            then({});
            return;
        }
        // UID SEARCH OR X-GM-MSGID a OR X-GM-MSGID b X-GM-MSGID c ..., 50 at a time,
        // then UID FETCH (X-GM-MSGID) to pair the UIDs with ids.
        auto found = std::make_shared<QHash<QString, qint64>>();
        auto chunks = std::make_shared<QList<QStringList>>();
        for (int i = 0; i < ids.size(); i += 50) {
            chunks->append(ids.mid(i, 50));
        }
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, gen, found, chunks, then, step]() {
            if (gen != m_generation) {
                *step = nullptr;
                return;
            }
            if (chunks->isEmpty()) {
                then(*found);
                *step = nullptr;
                return;
            }
            const QStringList chunk = chunks->takeFirst();
            QByteArray criteria;
            int terms = 0;
            for (const QString &id : chunk) {
                bool ok = false;
                const quint64 n = id.toULongLong(&ok, 16);
                if (!ok) {
                    continue;
                }
                const QByteArray term = "X-GM-MSGID " + QByteArray::number(n);
                criteria = terms == 0 ? term : "OR " + term + ' ' + criteria;
                ++terms;
            }
            if (terms == 0) {
                (*step)();
                return;
            }
            m_imap.command("UID SEARCH " + criteria, [this, gen, found, step](const ImapClient::Reply &r) {
                if (gen != m_generation) {
                    return;
                }
                const QList<qint64> uids = searchResults(r);
                if (uids.isEmpty()) {
                    (*step)();
                    return;
                }
                QByteArray set;
                for (const qint64 u : uids) {
                    set += (set.isEmpty() ? "" : ",") + QByteArray::number(u);
                }
                m_imap.command("UID FETCH " + set + " (UID X-GM-MSGID)", [this, gen, found, step](const ImapClient::Reply &fr) {
                    if (gen != m_generation) {
                        return;
                    }
                    for (const ImapUntagged &u : fr.untagged) {
                        if (const ImapValue *attrs = fetchAttrs(u)) {
                            const QString id = hexId(attrs->get("X-GM-MSGID"));
                            if (!id.isEmpty() && attrs->get("UID")) {
                                found->insert(id, attrs->get("UID")->num());
                            }
                        }
                    }
                    (*step)();
                });
            });
        };
        (*step)();
    });
}

// ---- Changing mail ---------------------------------------------------------

QByteArray ImapSyncEngine::gmLabel(const QString &labelId) const
{
    static const QHash<QString, QString> sys = {
        {QStringLiteral("INBOX"), QStringLiteral("\\Inbox")},
        {QStringLiteral("IMPORTANT"), QStringLiteral("\\Important")},
        {QStringLiteral("SENT"), QStringLiteral("\\Sent")},
        {QStringLiteral("DRAFT"), QStringLiteral("\\Draft")},
    };
    const QString name = sys.value(labelId, labelId);
    return ImapClient::quote(QString::fromLatin1(ImapClient::encodeMailboxName(name)));
}

void ImapSyncEngine::applyLabels(const QStringList &ids, const QStringList &add, const QStringList &remove,
                                 std::function<void(bool ok, const QString &error)> done)
{
    locate(ids, [this, ids, add, remove, done](const Located &where) {
        if (where.isEmpty()) {
            done(false, tr("Not found on the server."));
            return;
        }
        auto groups = std::make_shared<QList<QPair<QString, QHash<QString, qint64>>>>();
        for (auto it = where.constBegin(); it != where.constEnd(); ++it) {
            groups->append({it.key(), it.value()});
        }
        auto ok = std::make_shared<bool>(true);
        auto error = std::make_shared<QString>();
        auto step = std::make_shared<std::function<void()>>();
        const int gen = m_generation;
        *step = [this, gen, groups, add, remove, ok, error, done, step]() {
            if (gen != m_generation) {
                *step = nullptr;
                return;
            }
            if (groups->isEmpty()) {
                done(*ok, *error);
                *step = nullptr;
                return;
            }
            const auto [label, uidsById] = groups->takeFirst();
            QByteArray set;
            for (const qint64 u : uidsById) {
                set += (set.isEmpty() ? "" : ",") + QByteArray::number(u);
            }
            const bool inBin = label == QLatin1String("TRASH") || label == QLatin1String("SPAM");
            const QStringList ids = uidsById.keys();
            // Leaving Trash / Spam: move it out first (to the Inbox if it is
            // going back there), then the rest of the change in All Mail.
            if (inBin && remove.contains(label)) {
                Folder *to = add.contains(QStringLiteral("INBOX")) ? folder(QStringLiteral("INBOX"))
                                                                    : folder(QString::fromLatin1(kAllMailLabel));
                if (!to) {
                    to = folder(QStringLiteral("INBOX"));
                }
                const QByteArray dest = to ? to->wire : QByteArray("\"INBOX\"");
                selectFolder(label, [this, gen, set, dest, ids, add, remove, label, ok, error, step](bool sel) {
                    if (!sel) {
                        *ok = false;
                        (*step)();
                        return;
                    }
                    m_imap.command(moveCmd(set, dest), [this, gen, ids, add, remove, label, ok, error, step](const ImapClient::Reply &r) {
                        if (gen != m_generation) {
                            return;
                        }
                        if (!r.ok) {
                            *ok = false;
                            *error = r.text;
                            (*step)();
                            return;
                        }
                        QStringList restAdd = add;
                        restAdd.removeAll(QStringLiteral("INBOX"));
                        QStringList restRemove = remove;
                        restRemove.removeAll(label);
                        if (add.contains(QStringLiteral("INBOX")) && !restAdd.isEmpty()) {
                            // Moved to INBOX already; the other labels still to add.
                        }
                        if (restAdd.isEmpty() && restRemove.isEmpty()) {
                            (*step)();
                            return;
                        }
                        applyLabels(ids, restAdd, restRemove, [ok, error, step](bool good, const QString &e) {
                            if (!good) {
                                *ok = false;
                                *error = e;
                            }
                            (*step)();
                        });
                    });
                });
                return;
            }
            // Flags and labels with STORE, then (into Trash / Spam) a MOVE.
            QList<QByteArray> cmds;
            auto flag = [&](const QString &id, const char *f) {
                if (add.contains(id)) {
                    cmds.append(id == QLatin1String("UNREAD") ? "UID STORE " + set + " -FLAGS.SILENT (" + f + ")"
                                                              : "UID STORE " + set + " +FLAGS.SILENT (" + f + ")");
                }
                if (remove.contains(id)) {
                    cmds.append(id == QLatin1String("UNREAD") ? "UID STORE " + set + " +FLAGS.SILENT (" + f + ")"
                                                              : "UID STORE " + set + " -FLAGS.SILENT (" + f + ")");
                }
            };
            flag(QStringLiteral("UNREAD"), "\\Seen");
            flag(QStringLiteral("STARRED"), "\\Flagged");
            static const QStringList special{QStringLiteral("UNREAD"), QStringLiteral("STARRED"), QStringLiteral("TRASH"),
                                             QStringLiteral("SPAM")};
            QByteArray plus;
            QByteArray minus;
            for (const QString &l : add) {
                if (!special.contains(l)) {
                    plus += (plus.isEmpty() ? "" : " ") + gmLabel(l);
                }
            }
            for (const QString &l : remove) {
                if (!special.contains(l)) {
                    minus += (minus.isEmpty() ? "" : " ") + gmLabel(l);
                }
            }
            if (!minus.isEmpty()) {
                cmds.append("UID STORE " + set + " -X-GM-LABELS.SILENT (" + minus + ")");
            }
            if (!plus.isEmpty()) {
                cmds.append("UID STORE " + set + " +X-GM-LABELS.SILENT (" + plus + ")");
            }
            for (const char *bin : {"TRASH", "SPAM"}) {
                const QString b = QString::fromLatin1(bin);
                if (add.contains(b) && label != b) {
                    if (Folder *to = folder(b)) {
                        cmds.append(moveCmd(set, to->wire));
                    }
                }
            }
            if (cmds.isEmpty()) {
                (*step)();
                return;
            }
            selectFolder(label, [this, gen, cmds, ok, error, step](bool sel) {
                if (!sel) {
                    *ok = false;
                    (*step)();
                    return;
                }
                auto pending = std::make_shared<int>(int(cmds.size()));
                for (const QByteArray &c : cmds) {
                    m_imap.command(c, [gen, this, pending, ok, error, step](const ImapClient::Reply &r) {
                        if (gen != m_generation) {
                            return;
                        }
                        if (!r.ok) {
                            *ok = false;
                            *error = r.text.isEmpty() ? tr("Disconnected.") : r.text;
                        }
                        if (--*pending == 0) {
                            (*step)();
                        }
                    });
                }
            });
        };
        (*step)();
    });
}

QByteArray ImapSyncEngine::moveCmd(const QByteArray &set, const QByteArray &destWire) const
{
    if (m_imap.hasCapability("MOVE")) {
        return "UID MOVE " + set + ' ' + destWire;
    }
    // Without MOVE, Gmail still files by label: Trash / Spam are labels too.
    for (const Folder &f : m_folders) {
        if (f.wire == destWire && (f.labelId == QLatin1String("TRASH") || f.labelId == QLatin1String("SPAM"))) {
            return "UID STORE " + set + " +X-GM-LABELS.SILENT (" + (f.labelId == QLatin1String("TRASH") ? "\"\\\\Trash\"" : "\"\\\\Spam\"") + ")";
        }
    }
    return "UID COPY " + set + ' ' + destWire;
}

void ImapSyncEngine::modifyOptimistic(const QStringList &ids, const QStringList &add, const QStringList &remove,
                                      const QString &what, std::function<void(bool ok)> after, bool announce)
{
    if (ids.isEmpty() || (add.isEmpty() && remove.isEmpty())) {
        return;
    }
    // What this edit really changes on each message, so a refusal undoes
    // exactly that and keeps anything that arrived meanwhile.
    QHash<QString, QPair<QStringList, QStringList>> changed;
    {
        const MailCache::Batch batch(*m_cache);
        for (const QString &id : ids) {
            const QStringList before = m_cache->summary(id).labels;
            QStringList a, r;
            for (const QString &l : add) {
                if (!before.contains(l)) {
                    a.append(l);
                }
            }
            for (const QString &l : remove) {
                if (before.contains(l)) {
                    r.append(l);
                }
            }
            changed.insert(id, {a, r});
            m_cache->modifyLabels(id, add, remove);
        }
    }
    if (announce) {
        emit messagesChanged();
    }
    if (!m_connected) {
        // Offline: put it back and say so.
        for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
            m_cache->modifyLabels(it.key(), it.value().second, it.value().first);
        }
        emit syncError(tr("%1: not connected to %2.").arg(what, m_account.imapHost));
        emit messagesChanged();
        if (after) {
            after(false);
        }
        return;
    }
    QStringList touched = add + remove;
    enqueue([this, ids, add, remove, what, after, changed, touched](Done done) {
        applyLabels(ids, add, remove, [this, what, after, changed, touched, done](bool ok, const QString &error) {
            if (!ok) {
                const MailCache::Batch batch(*m_cache);
                for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
                    m_cache->modifyLabels(it.key(), it.value().second, it.value().first);
                }
                emit syncError(tr("%1 failed: %2").arg(what, error.left(200)));
                emit messagesChanged();
            }
            if (after) {
                after(ok);
            }
            refreshCounts(done);
        });
    });
}

QStringList ImapSyncEngine::userLabels(const QStringList &labels) const
{
    QStringList out;
    for (const QString &l : labels) {
        if (Folder *f = const_cast<ImapSyncEngine *>(this)->folder(l); f && f->type == QLatin1String("user")) {
            out.append(l);
        }
    }
    return out;
}

void ImapSyncEngine::markRead(const QString &id)
{
    if (!m_cache->summary(id).unread()) {
        return;
    }
    modifyOptimistic({id}, {}, {QStringLiteral("UNREAD")}, tr("Marking as read"), {}, /*announce=*/false);
}

void ImapSyncEngine::markUnread(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || m.unread()) {
        return;
    }
    modifyOptimistic({id}, {QStringLiteral("UNREAD")}, {}, tr("Marking as unread"));
}

void ImapSyncEngine::markJunk(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || m.labels.contains(QStringLiteral("SPAM"))) {
        return;
    }
    modifyOptimistic({id}, {QStringLiteral("SPAM")}, {QStringLiteral("INBOX")}, tr("Marking as Junk"));
}

void ImapSyncEngine::markNotJunk(const QString &id)
{
    const CachedMessage m = m_cache->summary(id);
    if (m.id.isEmpty() || !m.labels.contains(QStringLiteral("SPAM"))) {
        return;
    }
    modifyOptimistic({id}, {QStringLiteral("INBOX")}, {QStringLiteral("SPAM")}, tr("Marking as Not Junk"));
}

void ImapSyncEngine::snooze(const QString &, qint64) { notYet(tr("Snooze")); }
void ImapSyncEngine::unsnooze(const QString &) { notYet(tr("Snooze")); }
int ImapSyncEngine::wakeDue(qint64) { return 0; }

void ImapSyncEngine::trash(const QString &id)
{
    trashMany({id});
}

void ImapSyncEngine::trashMany(const QStringList &idsIn)
{
    QStringList ids;
    QStringList folders;
    for (const QString &id : idsIn) {
        const CachedMessage m = m_cache->summary(id);
        if (m.id.isEmpty()) {
            continue;
        }
        ids.append(id);
        m_labelsBeforeTrash.insert(id, m.labels);
        for (const QString &l : userLabels(m.labels)) {
            if (!folders.contains(l)) {
                folders.append(l);
            }
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    // Out of the Inbox and out of its folder: Trash is a place of its own.
    modifyOptimistic(ids, {QStringLiteral("TRASH")}, QStringList{QStringLiteral("INBOX")} + folders,
                     tr("Moving to Trash"), [this, ids](bool ok) {
        for (const QString &id : ids) {
            if (ok) {
                emit trashSucceeded(id);
            } else {
                m_labelsBeforeTrash.remove(id);
                emit trashFailed(id, tr("The server refused the move to Trash."));
            }
        }
    });
}

bool ImapSyncEngine::untrash(const QString &id)
{
    if (!m_labelsBeforeTrash.contains(id)) {
        return false;
    }
    const QStringList before = m_labelsBeforeTrash.take(id);
    const QStringList now = m_cache->summary(id).labels;
    QStringList add;
    for (const QString &l : before) {
        if (!now.contains(l) && l != QLatin1String("TRASH")) {
            add.append(l);
        }
    }
    // Queued behind the trash itself, so the two can't cross on the wire.
    modifyOptimistic({id}, add, {QStringLiteral("TRASH")}, tr("Undoing the move to Trash"), [this, id, before](bool ok) {
        if (!ok) {
            m_labelsBeforeTrash.insert(id, before);
        }
    });
    return true;
}

void ImapSyncEngine::setFlag(const QString &id, const QString &color)
{
    const CachedMessage m = m_cache->summary(id);
    if (!m_running || m.id.isEmpty()) {
        return;
    }
    m_cache->setFlag(id, color);
    const QString starred = QStringLiteral("STARRED");
    const bool isStarred = m.labels.contains(starred);
    if (color.isEmpty() && isStarred) {
        modifyOptimistic({id}, {}, {starred}, tr("Clearing the flag"));
    } else if (!color.isEmpty() && !isStarred) {
        modifyOptimistic({id}, {starred}, {}, tr("Flagging"));
    } else {
        emit messagesChanged();
    }
}

void ImapSyncEngine::idsInFolder(const QString &labelId, std::function<void(bool ok, const QHash<qint64, QString> &uidToId,
                                                                            const QHash<qint64, QStringList> &gmLabels)> then)
{
    const int gen = m_generation;
    selectFolder(labelId, [this, gen, then](bool ok) {
        if (!ok) {
            then(false, {}, {});
            return;
        }
        Folder *f = nullptr;
        for (Folder &c : m_folders) {
            if (c.name == m_imap.selected()) {
                f = &c;
            }
        }
        if (!f || f->exists == 0) {
            then(true, {}, {});
            return;
        }
        m_imap.command("UID FETCH 1:* (UID X-GM-MSGID X-GM-LABELS)", [this, gen, then](const ImapClient::Reply &r) {
            if (gen != m_generation) {
                return;
            }
            QHash<qint64, QString> ids;
            QHash<qint64, QStringList> labels;
            for (const ImapUntagged &u : r.untagged) {
                if (const ImapValue *attrs = fetchAttrs(u)) {
                    const qint64 uid = attrs->get("UID") ? attrs->get("UID")->num() : 0;
                    ids.insert(uid, hexId(attrs->get("X-GM-MSGID")));
                    QStringList l;
                    if (const ImapValue *gm = attrs->get("X-GM-LABELS"); gm && gm->isList()) {
                        for (const ImapValue &v : gm->items) {
                            l.append(ImapClient::decodeMailboxName(v.text));
                        }
                    }
                    labels.insert(uid, l);
                }
            }
            then(r.ok, ids, labels);
        });
    });
}

void ImapSyncEngine::emptyTrash()
{
    if (!m_connected) {
        emit trashEmptied(-1);
        return;
    }
    // Like the Gmail API path: zmail hides what is in Trash now; Gmail
    // erases it after 30 days.
    enqueue([this](Done done) {
        idsInFolder(QStringLiteral("TRASH"), [this, done](bool ok, const QHash<qint64, QString> &ids, const QHash<qint64, QStringList> &) {
            if (!ok) {
                emit trashEmptied(-1);
                done();
                return;
            }
            const QStringList list = ids.values();
            m_cache->setPurged(list);
            emit messagesChanged();
            emit labelsChanged();
            emit trashEmptied(int(list.size()));
            done();
        });
    });
}

void ImapSyncEngine::createLabel(const QString &name, const QString &, std::function<void(const QString &id)> then)
{
    if (!m_connected || name.trimmed().isEmpty()) {
        if (then) {
            then({});
        }
        return;
    }
    // Colours are a Gmail API feature; IMAP folders have none.
    const QString clean = name.trimmed();
    enqueue([this, clean, then](Done done) {
        m_imap.command("CREATE " + ImapClient::quote(QString::fromLatin1(ImapClient::encodeMailboxName(clean))),
                       [this, clean, then, done](const ImapClient::Reply &r) {
            if (!r.ok) {
                emit syncError(tr("Creating folder failed: %1").arg(r.text.left(200)));
                if (then) {
                    then({});
                }
                done();
                return;
            }
            listFolders([this, clean, then, done] {
                refreshCounts([this, clean, then, done] {
                    if (then) {
                        then(folder(clean) ? clean : QString());
                    }
                    done();
                });
            });
        });
    });
}

void ImapSyncEngine::renameLabel(const QString &id, const QString &newName)
{
    Folder *f = folder(id);
    if (!m_connected || !f || f->type != QLatin1String("user") || newName.trimmed().isEmpty()) {
        return;
    }
    const QString to = newName.trimmed();
    enqueue([this, id, to](Done done) {
        Folder *f = folder(id);
        if (!f) {
            done();
            return;
        }
        if (m_imap.selected() == f->name) {
            m_imap.setSelected({});
        }
        m_imap.command("RENAME " + f->wire + ' ' + ImapClient::quote(QString::fromLatin1(ImapClient::encodeMailboxName(to))),
                       [this, id, to, done](const ImapClient::Reply &r) {
            if (!r.ok) {
                emit syncError(tr("Renaming folder failed: %1").arg(r.text.left(200)));
                done();
                return;
            }
            {
                const MailCache::Batch batch(*m_cache);
                for (const QString &messageId : m_cache->messageIds(id)) {
                    m_cache->modifyLabels(messageId, {to}, {id});
                }
            }
            listFolders([this, done] {
                refreshCounts(done);
                emit messagesChanged();
            });
        });
    });
}

void ImapSyncEngine::deleteLabel(const QString &id)
{
    Folder *f = folder(id);
    if (!m_connected || !f || f->type != QLatin1String("user")) {
        return;
    }
    // Gmail: deleting a label folder removes the label; the mail stays in All Mail.
    enqueue([this, id](Done done) {
        Folder *f = folder(id);
        if (!f) {
            done();
            return;
        }
        if (m_imap.selected() == f->name) {
            m_imap.setSelected({});
        }
        m_imap.command("DELETE " + f->wire, [this, id, done](const ImapClient::Reply &r) {
            if (!r.ok) {
                emit syncError(tr("Deleting folder failed: %1").arg(r.text.left(200)));
                done();
                return;
            }
            {
                const MailCache::Batch batch(*m_cache);
                for (const QString &messageId : m_cache->messageIds(id)) {
                    m_cache->modifyLabels(messageId, {}, {id});
                }
            }
            listFolders([this, done] {
                emit messagesChanged();
                refreshCounts(done);
            });
        });
    });
}

void ImapSyncEngine::emptyLabel(const QString &id, int, std::shared_ptr<QStringList>)
{
    if (!m_connected || !folder(id)) {
        emit labelEmptied(id, {});
        return;
    }
    // Every message in the folder goes to Trash (and out of the folder and Inbox).
    enqueue([this, id](Done done) {
        idsInFolder(id, [this, id, done](bool ok, const QHash<qint64, QString> &uidToId, const QHash<qint64, QStringList> &) {
            const QStringList ids = uidToId.values();
            if (!ok || ids.isEmpty()) {
                if (!ok) {
                    emit syncError(tr("Emptying the folder failed."));
                }
                emit labelEmptied(id, {});
                done();
                return;
            }
            {
                const MailCache::Batch batch(*m_cache);
                for (const QString &m : ids) {
                    if (m_cache->contains(m)) {
                        m_cache->modifyLabels(m, {QStringLiteral("TRASH")}, {id, QStringLiteral("INBOX")});
                    }
                }
            }
            emit messagesChanged();
            QByteArray set;
            for (auto it = uidToId.constBegin(); it != uidToId.constEnd(); ++it) {
                set += (set.isEmpty() ? "" : ",") + QByteArray::number(it.key());
            }
            Folder *trashFolder = folder(QStringLiteral("TRASH"));
            m_imap.command(moveCmd(set, trashFolder ? trashFolder->wire : QByteArray("\"[Gmail]/Trash\"")),
                           [this, id, ids, done](const ImapClient::Reply &r) {
                if (!r.ok) {
                    emit syncError(tr("Emptying the folder failed: %1").arg(r.text.left(200)));
                }
                emit labelEmptied(id, r.ok ? ids : QStringList());
                refreshCounts(done);
            });
        });
    });
}

void ImapSyncEngine::unemptyLabel(const QString &id, const QStringList &messageIds)
{
    modifyOptimistic(messageIds, {id}, {QStringLiteral("TRASH")}, tr("Undoing Empty Folder"));
}

void ImapSyncEngine::clearImportantFromFiled()
{
    if (!m_connected || !folder(QStringLiteral("IMPORTANT"))) {
        emit importantCleared(0, false);
        return;
    }
    enqueue([this](Done done) {
        idsInFolder(QStringLiteral("IMPORTANT"), [this, done](bool ok, const QHash<qint64, QString> &ids,
                                                             const QHash<qint64, QStringList> &labels) {
            if (!ok) {
                emit importantCleared(0, false);
                done();
                return;
            }
            QByteArray set;
            QStringList cleared;
            for (auto it = labels.constBegin(); it != labels.constEnd(); ++it) {
                bool filed = false;
                bool inbox = false;
                for (const QString &l : it.value()) {
                    inbox = inbox || l == QLatin1String("\\Inbox");
                    filed = filed || !l.startsWith(QLatin1Char('\\'));
                }
                if (filed && !inbox) {
                    set += (set.isEmpty() ? "" : ",") + QByteArray::number(it.key());
                    cleared.append(ids.value(it.key()));
                }
            }
            if (cleared.isEmpty()) {
                emit importantCleared(0, true);
                done();
                return;
            }
            m_imap.command("UID STORE " + set + " -X-GM-LABELS.SILENT (\"\\\\Important\")",
                           [this, cleared, done](const ImapClient::Reply &r) {
                if (r.ok) {
                    const MailCache::Batch batch(*m_cache);
                    for (const QString &m : cleared) {
                        m_cache->modifyLabels(m, {}, {QStringLiteral("IMPORTANT")});
                    }
                }
                emit messagesChanged();
                emit importantCleared(r.ok ? int(cleared.size()) : 0, r.ok);
                refreshCounts(done);
            });
        });
    });
}

void ImapSyncEngine::restoreFolders(const QString &messageId, const QStringList &before)
{
    const CachedMessage now = m_cache->summary(messageId);
    if (!m_running || now.id.isEmpty()) {
        return;
    }
    QStringList folders{QStringLiteral("INBOX"), QStringLiteral("IMPORTANT")};
    for (const CachedLabel &l : m_cache->labels()) {
        if (l.type == QLatin1String("user")) {
            folders.append(l.id);
        }
    }
    QStringList add, remove;
    for (const QString &l : std::as_const(folders)) {
        const bool was = before.contains(l);
        const bool is = now.labels.contains(l);
        if (was && !is) {
            add.append(l);
        } else if (!was && is) {
            remove.append(l);
        }
    }
    modifyOptimistic({messageId}, add, remove, tr("Undoing the move"));
}

void ImapSyncEngine::moveToLabel(const QString &messageId, const QString &targetLabelId)
{
    const CachedMessage before = m_cache->summary(messageId);
    if (!m_running || before.id.isEmpty() || targetLabelId.isEmpty()) {
        return;
    }
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
    const QString important = QStringLiteral("IMPORTANT");
    if (targetLabelId != inbox && before.labels.contains(important)) {
        remove.append(important);
    }
    modifyOptimistic({messageId}, add, remove, tr("Moving to folder"));
}

void ImapSyncEngine::touchCounts(const QStringList &)
{
    if (m_connected) {
        enqueue([this](Done done) { refreshCounts(done); });
    }
}

void ImapSyncEngine::notYet(const QString &what)
{
    emit statusChanged(tr("%1 isn't available for app-password accounts yet; it comes in the next zmail update.").arg(what));
}

// ---- Gmail-syntax search (X-GM-RAW) ---------------------------------------

void ImapSyncEngine::serverSearch(const QString &query, SearchCb cb)
{
    const QString q = query.simplified();
    if (!m_connected || q.isEmpty()) {
        cb({}, m_connected ? QString() : tr("Not connected."));
        return;
    }
    enqueue([this, q, cb](Done done) {
        const QString label = folder(QString::fromLatin1(kAllMailLabel)) ? QString::fromLatin1(kAllMailLabel)
                                                                        : QStringLiteral("INBOX");
        const int gen = m_generation;
        selectFolder(label, [this, gen, q, cb, done, label](bool ok) {
            if (!ok) {
                cb({}, tr("Couldn't open All Mail."));
                done();
                return;
            }
            m_imap.command("UID SEARCH CHARSET UTF-8 X-GM-RAW " + ImapClient::quote(q),
                           [this, gen, cb, done, label](const ImapClient::Reply &r) {
                if (gen != m_generation) {
                    return;
                }
                if (!r.ok) {
                    cb({}, r.text.left(200));
                    done();
                    return;
                }
                QList<qint64> uids = searchResults(r);
                std::sort(uids.begin(), uids.end());
                uids = uids.mid(std::max<qsizetype>(0, uids.size() - kServerSearchLimit)); // newest
                if (uids.isEmpty()) {
                    cb({}, {});
                    done();
                    return;
                }
                QByteArray set;
                for (const qint64 u : uids) {
                    set += (set.isEmpty() ? "" : ",") + QByteArray::number(u);
                }
                m_imap.command("UID FETCH " + set + " (" + kMetaItems + ")", [this, gen, cb, done, label](const ImapClient::Reply &fr) {
                    if (gen != m_generation) {
                        return;
                    }
                    QStringList ids;
                    {
                        const MailCache::Batch batch(*m_cache);
                        for (const ImapUntagged &u : fr.untagged) {
                            if (const ImapValue *attrs = fetchAttrs(u)) {
                                const CachedMessage m = messageFromFetch(*attrs, label);
                                if (m.id.isEmpty()) {
                                    continue;
                                }
                                if (m_cache->contains(m.id)) {
                                    m_cache->setLabels(m.id, m.labels); // keep its body
                                } else {
                                    m_cache->upsert(m);
                                }
                                ids.append(m.id);
                            }
                        }
                    }
                    emit messagesChanged();
                    cb(ids, fr.ok ? QString() : fr.text);
                    done();
                });
            });
        });
    }, /*front=*/true);
}

} // namespace zmail
