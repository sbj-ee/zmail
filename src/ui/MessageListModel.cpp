#include "MessageListModel.h"

#include "Flags.h"

#include "Icons.h"
#include "Theme.h"
#include "core/SizeFormat.h"

#include <QApplication>
#include <QDateTime>
#include <QFont>
#include <QPalette>
#include <QMimeData>
#include <QSet>

namespace zmail::ui {

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void MessageListModel::setItems(QList<MailItem> items)
{
    // The same messages in the same order (one was marked read, a count
    // changed): update in place. A reset throws away the view's press, so a
    // sync landing while the mouse button was down (every click on unread
    // mail causes one) turned the start of a drag into a rubber-band
    // selection, and a message could not be dragged to a folder (0.5.9).
    bool sameRows = items.size() == m_items.size() && !items.isEmpty();
    for (qsizetype i = 0; sameRows && i < items.size(); ++i) {
        sameRows = items.at(i).id == m_items.at(i).id;
    }
    if (sameRows) {
        m_items = std::move(items);
        emit dataChanged(index(0, 0), index(int(m_items.size()) - 1, ColumnCount - 1));
        return;
    }
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
}

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

int MessageListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QString MessageListModel::statusGlyph(MailStatus s)
{
    switch (s) {
    case MailStatus::Unread: return QStringLiteral("\u2022"); // bullet, as in Eudora
    case MailStatus::Read: return QString();
    case MailStatus::Replied: return QStringLiteral("R");
    case MailStatus::Forwarded: return QStringLiteral("F");
    case MailStatus::Queued: return QStringLiteral("Q");
    case MailStatus::Sent: return QStringLiteral("S");
    }
    return {};
}

QString MessageListModel::formatDate(const QDateTime &dt)
{
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(dt, QStringLiteral("M/d/yy  h:mm AP"));
}

QString MessageListModel::formatSize(qint64 bytes)
{
    return zmail::formatSize(bytes);
}


Qt::ItemFlags MessageListModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && !m_items.at(index.row()).id.isEmpty()) {
        f |= Qt::ItemIsDragEnabled;
    }
    return f;
}

QStringList MessageListModel::mimeTypes() const
{
    return {QString::fromLatin1(kMessageIdsMime)};
}

QMimeData *MessageListModel::mimeData(const QModelIndexList &indexes) const
{
    QSet<QString> ids;
    for (const QModelIndex &i : indexes) {
        if (!i.isValid()) {
            continue;
        }
        const QString id = m_items.at(i.row()).id;
        if (!id.isEmpty()) {
            ids.insert(id);
        }
    }
    if (ids.isEmpty()) {
        return nullptr;
    }
    auto *mime = new QMimeData;
    QByteArray raw;
    for (const QString &id : ids) {
        raw += id.toUtf8();
        raw += char(10);
    }
    mime->setData(QByteArray(kMessageIdsMime), raw);
    return mime;
}

