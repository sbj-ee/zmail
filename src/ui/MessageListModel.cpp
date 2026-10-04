#include "MessageListModel.h"

#include "Icons.h"
#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace zmail::ui {

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void MessageListModel::setItems(QList<MailItem> items)
{
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

QString MessageListModel::formatSizeK(qint64 bytes)
{
    return QString::number(std::max<qint64>(1, (bytes + 1023) / 1024));
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
        case Status: return statusGlyph(m.status);
        case Priority:
            return m.priority == MailPriority::High ? QStringLiteral("!")
                 : m.priority == MailPriority::Low ? QStringLiteral("\u2193") : QString();
        case Who: return m.who;
        case Date: return formatDate(m.date);
        case Size: return formatSizeK(m.sizeBytes);
        case Subject: return m.subject;
        default: return {};
        }
    case Qt::DecorationRole:
        if (col == Attachment && m.hasAttachment) {
            return icon(QStringLiteral("paperclip"));
        }
        if (col == Label && m.labelColor.isValid()) {
            return swatch(m.labelColor);
        }
        if (col == Status && m.suspicious) {
            return icon(QStringLiteral("shield-alert"), suspiciousForeground(pal));
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
            QFont f = QApplication::font();
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
        if (m.suspicious) {
            return tr("Suspicious: see the warning banner in the preview");
        }
        return {};
    case SortRole:
        switch (col) {
        case Status: return int(m.status);
        case Priority: return int(m.priority);
        case Attachment: return m.hasAttachment;
        case Label: return m.label;
        case Who: return m.who.toLower();
        case Date: return m.date.toSecsSinceEpoch();
        case Size: return m.sizeBytes;
        case Subject: return m.subject.toLower();
        default: return {};
        }
    case SuspiciousRole: return m.suspicious;
    case MailboxesRole: return m.mailboxes;
    case LabelRole: return m.label;
    case SearchTextRole:
        return QStringList{m.who, m.address, m.subject, m.preview, m.label}.join(QLatin1Char('\n'));
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
        case Size: return tr("K");
        case Subject: return tr("Subject");
        default: return QString();
        }
    }
    if (role == Qt::DecorationRole) {
        switch (section) {
        case Status: return icon(QStringLiteral("mail"));
        case Priority: return icon(QStringLiteral("flag"));
        case Attachment: return icon(QStringLiteral("paperclip"));
        case Label: return icon(QStringLiteral("tag"));
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Status: return tr("Status: \u2022 unread, R replied, F forwarded, Q queued, S sent");
        case Priority: return tr("Priority");
        case Attachment: return tr("Attachments");
        case Label: return tr("Label");
        case Size: return tr("Size in kilobytes");
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

void MessageFilterProxy::setSearchText(const QString &text)
{
    m_search = text.trimmed();
    invalidateFilter();
}

bool MessageFilterProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const QModelIndex idx = sourceModel()->index(row, 0, parent);
    if (m_mailbox.startsWith(QLatin1String("label:"))) {
        if (idx.data(MessageListModel::LabelRole).toString() != m_mailbox.mid(6)) {
            return false;
        }
    } else if (!idx.data(MessageListModel::MailboxesRole).toStringList().contains(m_mailbox)) {
        return false;
    }
    if (!m_search.isEmpty()
        && !idx.data(MessageListModel::SearchTextRole).toString().contains(m_search, Qt::CaseInsensitive)) {
        return false;
    }
    return true;
}

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

} // namespace zmail::ui
