#include "ContactsWindow.h"
#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "Icons.h"

#include <QAbstractButton>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QSaveFile>
#include <QPainter>
#include <QFileDialog>
#include <QDir>
#include <QDate>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace zmail::ui {

namespace {
constexpr int kListLimit = 2000;

constexpr int kNameRole = Qt::UserRole + 1;
constexpr int kDetailRole = Qt::UserRole + 2;

// One contact per row: the name in bold over its addresses and categories,
// with a rule underneath so it is plain where one contact ends and the next
// begins.
class ContactRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        QFont bold = option.font;
        bold.setBold(true);
        return QSize(120, QFontMetrics(bold).height() + option.fontMetrics.height() + 2 * kPad + 3);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        o.text.clear(); // the panel (selection, hover) only
        const QWidget *w = o.widget;
        (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &o, p, w);
        const bool selected = o.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = (o.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const QRect r = o.rect.adjusted(8, kPad, -8, -kPad - 1);
        p->save();
        QFont bold = o.font;
        bold.setBold(true);
        const QFontMetrics bm(bold);
        p->setFont(bold);
        p->setPen(o.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
        p->drawText(QRect(r.left(), r.top(), r.width(), bm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    bm.elidedText(index.data(kNameRole).toString(), Qt::ElideRight, r.width()));
        p->setFont(o.font);
        QColor dim = o.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        dim.setAlphaF(selected ? 0.85 : 0.65);
        p->setPen(dim);
        p->drawText(QRect(r.left(), r.top() + bm.height() + 1, r.width(), o.fontMetrics.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    o.fontMetrics.elidedText(index.data(kDetailRole).toString(), Qt::ElideRight, r.width()));
        p->setPen(o.palette.color(QPalette::Mid));
        p->drawLine(o.rect.bottomLeft(), o.rect.bottomRight());
        p->restore();
    }

private:
    static constexpr int kPad = 5;
};

bool isCategory(const QString &group)
{
    return !group.startsWith(QLatin1Char('\x01'));
}

QString sourceText(const Contact &c)
{
    if (c.source == QLatin1String("google")) {
        return ContactsWindow::tr("Google Contacts");
    }
    if (c.source == QLatin1String("other")) {
        return ContactsWindow::tr("Google \"Other contacts\" (saved automatically by Gmail)");
    }
    return ContactsWindow::tr("Saved in zmail");
}
} // namespace

ContactsWindow::ContactsWindow(ContactStore *store, ContactsSync *sync, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_sync(sync)
{
    setObjectName(QStringLiteral("contactsWindow"));
    setWindowTitle(tr("Contacts"));
    resize(1040, 620);
    auto *lay = new QVBoxLayout(this);
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName(QStringLiteral("contactsSplitter"));
    split->setChildrenCollapsible(false);
    lay->addWidget(split, 1);

    // ---- left: categories ----
    auto *left = new QWidget(split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    m_groups = new QListWidget(left);
    m_groups->setObjectName(QStringLiteral("contactGroups"));
    m_groups->setIconSize(QSize(14, 14));
    ll->addWidget(m_groups, 1);
    auto *groupButtons = new QHBoxLayout;
    auto *newCat = new QPushButton(tr("New…"), left);
    newCat->setObjectName(QStringLiteral("newCategoryButton"));
    newCat->setToolTip(tr("New category"));
    m_renameCategory = new QPushButton(tr("Rename…"), left);
    m_renameCategory->setObjectName(QStringLiteral("renameCategoryButton"));
    m_deleteCategory = new QPushButton(tr("Delete"), left);
    m_deleteCategory->setObjectName(QStringLiteral("deleteCategoryButton"));
    m_deleteCategory->setToolTip(tr("Delete the category. Its contacts are kept."));
    for (QPushButton *b : {newCat, m_renameCategory, m_deleteCategory}) {
        b->setAutoDefault(false);
        groupButtons->addWidget(b);
    }
    ll->addLayout(groupButtons);

    // ---- middle: contacts ----
    auto *middle = new QWidget(split);
    auto *ml = new QVBoxLayout(middle);
    ml->setContentsMargins(0, 0, 0, 0);
    m_search = new QLineEdit(middle);
    m_search->setObjectName(QStringLiteral("contactsSearch"));
    m_search->setPlaceholderText(tr("Search names, addresses, fields and comments"));
    m_search->setClearButtonEnabled(true);
    ml->addWidget(m_search);
    m_list = new QListWidget(middle);
    m_list->setObjectName(QStringLiteral("contactsList"));
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setItemDelegate(new ContactRowDelegate(m_list));
    m_list->setUniformItemSizes(true);
    ml->addWidget(m_list, 1);
    auto *listButtons = new QHBoxLayout;
    m_hide = new QPushButton(middle);
    m_hide->setObjectName(QStringLiteral("hideContactsButton"));
    m_hide->setAutoDefault(false);
    m_categorize = new QPushButton(tr("Add to Category"), middle);
    m_categorize->setObjectName(QStringLiteral("categorizeButton"));
    m_categorize->setAutoDefault(false);
    listButtons->addWidget(m_hide);
    listButtons->addWidget(m_categorize);
    listButtons->addStretch(1);
    ml->addLayout(listButtons);
    m_status = new QLabel(middle);
    m_status->setObjectName(QStringLiteral("contactsStatus"));
    ml->addWidget(m_status);

    // ---- right: the chosen contact ----
    m_detail = new QWidget(split);
    m_detail->setObjectName(QStringLiteral("contactDetail"));
    auto *dl = new QVBoxLayout(m_detail);
    dl->setContentsMargins(6, 0, 0, 0);
    m_name = new QLabel(m_detail);
    m_name->setObjectName(QStringLiteral("contactName"));
    m_name->setTextFormat(Qt::PlainText);
    m_name->setWordWrap(true);
    QFont nf = m_name->font();
    nf.setBold(true);
    nf.setPointSizeF(nf.pointSizeF() * 1.2);
    m_name->setFont(nf);
    dl->addWidget(m_name);
    m_emails = new QLabel(m_detail);
    m_emails->setObjectName(QStringLiteral("contactEmails"));
    m_emails->setTextFormat(Qt::PlainText);
    m_emails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_emails->setWordWrap(true);
    dl->addWidget(m_emails);
    m_source = new QLabel(m_detail);
    m_source->setObjectName(QStringLiteral("contactSource"));
    m_source->setForegroundRole(QPalette::PlaceholderText);
    m_source->setWordWrap(true);
    dl->addWidget(m_source);
    auto *nickRow = new QFormLayout;
    m_nickname = new QLineEdit(m_detail);
    m_nickname->setObjectName(QStringLiteral("contactNickname"));
    m_nickname->setPlaceholderText(tr("A short name to type in To instead of the address"));
    m_nickname->setToolTip(tr("Type the nickname in To, Cc or Bcc and zmail fills in the address. "
                              "A category's name works the same way, for everyone in it."));
    nickRow->addRow(tr("Nickname:"), m_nickname);
    dl->addLayout(nickRow);
    m_hidden = new QCheckBox(tr("Hide this contact"), m_detail);
    m_hidden->setObjectName(QStringLiteral("contactHidden"));
    m_hidden->setToolTip(tr("Hidden contacts are left out of this list and are not suggested when you write a message."));
    dl->addWidget(m_hidden);

    dl->addWidget(new QLabel(tr("Categories:"), m_detail));
    m_categoryChecks = new QListWidget(m_detail);
    m_categoryChecks->setObjectName(QStringLiteral("contactCategories"));
    m_categoryChecks->setMaximumHeight(120);
    dl->addWidget(m_categoryChecks);

    auto *fieldsHead = new QHBoxLayout;
    fieldsHead->addWidget(new QLabel(tr("Fields:"), m_detail));
    fieldsHead->addStretch(1);
    auto *addField = new QPushButton(tr("Add Field"), m_detail);
    addField->setObjectName(QStringLiteral("addFieldButton"));
    addField->setAutoDefault(false);
    auto *fieldMenu = new QMenu(addField);
    for (const QString &preset : {tr("Phone"), tr("Mobile"), tr("Company"), tr("Title"), tr("Address"),
                                  tr("Birthday"), tr("Website")}) {
        fieldMenu->addAction(preset, this, [this, preset]() { this->addField(preset); });
    }
    fieldMenu->addSeparator();
    fieldMenu->addAction(tr("Other…"), this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Add Field"), tr("Field name:"), QLineEdit::Normal, {}, &ok);
        if (ok) {
            this->addField(name);
        }
    });
    addField->setMenu(fieldMenu);
    m_removeField = new QPushButton(tr("Remove"), m_detail);
    m_removeField->setObjectName(QStringLiteral("removeFieldButton"));
    m_removeField->setAutoDefault(false);
    fieldsHead->addWidget(addField);
    fieldsHead->addWidget(m_removeField);
    dl->addLayout(fieldsHead);
    m_fields = new QTableWidget(0, 2, m_detail);
    m_fields->setObjectName(QStringLiteral("contactFields"));
    m_fields->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    m_fields->horizontalHeader()->setStretchLastSection(true);
    m_fields->verticalHeader()->hide();
    m_fields->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fields->setSelectionMode(QAbstractItemView::SingleSelection);
    dl->addWidget(m_fields, 1);

    dl->addWidget(new QLabel(tr("Comment:"), m_detail));
    m_comment = new QPlainTextEdit(m_detail);
    m_comment->setObjectName(QStringLiteral("contactComment"));
    m_comment->setPlaceholderText(tr("Notes about this contact, kept on this computer"));
    m_comment->setTabChangesFocus(true);
    dl->addWidget(m_comment, 1);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
    split->setSizes({210, 400, 400});

    auto *row = new QHBoxLayout;
    auto *syncBtn = new QPushButton(tr("Sync from Google"), this);
    syncBtn->setObjectName(QStringLiteral("contactsSyncButton"));
    syncBtn->setAutoDefault(false);
    syncBtn->setEnabled(m_sync != nullptr);
    row->addWidget(syncBtn);
    auto *exportBtn = new QPushButton(tr("Export"), this);
    exportBtn->setObjectName(QStringLiteral("contactsExportButton"));
    exportBtn->setAutoDefault(false);
    exportBtn->setToolTip(tr("Save contacts, with their categories, fields and comments, as a JSON file"));
    auto *exportMenu = new QMenu(exportBtn);
    const auto ask = [this](bool everything) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Contacts"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                .filePath(QStringLiteral("zmail-contacts-%1.json").arg(QDate::currentDate().toString(Qt::ISODate))),
            tr("JSON files (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        if (!exportJson(path, everything)) {
            QMessageBox::warning(this, tr("Export Contacts"), tr("Couldn't write %1.").arg(path));
        }
    };
    exportMenu->addAction(tr("The Contacts Shown\u2026"), this, [ask]() { ask(false); })
        ->setObjectName(QStringLiteral("actionExportShown"));
    exportMenu->addAction(tr("All Contacts, Hidden Ones Too\u2026"), this, [ask]() { ask(true); })
        ->setObjectName(QStringLiteral("actionExportAll"));
    exportBtn->setMenu(exportMenu);
    row->addWidget(exportBtn);
    auto *local = new QLabel(tr("Categories, fields, comments and hidden contacts stay on this computer."), this);
    local->setForegroundRole(QPalette::PlaceholderText);
    row->addWidget(local, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    row->addWidget(buttons);
    lay->addLayout(row);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refreshList(); });
    connect(syncBtn, &QPushButton::clicked, this, &ContactsWindow::syncNow);
    connect(m_groups, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (it && !m_loading) {
            showGroup(it->data(Qt::UserRole).toString());
        }
    });
    connect(newCat, &QPushButton::clicked, this, [this]() { newCategory(askCategoryName(tr("New Category"))); });
    connect(m_renameCategory, &QPushButton::clicked, this, [this]() {
        if (isCategory(m_group)) {
            renameCategory(m_group, askCategoryName(tr("Rename Category"), m_group));
        }
    });
    connect(m_deleteCategory, &QPushButton::clicked, this, [this]() {
        if (!isCategory(m_group)) {
            return;
        }
        const auto choice = QMessageBox::question(
            this, tr("Delete Category"),
            tr("Delete the category \"%1\"?\n\nIts contacts are kept; they just leave this category.").arg(m_group),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice == QMessageBox::Yes) {
            deleteCategory(m_group);
        }
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        if (!m_loading) {
            showDetail();
        }
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, &ContactsWindow::showListMenu);
    connect(m_hide, &QPushButton::clicked, this,
            [this]() { setSelectedHidden(m_group != QLatin1String(kHidden)); });
    connect(m_categorize, &QPushButton::clicked, this, [this]() {
        QMenu *menu = categorizeMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_categorize->mapToGlobal(QPoint(0, m_categorize->height())));
    });
    connect(m_hidden, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_loading && !m_detailId.isEmpty()) {
            m_store->setHidden({m_detailId}, on);
            refresh(); // it leaves (or joins) the list on show
        }
    });
    connect(m_categoryChecks, &QListWidget::itemChanged, this, [this]() {
        if (!m_loading) {
            saveCategories();
        }
    });
    connect(m_fields, &QTableWidget::itemChanged, this, [this]() {
        if (!m_loading) {
            saveFields();
        }
    });
    connect(m_fields, &QTableWidget::itemSelectionChanged, this,
            [this]() { m_removeField->setEnabled(m_fields->currentRow() >= 0); });
    connect(m_removeField, &QPushButton::clicked, this, &ContactsWindow::removeCurrentField);
    connect(m_nickname, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (!m_loading && !m_detailId.isEmpty()) {
            m_store->setNickname(m_detailId, text);
        }
    });
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this]() {
        if (!m_loading && !m_detailId.isEmpty()) {
            m_store->setComment(m_detailId, m_comment->toPlainText());
        }
    });
    if (m_sync) {
        connect(m_sync, &ContactsSync::finished, this, [this](bool ok, const QString &msg) {
            if (ok) {
                refresh();
            }
            m_status->setText(msg);
        });
        connect(m_sync, &ContactsSync::progress, this, [this](const QString &s) { m_status->setText(s); });
    }
    refresh();
}