void MessageListModel::paletteChanged()
{
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
    emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return {};
    }
    const MailItem &m = m_items.at(index.row());
    const QPalette pal = QApplication::palette();
    const int col = index.column();

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Status: return m.snoozeBadge ? QString() : statusGlyph(m.status);
        case Priority:
            return m.priority == MailPriority::High ? QStringLiteral("!")
                 : m.priority == MailPriority::Low ? QStringLiteral("\u2193") : QString();
        case Who: return m.who;
        case Date:
            if (m.snoozeWakeMs > 0) {
                return formatDate(QDateTime::fromMSecsSinceEpoch(m.snoozeWakeMs).toLocalTime());
            }
            return formatDate(m.date);
        case Size: return formatSize(m.sizeBytes);
        case Subject: return m.subject;
        default: return {};
        }
    case Qt::DecorationRole:
        if (col == Attachment && m.hasAttachment) {
            return icon(QStringLiteral("paperclip"));
        }
        if (col == Priority && !m.flag.isEmpty()) {
            return icon(QStringLiteral("flag"), flagColor(m.flag));
        }
        if (col == Label && m.labelColor.isValid()) {
            return swatch(m.labelColor);
        }
        if (col == Status && m.suspicious) {
            return icon(QStringLiteral("shield-alert"), suspiciousForeground(pal));
        }
        if (col == Status && m.snoozeBadge) {
            return icon(QStringLiteral("clock"));
        }
        return {};
    case Qt::ForegroundRole: {
        if (m.suspicious) {
            return suspiciousForeground(pal);
        }
        if (col == Priority && m.priority == MailPriority::High) {
            return readableOn(QColor(0xd3, 0x2f, 0x2f), pal.color(QPalette::Base));
        }
        if (m.ruleColor.isValid()) {
            return readableOn(m.ruleColor, rowTint(m.ruleColor, pal.color(QPalette::Base)));
        }
        if (col == Priority || col == Size) {
            return pal.color(QPalette::PlaceholderText);
        }
        return {};
    }
    case Qt::BackgroundRole:
        if (m.suspicious) {
            return suspiciousBackground(pal);
        }
        if (m.ruleColor.isValid()) {
            return rowTint(m.ruleColor, pal.color(QPalette::Base));
        }
        return {};
    case Qt::FontRole:
        if (m.status == MailStatus::Unread || (col == Priority && m.priority == MailPriority::High)) {
            // Bold and nothing else: the view fills in the rest from its
            // own font, so the list's text size (Settings > Message List)
            // applies to unread rows too.
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    case Qt::TextAlignmentRole:
        if (col == Size) {
            return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (col <= Label) {
            return int(Qt::AlignCenter);
        }
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ToolTipRole:
        if (col == Who) {
            return QStringLiteral("%1 <%2>").arg(m.who, m.address);
        }
        if (col == Label) {
            return m.label;
        }
        if (col == Priority) {
            return m.flag.isEmpty() ? tr("Click to flag") : tr("%1 flag. Click to clear it.").arg(flagName(m.flag));
        }
        if (col == Size) {
            // sizeEstimate from Gmail: close to, but not exactly, the raw size.
            return tr("%1 (Gmail's estimate)").arg(zmail::formatExactBytes(m.sizeBytes));
        }
        if (m.suspicious) {
            return tr("Suspicious: see the warning banner in the preview");
        }
        if (m.snoozeWakeMs > 0 && col == Date) {
            return tr("Snoozed until %1")
                .arg(formatDate(QDateTime::fromMSecsSinceEpoch(m.snoozeWakeMs).toLocalTime()));
        }
        if (m.snoozeBadge) {
            return tr("Returned from snooze");
        }
        return {};
    case SortRole:
        switch (col) {
        case Status: return int(m.status);
        // Flagged first when sorted descending, grouped by colour; then priority.
        case Priority: return (m.flag.isEmpty() ? 0 : 100 - flagOrder(m.flag)) * 10 + int(m.priority);
        case Attachment: return m.hasAttachment;
        case Label: return m.label;
        case Who: return m.who.toLower();
        case Date: return m.date.toSecsSinceEpoch();
        case Size: return m.sizeBytes;
        case Subject: return m.subject.toLower();
        default: return {};
        }
    case IdRole: return m.id;
    case SuspiciousRole: return m.suspicious;
    case MailboxesRole: return m.mailboxes;
    case LabelRole: return m.label;
    case SearchTextRole:
        return QStringList{m.who, m.address, m.subject, m.preview, m.label}.join(QLatin1Char('\n'));
    case SnoozeBadgeRole: return m.snoozeBadge;
    case SnoozeWakeRole: return m.snoozeWakeMs;
    default:
        return {};
    }
}

