#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QDateTime>
#include <QList>
#include <QSortFilterProxyModel>
#include <QStringList>

namespace zmail::ui {

enum class MailStatus { Unread, Read, Replied, Forwarded, Queued, Sent };
enum class MailPriority { Low = 0, Normal = 1, High = 2 };

struct MailItem
{
    MailStatus status = MailStatus::Read;
    MailPriority priority = MailPriority::Normal;
    bool hasAttachment = false;
    QString label;          // Gmail label shown as a colour swatch ("" = none)
    QColor labelColor;
    QString who;            // sender (or recipient in Out)
    QString address;
    QDateTime date;
    qint64 sizeBytes = 0;
    QString subject;
    QColor ruleColor;       // colour from the first matching rule (invalid = none)
    bool suspicious = false;
    QStringList mailboxes;  // "In", "Out", "Junk", "Trash"
    QString preview;        // plain-text body for the preview pane
    QStringList attachments;
    // Live Gmail data (empty for sample data).
    QString id;             // Gmail message id
    QString to;             // To: header
};

// Eudora-style columns.
class MessageListModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Status, Priority, Attachment, Label, Who, Date, Size, Subject, ColumnCount };
    static constexpr int SortRole = Qt::UserRole + 1;
    static constexpr int SuspiciousRole = Qt::UserRole + 2;
    static constexpr int MailboxesRole = Qt::UserRole + 3;
    static constexpr int LabelRole = Qt::UserRole + 4;
    static constexpr int SearchTextRole = Qt::UserRole + 5;

    explicit MessageListModel(QObject *parent = nullptr);

    void setItems(QList<MailItem> items);
    const QList<MailItem> &items() const { return m_items; }
    const MailItem &item(int row) const { return m_items.at(row); }
    int rowForId(const QString &id) const;
    void setStatus(int row, MailStatus status);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    static QString statusGlyph(MailStatus s);
    static QString formatDate(const QDateTime &dt);
    // "3 KB", "1.2 MB" (zmail::formatSize); the exact count is in the tooltip.
    static QString formatSize(qint64 bytes);

    // Call after a palette change so colours are recomputed.
    void paletteChanged();

private:
    QList<MailItem> m_items;
};

// One term of the search box. Gmail-style operators, matched locally
// against the cached rows (case-insensitive substring):
//   from:priya   to:dana   subject:"crew schedule"   label:work
//   has:attachment   is:unread   is:read
// Bare words and "quoted phrases" match sender, address, subject, body
// preview and label. A leading '-' negates a term (-from:newsletter).
// Unknown "word:" tokens (8:30, http://...) are searched as plain text.
struct SearchTerm
{
    enum Field { Any, From, To, Subject, Label, HasAttachment, IsUnread, IsRead };
    Field field = Any;
    QString text;
    bool negate = false;
    bool operator==(const SearchTerm &o) const { return field == o.field && text == o.text && negate == o.negate; }
};

// Mailbox ("In", "Out", ..., or "label:<name>") + search filter.
class MessageFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit MessageFilterProxy(QObject *parent = nullptr);
    void setMailbox(const QString &mailbox);
    QString mailbox() const { return m_mailbox; }
    void setSearchText(const QString &text);

    static QList<SearchTerm> parseSearch(const QString &text);
    static bool matches(const QList<SearchTerm> &terms, const MailItem &m);
    // The operators parseSearch() understands, for the search box tooltip.
    static QString searchHelp();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    QString m_mailbox = QStringLiteral("In");
    QList<SearchTerm> m_terms;
};

// Fake, realistic-looking sample data (fictional people and companies only).
QList<MailItem> sampleMail();

} // namespace zmail::ui