ContactQuery ContactsWindow::query() const
{
    ContactQuery q;
    q.search = m_search->text();
    q.limit = kListLimit;
    if (m_group == QLatin1String(kHidden)) {
        q.show = ContactQuery::Show::Hidden;
    } else if (m_group == QLatin1String(kUncategorized)) {
        q.uncategorized = true;
    } else if (isCategory(m_group)) {
        q.category = m_group;
    }
    return q;
}

void ContactsWindow::refresh()
{
    if (!m_store || !m_store->isOpen()) {
        m_list->clear();
        m_groups->clear();
        m_detail->setEnabled(false);
        m_status->setText(tr("No contacts store."));
        return;
    }
    refreshGroups();
    refreshList();
}

void ContactsWindow::refreshGroups()
{
    const QList<CategoryCount> cats = m_store->categories();
    if (isCategory(m_group)) {
        // Renamed or deleted from under us: back to everything.
        bool found = false;
        for (const CategoryCount &c : cats) {
            if (c.name.compare(m_group, Qt::CaseInsensitive) == 0) {
                m_group = c.name;
                found = true;
            }
        }
        if (!found) {
            m_group = QString::fromLatin1(kAll);
        }
    }
    const bool was = std::exchange(m_loading, true);
    m_groups->clear();
    const auto add = [this](const QString &text, int n, const QString &key) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(text).arg(n), m_groups);
        it->setData(Qt::UserRole, key);
        if (key == m_group) {
            m_groups->setCurrentItem(it);
        }
        return it;
    };
    ContactQuery all;
    add(tr("All Contacts"), m_store->count(all), QString::fromLatin1(kAll));
    ContactQuery none;
    none.uncategorized = true;
    add(tr("Uncategorized"), m_store->count(none), QString::fromLatin1(kUncategorized));
    for (const CategoryCount &c : cats) {
        QListWidgetItem *row = add(c.name, c.contacts, c.name);
        row->setIcon(icon(QStringLiteral("folder")));
        row->setToolTip(tr("Type \u201c%1\u201d in To, Cc or Bcc to write to everyone in this category.").arg(c.name));
    }
    ContactQuery hidden;
    hidden.show = ContactQuery::Show::Hidden;
    add(tr("Hidden"), m_store->count(hidden), QString::fromLatin1(kHidden));
    m_loading = was;
    m_renameCategory->setEnabled(isCategory(m_group));
    m_deleteCategory->setEnabled(isCategory(m_group));
    m_hide->setText(m_group == QLatin1String(kHidden) ? tr("Unhide") : tr("Hide"));
}