QVariant MessageListModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Who: return tr("Who");
        case Date: return tr("Date");
        case Size: return tr("Size");
        case Subject: return tr("Subject");
        default: return QString();
        }
    }
    if (role == Qt::TextAlignmentRole) {
        // Headers line up with their values: numbers right, icon columns centred.
        if (section == Size) {
            return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (section <= Label) {
            return int(Qt::AlignCenter);
        }
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::DecorationRole) {
        // In the header's text colour (brand themes give headers their own palette).
        const QColor hc = QApplication::palette("QHeaderView").color(QPalette::ButtonText);
        switch (section) {
        case Status: return icon(QStringLiteral("mail"), hc);
        case Priority: return icon(QStringLiteral("flag"), hc);
        case Attachment: return icon(QStringLiteral("paperclip"), hc);
        case Label: return icon(QStringLiteral("tag"), hc);
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Status: return tr("Status: \u2022 unread, R replied, F forwarded, Q queued, S sent");
        case Priority: return tr("Flag: click a row here to flag it, or right-click for a colour");
        case Attachment: return tr("Attachments");
        case Label: return tr("Label");
        case Size: return tr("Message size (Gmail's estimate); hover a row for the exact bytes");
        default: return {};
        }
    }
    return {};
}

MessageFilterProxy::MessageFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(MessageListModel::SortRole);
    setDynamicSortFilter(true);
}

void MessageFilterProxy::setMailbox(const QString &mailbox)
{
    m_mailbox = mailbox;
    invalidateFilter();
}

