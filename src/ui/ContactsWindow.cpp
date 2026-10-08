#include "ContactsWindow.h"
#include "ContactEditDialog.h"
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
#include <QTextBrowser>
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
    m_new = new QPushButton(tr("New Contact\u2026"), middle);
    m_new->setObjectName(QStringLiteral("newContactButton"));
    m_new->setAutoDefault(false);
    listButtons->addWidget(m_new);
    listButtons->addWidget(m_hide);
    listButtons->addWidget(m_categorize);
    listButtons->addStretch(1);
    ml->addLayout(listButtons);
    m_status = new QLabel(middle);
    m_status->setObjectName(QStringLiteral("contactsStatus"));
    ml->addWidget(m_status);

    // ---- right: the chosen contact, to read; Edit opens it to change ----
    m_detail = new QWidget(split);
    m_detail->setObjectName(QStringLiteral("contactDetail"));
    auto *dl = new QVBoxLayout(m_detail);
    dl->setContentsMargins(6, 0, 0, 0);
    m_summary = new QTextBrowser(m_detail);
    m_summary->setObjectName(QStringLiteral("contactSummary"));
    m_summary->setOpenLinks(false);
    m_summary->setFrameShape(QFrame::NoFrame);
    dl->addWidget(m_summary, 1);
    auto *detailButtons = new QHBoxLayout;
    m_edit = new QPushButton(tr("Edit\u2026"), m_detail);
    m_edit->setObjectName(QStringLiteral("editContactButton"));
    m_edit->setAutoDefault(false);
    m_edit->setToolTip(tr("Change this contact's name, addresses, categories, fields and comment (or double-click it)"));
    detailButtons->addWidget(m_edit);
    detailButtons->addStretch(1);
    dl->addLayout(detailButtons);

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
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        editContact(item->data(Qt::UserRole).toString());
    });
    connect(m_edit, &QPushButton::clicked, this, [this]() {
        if (!m_detailId.isEmpty()) {
            editContact(m_detailId);
        }
    });
    connect(m_new, &QPushButton::clicked, this, [this]() { editContact(QString()); });
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
                                          : (total == 1 ? tr("1 contact") : tr("%1 contacts").arg(total)));
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
    m_detailId = ids.size() == 1 ? ids.first() : QString();
    m_edit->setEnabled(!m_detailId.isEmpty());
    if (m_detailId.isEmpty()) {
        m_summary->setHtml(QStringLiteral("<p style='color:gray'>%1</p>")
                               .arg(ids.isEmpty() ? tr("No contact selected.")
                                                  : tr("%1 contacts selected. Use Hide or Add to Category for all of them.").arg(ids.size())));
        return;
    }
    const Contact c = m_store->contact(m_detailId);
    const auto esc = [](const QString &s) { return s.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")); };
    QString html = QStringLiteral("<h3>%1</h3>").arg(esc(c.displayName));
    for (const ContactEmail &e : c.emails) {
        html += QStringLiteral("<div>%1%2</div>").arg(esc(e.email), c.emails.size() > 1 && e.primary ? tr(" &nbsp;(primary)") : QString());
    }
    html += QStringLiteral("<p style='color:gray'>%1%2</p>")
                .arg(esc(sourceText(c)), c.edited ? tr(", edited here") : QString());
    QString rows;
    const auto row = [&rows, &esc](const QString &key, const QString &value) {
        if (!value.trimmed().isEmpty()) {
            rows += QStringLiteral("<tr><td align='right'><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>").arg(esc(key), esc(value));
        }
    };
    row(tr("Nickname:"), c.nickname);
    row(tr("Categories:"), c.categories.join(QStringLiteral(", ")));
    for (const ContactField &f : c.fields) {
        row(f.name + QLatin1Char(':'), f.value);
    }
    if (c.hidden) {
        row(tr("Hidden:"), tr("yes"));
    }
    if (!rows.isEmpty()) {
        html += QStringLiteral("<table cellspacing='2'>%1</table>").arg(rows);
    }
    if (!c.comment.trimmed().isEmpty()) {
        html += QStringLiteral("<p><b>%1</b><br>%2</p>").arg(tr("Comment"), esc(c.comment));
    }
    m_summary->setHtml(html);
}

// The edit dialog for a contact, filled in but not shown (editContact()
// runs it; the tests drive it directly).
ContactEditDialog *ContactsWindow::makeEditDialog(const QString &contactId)
{
    QStringList categories;
    for (const CategoryCount &c : m_store->categories()) {
        categories << c.name;
    }
    Contact c = contactId.isEmpty() ? Contact() : m_store->contact(contactId);
    if (contactId.isEmpty()) {
        c.source = QStringLiteral("local");
        if (isCategory(m_group)) {
            c.categories << m_group; // a new contact made while looking at a category starts in it
        }
    }
    return new ContactEditDialog(c, categories, this);
}

// What OK in the dialog does: save it (creating the contact if it is new),
// or go back to Google's name and addresses. Returns the contact's id.
QString ContactsWindow::applyEdit(const ContactEditDialog *dialog)
{
    Contact c = dialog->contact();
    if (c.id.isEmpty()) {
        c.id = m_store->createContact(c.displayName, c.emails);
        if (c.id.isEmpty()) {
            return {};
        }
    }
    if (dialog->revertRequested()) {
        m_store->revertToSource(c.id);
        const Contact source = m_store->contact(c.id);
        c.displayName = source.displayName; // the rest of what was edited is still kept
        c.emails = source.emails;
    }
    m_store->updateContact(c);
    refresh();
    selectContacts({c.id});
    return c.id;
}

void ContactsWindow::editContact(const QString &contactId)
{
    ContactEditDialog *dialog = makeEditDialog(contactId);
    if (dialog->exec() == QDialog::Accepted) {
        applyEdit(dialog);
    }
    dialog->deleteLater();
}

void ContactsWindow::deleteSelected()
{
    // Only contacts made here can be deleted: one from Google would come
    // straight back with the next sync (hide those instead).
    QStringList mine;
    for (const QString &id : selectedIds()) {
        if (m_store->contact(id).source == QLatin1String("local")) {
            mine << id;
        }
    }
    for (const QString &id : std::as_const(mine)) {
        m_store->removeContact(id);
    }
    if (!mine.isEmpty()) {
        refresh();
    }
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
    m_status->setText((n == 1 ? tr("Exported 1 contact to %1").arg(QDir::toNativeSeparators(path))
                              : tr("Exported %1 contacts to %2").arg(n).arg(QDir::toNativeSeparators(path))));
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
    const QStringList ids = selectedIds();
    if (ids.size() == 1) {
        const QString id = ids.first();
        menu->addAction(tr("Edit\u2026"), this, [this, id]() { editContact(id); });
        menu->addSeparator();
    }
    menu->addAction(inHidden ? tr("Unhide") : tr("Hide"), this, [this, inHidden]() { setSelectedHidden(!inHidden); });
    menu->addMenu(categorizeMenu(menu));
    if (isCategory(m_group)) {
        const QString name = m_group;
        menu->addAction(tr("Remove from \"%1\"").arg(name), this, [this, name]() { removeSelectedFromCategory(name); });
    }
    bool anyLocal = false;
    for (const QString &id : ids) {
        anyLocal = anyLocal || m_store->contact(id).source == QLatin1String("local");
    }
    if (anyLocal) {
        menu->addSeparator();
        menu->addAction(tr("Delete"), this, &ContactsWindow::deleteSelected)
            ->setToolTip(tr("Contacts made in zmail can be deleted; ones from Google can be hidden"));
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