void ContactsWindow::refreshList()
{
    if (!m_store || !m_store->isOpen()) {
        return;
    }
    const QStringList keep = selectedIds();
    const ContactQuery q = query();
    const QList<Contact> list = m_store->contacts(q);
    {
        const bool was = std::exchange(m_loading, true);
        m_list->clear();
        for (const Contact &c : list) {
            QStringList emails;
            for (const ContactEmail &e : c.emails) {
                emails << e.email;
            }
            QString second = emails.isEmpty() ? tr("(no email)") : emails.join(QStringLiteral(", "));
            if (!c.nickname.isEmpty()) {
                second = QStringLiteral("\u201c%1\u201d   ").arg(c.nickname) + second;
            }
            if (!c.categories.isEmpty()) {
                second += QStringLiteral("   • ") + c.categories.join(QStringLiteral(", "));
            }
            auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(c.displayName, second), m_list);
            item->setData(Qt::UserRole, c.id);
            item->setData(kNameRole, c.displayName);
            item->setData(kDetailRole, second);
            QString tip = sourceText(c);
            if (c.trusted) {
                tip += tr(" · trusted");
            }
            if (!c.comment.isEmpty()) {
                tip += QLatin1Char('\n') + c.comment;
            }
            item->setToolTip(tip);
            if (keep.contains(c.id)) {
                item->setSelected(true);
                if (!m_list->currentItem()) {
                    m_list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
                }
            }
        }
        m_loading = was;
    }
    const int total = m_store->count(q);
    m_status->setText(total > list.size() ? tr("Showing the first %1 of %2 contacts. Search to narrow them.").arg(list.size()).arg(total)
                                          : tr("%n contact(s)", nullptr, total));
    showDetail();
}