int MessageListModel::rowForId(const QString &id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

void MessageListModel::setStatus(int row, MailStatus status)
{
    if (row < 0 || row >= m_items.size() || m_items[row].status == status) {
        return;
    }
    m_items[row].status = status;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void MessageFilterProxy::setSearchText(const QString &text)
{
    m_terms = parseSearch(text);
    m_fieldTerms.clear();
    for (const SearchTerm &t : std::as_const(m_terms)) {
        if (t.field != SearchTerm::Any || t.negate) {
            m_fieldTerms << t;
        }
    }
    invalidateFilter();
}

void MessageFilterProxy::setHideSpam(bool hide)
{
    if (m_hideSpam == hide) {
        return;
    }
    m_hideSpam = hide;
    invalidateFilter();
}

void MessageFilterProxy::setSearchIds(const QStringList &ids, bool fullText)
{
    if (m_searchIds == ids && m_searchIdsFullText == fullText) {
        return;
    }
    m_searchIds = ids;
    m_searchIdsFullText = fullText;
    invalidateFilter();
}

QList<SearchTerm> MessageFilterProxy::parseSearch(const QString &text)
{
    // Split on whitespace, keeping "quoted phrases" (also after an operator:
    // subject:"crew schedule") together.
    QStringList tokens;
    QString cur;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            cur += c;
        } else if (c.isSpace() && !quoted) {
            if (!cur.isEmpty()) {
                tokens << cur;
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) {
        tokens << cur;
    }

    static const struct { const char *name; SearchTerm::Field field; } ops[] = {
        {"from", SearchTerm::From}, {"to", SearchTerm::To}, {"subject", SearchTerm::Subject},
        {"label", SearchTerm::Label}};
    auto unquote = [](QString s) { return s.remove(QLatin1Char('"')).trimmed(); };

    QList<SearchTerm> out;
    for (QString tok : tokens) {
        SearchTerm t;
        if (tok.size() > 1 && tok.startsWith(QLatin1Char('-'))) {
            t.negate = true;
            tok.remove(0, 1);
        }
        const int colon = tok.indexOf(QLatin1Char(':'));
        const QString op = colon > 0 ? tok.left(colon).toLower() : QString();
        const QString val = colon > 0 ? unquote(tok.mid(colon + 1)) : QString();
        bool known = false;
        for (const auto &o : ops) {
            if (op == QLatin1String(o.name)) {
                t.field = o.field;
                t.text = val;
                known = true;
                break;
            }
        }
        if (!known && op == QLatin1String("has") &&
            (val.compare(QLatin1String("attachment"), Qt::CaseInsensitive) == 0 ||
             val.compare(QLatin1String("attachments"), Qt::CaseInsensitive) == 0)) {
            t.field = SearchTerm::HasAttachment;
            known = true;
        } else if (!known && op == QLatin1String("is") && val.compare(QLatin1String("unread"), Qt::CaseInsensitive) == 0) {
            t.field = SearchTerm::IsUnread;
            known = true;
        } else if (!known && op == QLatin1String("is") && val.compare(QLatin1String("read"), Qt::CaseInsensitive) == 0) {
            t.field = SearchTerm::IsRead;
            known = true;
        }
        if (!known) {
            t.field = SearchTerm::Any;
            t.text = unquote(tok);
        }
        // "from:" with nothing after it yet (still typing): no filter.
        const bool needsText = t.field != SearchTerm::HasAttachment && t.field != SearchTerm::IsUnread &&
                               t.field != SearchTerm::IsRead;
        if (needsText && t.text.isEmpty()) {
            continue;
        }
        out << t;
    }
    return out;
}

bool MessageFilterProxy::matches(const QList<SearchTerm> &terms, const MailItem &m)
{
    auto has = [](const QString &hay, const QString &needle) { return hay.contains(needle, Qt::CaseInsensitive); };
    // Sent-only rows show the recipient in Who (as Eudora's Out mailbox does).
    const bool whoIsRecipient = m.mailboxes.contains(QStringLiteral("Out")) && !m.mailboxes.contains(QStringLiteral("In"));
    for (const SearchTerm &t : terms) {
        bool hit = false;
        switch (t.field) {
        case SearchTerm::Any:
            hit = has(m.who, t.text) || has(m.address, t.text) || has(m.subject, t.text) || has(m.preview, t.text) ||
                  has(m.label, t.text);
            break;
        case SearchTerm::From:
            hit = !whoIsRecipient && (has(m.who, t.text) || has(m.address, t.text));
            break;
        case SearchTerm::To:
            hit = has(m.to, t.text) || (whoIsRecipient && (has(m.who, t.text) || has(m.address, t.text)));
            break;
        case SearchTerm::Subject: hit = has(m.subject, t.text); break;
        case SearchTerm::Label: hit = has(m.label, t.text); break;
        case SearchTerm::HasAttachment: hit = m.hasAttachment; break;
        case SearchTerm::IsUnread: hit = m.status == MailStatus::Unread; break;
        case SearchTerm::IsRead: hit = m.status != MailStatus::Unread; break;
        }
        if (hit == t.negate) {
            return false;
        }
    }
    return true;
}

QString MessageFilterProxy::searchHelp()
{
    return tr("Search all synced mail (subject, from, to, body).\n"
              "from:name   to:name   subject:word   label:name\n"
              "has:attachment   is:unread   is:read\n"
              "\"exact phrase\"   -word (exclude)");
}

bool MessageFilterProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const QModelIndex idx = sourceModel()->index(row, 0, parent);
    const QStringList boxes = idx.data(MessageListModel::MailboxesRole).toStringList();
    // Trashed mail shows only in Trash, as in Gmail. Gmail keeps a trashed
    // message's other labels (user labels, STARRED, SENT, SPAM), so without
    // this a deleted message stayed in every view but In.
    const bool trashView = m_mailbox == QLatin1String("Trash") || m_mailbox == QLatin1String("gmail:TRASH");
    const bool trashed = boxes.contains(QStringLiteral("Trash"));
    if (trashed && !trashView) {
        return false;
    }
    // Hide Spam: keep SPAM out of every mailbox except Junk itself (and
    // Trash, so mail deleted from Junk can still be found there).
    if (m_hideSpam && m_mailbox != QLatin1String("Junk") && !(trashView && trashed)
        && boxes.contains(QStringLiteral("Junk"))) {
        return false;
    }
    if (m_mailbox == QLatin1String("Snoozed")) {
        if (idx.data(MessageListModel::SnoozeWakeRole).toLongLong() <= 0) {
            return false;
        }
    } else if (m_mailbox.startsWith(QLatin1String("label:"))) {
        if (idx.data(MessageListModel::LabelRole).toString() != m_mailbox.mid(6)) {
            return false;
        }
    } else if (m_mailbox == QLatin1String("Search")) {
        if (!m_searchIds.isEmpty()) {
            if (!m_searchIds.contains(idx.data(MessageListModel::IdRole).toString())) {
                return false;
            }
        } else if (m_terms.isEmpty()) {
            // Sample data / operator-only: match terms against every loaded row.
            return false;
        }
    } else if (!boxes.contains(m_mailbox)) {
        return false;
    }
    // Active snoozes leave In (and other label views) until they wake; a
    // snoozed message that was deleted still shows in Trash.
    if (m_mailbox != QLatin1String("Snoozed") && !trashView
        && idx.data(MessageListModel::SnoozeWakeRole).toLongLong() > 0) {
        return false;
    }
    // The full-text hit list already matched the words to find (over the
    // bodies too, which list rows don't carry): operators and exclusions are left.
    const bool ftsHits = m_mailbox == QLatin1String("Search") && m_searchIdsFullText && !m_searchIds.isEmpty();
    const QList<SearchTerm> &terms = ftsHits ? m_fieldTerms : m_terms;
    if (!terms.isEmpty()) {
        const auto *model = qobject_cast<const MessageListModel *>(sourceModel());
        if (model && row < model->rowCount()) {
            return matches(terms, model->item(row));
        }
    }
    return true;
}

// Sample rows leave the live-data fields (id, to) at their defaults.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
QList<MailItem> sampleMail()
{
    // All people, companies and addresses are fictional (example.* domains,
    // Microsoft's fictional Contoso/Fabrikam/Northwind/Tailspin brands).
    const QDateTime now(QDate(2026, 10, 3), QTime(20, 41));
    const QColor work(0x7b, 0x3f, 0xb5), family(0x00, 0x89, 0x7b), receipts(0xe0, 0x8e, 0x0b),
        travel(0x1e, 0x6f, 0xd9), news(0x78, 0x80, 0x88);
    auto at = [&](int daysAgo, int h, int m) { return QDateTime(now.date().addDays(-daysAgo), QTime(h, m)); };
    const QStringList in{QStringLiteral("In")};

    QList<MailItem> v;
    auto add = [&](MailItem m) { v.append(std::move(m)); };

    add({MailStatus::Unread, MailPriority::High, true, "Work", work, "Priya Raman", "priya.raman@example.com",
         at(0, 20, 12), 421'880, "Q4 budget review \u2014 numbers attached", work, false, in,
         "Hi Alex,\n\nAttached is the Q4 budget draft with the revised travel line. Can you look over tabs 2 and 3 "
         "before Monday's review? The contractor estimate came in about 8% under what we planned.\n\nThanks,\nPriya",
         {"Q4-budget-draft.xlsx (412 K)"}});
    add({MailStatus::Unread, MailPriority::High, false, "", {}, "Contoso Bank Security",
         "alerts@contoso-secure-login.example", at(0, 19, 58), 18'240,
         "URGENT: Verify your account within 24 hours", {}, true, {"In", "Junk"},
         "Dear customer,\n\nWe detected unusual sign-in activity. Your online banking will be suspended unless you "
         "verify your identity within 24 hours.\n\nVerify now: https://contoso-bank.example.com.verify-id.example/login\n\n"
         "Contoso Bank Security Team", {}});
    add({MailStatus::Unread, MailPriority::Normal, false, "Family", family, "Ruth Ellison", "ruth.ellison@example.net",
         at(0, 18, 30), 3'120, "Sunday dinner at our place?", family, false, in,
         "We're thinking pot roast around 5. Bring the kids if they're free. Let me know by Friday!\n\nLove,\nRuth", {}});
    add({MailStatus::Unread, MailPriority::Normal, false, "Family", family, "Kenji Watanabe", "kenji.w@example.org",
         at(0, 16, 4), 4'010, "Grill parts \u2014 which model was it?", family, false, in,
         "Was it the 3-burner or the 4-burner? The hardware store has the igniter kit for both.", {}});
    add({MailStatus::Replied, MailPriority::Normal, false, "Travel", travel, "Marcus Delgado", "marcus.delgado@example.com",
         at(1, 11, 47), 6'340, "Re: Fishing trip \u2014 first weekend in November", {}, false, in,
         "Lake cabin is booked Fri\u2013Sun. I'll bring the boat; you bring the coffee.", {}});
    add({MailStatus::Read, MailPriority::Normal, true, "Receipts", receipts, "Northwind Outfitters",
         "orders@northwind.example", at(1, 9, 15), 53'400, "Your order #NW-48213 has shipped", {}, false, in,
         "Your waders and two fly boxes are on the way. Estimated delivery: Tuesday.", {"invoice-NW-48213.pdf (38 K)"}});
    add({MailStatus::Forwarded, MailPriority::Normal, true, "Work", work, "Hannah Lindqvist", "h.lindqvist@example.com",
         at(2, 15, 22), 3'287'000, "Site visit photos", work, false, in,
         "Photos from Thursday's walk-through. The panel layout is in IMG_2214.", {"site-photos.zip (3,180 K)"}});
    add({MailStatus::Read, MailPriority::Low, false, "Newsletters", news, "The Weekly Solder", "issue@weeklysolder.example",
         at(2, 7, 0), 90'112, "Issue 112: ESP32 power budgets, LoRa range tests", {}, false, in,
         "This week: measuring deep-sleep current, a 12 km LoRa range test, and reader projects.", {}});
    add({MailStatus::Read, MailPriority::Normal, false, "", {}, "Lakeside Family Dental", "frontdesk@lakeside-dental.example",
         at(3, 10, 5), 2'210, "Appointment reminder: Tue Oct 6, 9:30 AM", {}, false, in,
         "Reply C to confirm or call us to reschedule.", {}});
    add({MailStatus::Read, MailPriority::Normal, true, "Travel", travel, "Tailspin Airlines", "noreply@tailspin.example",
         at(3, 6, 40), 34'800, "Your trip to Denver: check-in opens tomorrow", {}, false, in,
         "Flight TS 1187 departs 7:05 AM. Your boarding pass will be ready 24 hours before departure.",
         {"itinerary.pdf (22 K)"}});
    add({MailStatus::Read, MailPriority::Normal, false, "", {}, "Fabrikam Support", "support@fabrikam.example",
         at(4, 14, 31), 7'020, "Ticket #55127 resolved", {}, false, in,
         "Your router firmware issue has been marked resolved. Reply to reopen.", {}});
    add({MailStatus::Read, MailPriority::Normal, false, "", {}, "Elena Petrova", "elena.petrova@example.org",
         at(5, 20, 2), 3'400, "Book club pick for October", {}, false, in,
         "We're reading \"The Long Way Home\". Meeting at the library on the 22nd.", {}});

    add({MailStatus::Queued, MailPriority::Normal, false, "Travel", travel, "Marcus Delgado", "marcus.delgado@example.com",
         QDateTime(now.date().addDays(1), QTime(8, 0)), 2'900, "Tackle list for the lake", {}, false, {"Out"},
         "Scheduled to send tomorrow at 8:00 AM (CT).", {}});
    add({MailStatus::Sent, MailPriority::Normal, false, "Work", work, "Priya Raman", "priya.raman@example.com",
         at(1, 17, 10), 5'100, "Re: Q4 budget kickoff", {}, false, {"Out"}, "Sounds good, I'll review the draft.", {}});
    add({MailStatus::Read, MailPriority::Low, false, "Newsletters", news, "Contoso Rewards", "news@contoso-rewards.example",
         at(6, 8, 0), 61'000, "Your September points summary", {}, false, {"Trash"}, "You earned 1,240 points.", {}});
    return v;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace zmail::ui