QStringList ContactsWindow::selectedIds() const
{
    QStringList ids;
    for (int r = 0; r < m_list->count(); ++r) {
        if (m_list->item(r)->isSelected()) {
            ids << m_list->item(r)->data(Qt::UserRole).toString();
        }
    }
    return ids;
}

void ContactsWindow::selectContacts(const QStringList &ids)
{
    {
        const bool was = std::exchange(m_loading, true);
        m_list->clearSelection();
        for (int r = 0; r < m_list->count(); ++r) {
            if (ids.contains(m_list->item(r)->data(Qt::UserRole).toString())) {
                m_list->item(r)->setSelected(true);
            }
        }
        m_loading = was;
    }
    showDetail();
}

void ContactsWindow::showGroup(const QString &group)
{
    m_group = group.isEmpty() ? QString::fromLatin1(kAll) : group;
    refreshGroups();
    refreshList();
}

void ContactsWindow::showDetail()
{
    const QStringList ids = selectedIds();
    m_hide->setEnabled(!ids.isEmpty());
    m_categorize->setEnabled(!ids.isEmpty());
    const bool was = std::exchange(m_loading, true);
    m_detailId = ids.size() == 1 ? ids.first() : QString();
    const Contact c = m_detailId.isEmpty() ? Contact() : m_store->contact(m_detailId);
    m_detail->setEnabled(!m_detailId.isEmpty());
    if (m_detailId.isEmpty()) {
        m_name->setText(ids.isEmpty() ? tr("No contact selected") : tr("%n contact(s) selected", nullptr, ids.size()));
        m_emails->setText(ids.isEmpty() ? QString() : tr("Use Hide or Add to Category for all of them."));
        m_source->clear();
    } else {
        m_name->setText(c.displayName);
        QStringList emails;
        for (const ContactEmail &e : c.emails) {
            emails << e.email;
        }
        m_emails->setText(emails.join(QLatin1Char('\n')));
        m_source->setText(sourceText(c));
    }
    m_hidden->setChecked(c.hidden);
    if (m_nickname->text() != c.nickname) {
        m_nickname->setText(c.nickname);
    }
    m_categoryChecks->clear();
    for (const CategoryCount &cat : m_store->categories()) {
        auto *it = new QListWidgetItem(cat.name, m_categoryChecks);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        it->setCheckState(c.categories.contains(cat.name, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
    }
    m_fields->setRowCount(0);
    for (const ContactField &f : c.fields) {
        const int r = m_fields->rowCount();
        m_fields->insertRow(r);
        m_fields->setItem(r, 0, new QTableWidgetItem(f.name));
        m_fields->setItem(r, 1, new QTableWidgetItem(f.value));
    }
    m_removeField->setEnabled(false);
    if (m_comment->toPlainText() != c.comment) {
        m_comment->setPlainText(c.comment);
    }
    m_loading = was;
}

void ContactsWindow::saveCategories()
{
    if (m_detailId.isEmpty()) {
        return;
    }
    QStringList names;
    for (int r = 0; r < m_categoryChecks->count(); ++r) {
        if (m_categoryChecks->item(r)->checkState() == Qt::Checked) {
            names << m_categoryChecks->item(r)->text();
        }
    }
    m_store->setCategories(m_detailId, names);
    refresh(); // counts, and it may have left the category on show
}

void ContactsWindow::saveFields()
{
    if (m_detailId.isEmpty()) {
        return;
    }
    QList<ContactField> fields;
    for (int r = 0; r < m_fields->rowCount(); ++r) {
        const QTableWidgetItem *name = m_fields->item(r, 0);
        const QTableWidgetItem *value = m_fields->item(r, 1);
        fields.append({name ? name->text() : QString(), value ? value->text() : QString()});
    }
    m_store->setFields(m_detailId, fields);
}

void ContactsWindow::addField(const QString &name)
{
    if (m_detailId.isEmpty() || name.trimmed().isEmpty()) {
        return;
    }
    int r = 0;
    {
        const bool was = std::exchange(m_loading, true);
        r = m_fields->rowCount();
        m_fields->insertRow(r);
        m_fields->setItem(r, 0, new QTableWidgetItem(name.trimmed()));
        m_fields->setItem(r, 1, new QTableWidgetItem);
        m_loading = was;
    }
    saveFields();
    m_fields->setCurrentCell(r, 1);
    if (isVisible()) {
        m_fields->editItem(m_fields->item(r, 1)); // straight to typing the value
    }
}

void ContactsWindow::removeCurrentField()
{
    const int r = m_fields->currentRow();
    if (r < 0 || m_detailId.isEmpty()) {
        return;
    }
    {
        const bool was = std::exchange(m_loading, true);
        m_fields->removeRow(r);
        m_loading = was;
    }
    saveFields();
}

bool ContactsWindow::exportJson(const QString &path, bool everything)
{
    ContactQuery q = query();
    if (everything) {
        q = ContactQuery();
        q.show = ContactQuery::Show::All;
    }
    const int n = m_store->count(q);
    QSaveFile file(path);
    // Names, addresses and private notes: readable by the user only.
    if (!file.open(QIODevice::WriteOnly) || file.write(m_store->exportJson(q)) < 0 ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !file.commit()) {
        return false;
    }
    m_status->setText(tr("Exported %n contact(s) to %1", nullptr, n).arg(QDir::toNativeSeparators(path)));
    return true;
}

QString ContactsWindow::askCategoryName(const QString &title, const QString &current)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, title, tr("Category name:"), QLineEdit::Normal, current, &ok);
    return ok ? name.trimmed() : QString();
}

void ContactsWindow::newCategory(const QString &name)
{
    if (m_store->addCategory(name)) {
        showGroup(name.trimmed());
    }
}

void ContactsWindow::renameCategory(const QString &from, const QString &to)
{
    if (m_store->renameCategory(from, to)) {
        if (m_group.compare(from, Qt::CaseInsensitive) == 0) {
            m_group = to.trimmed();
        }
        refresh();
    }
}

void ContactsWindow::deleteCategory(const QString &name)
{
    m_store->deleteCategory(name);
    refresh();
}

void ContactsWindow::setSelectedHidden(bool hidden)
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        return;
    }
    m_store->setHidden(ids, hidden);
    refresh();
}

void ContactsWindow::addSelectedToCategory(const QString &name)
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty() || name.trimmed().isEmpty()) {
        return;
    }
    m_store->addToCategory(ids, name);
    refresh();
}

void ContactsWindow::removeSelectedFromCategory(const QString &name)
{
    for (const QString &id : selectedIds()) {
        QStringList names = m_store->contact(id).categories;
        for (qsizetype i = names.size() - 1; i >= 0; --i) {
            if (names.at(i).compare(name, Qt::CaseInsensitive) == 0) {
                names.removeAt(i);
            }
        }
        m_store->setCategories(id, names);
    }
    refresh();
}

QMenu *ContactsWindow::categorizeMenu(QWidget *parent)
{
    auto *menu = new QMenu(tr("Add to Category"), parent);
    for (const CategoryCount &c : m_store->categories()) {
        const QString name = c.name;
        menu->addAction(icon(QStringLiteral("folder")), name, this, [this, name]() { addSelectedToCategory(name); });
    }
    if (!menu->isEmpty()) {
        menu->addSeparator();
    }
    menu->addAction(tr("New Category…"), this,
                    [this]() { addSelectedToCategory(askCategoryName(tr("New Category"))); });
    return menu;
}

void ContactsWindow::showListMenu(const QPoint &pos)
{
    if (selectedIds().isEmpty()) {
        return;
    }
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const bool inHidden = m_group == QLatin1String(kHidden);
    menu->addAction(inHidden ? tr("Unhide") : tr("Hide"), this, [this, inHidden]() { setSelectedHidden(!inHidden); });
    menu->addMenu(categorizeMenu(menu));
    if (isCategory(m_group)) {
        const QString name = m_group;
        menu->addAction(tr("Remove from \"%1\"").arg(name), this, [this, name]() { removeSelectedFromCategory(name); });
    }
    menu->popup(m_list->viewport()->mapToGlobal(pos));
}

void ContactsWindow::syncNow()
{
    // Match Settings → Sync Contacts: request contact scopes / re-auth when needed.
    if (m_syncTrigger) {
        m_syncTrigger();
        return;
    }
    if (m_sync) {
        m_sync->sync();
    }
}

} // namespace zmail::ui
