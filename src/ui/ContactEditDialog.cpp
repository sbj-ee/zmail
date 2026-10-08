#include "ContactEditDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace zmail::ui {

ContactEditDialog::ContactEditDialog(const Contact &contact, const QStringList &allCategories, QWidget *parent)
    : QDialog(parent)
    , m_contact(contact)
{
    setObjectName(QStringLiteral("contactEditDialog"));
    setModal(true);
    setWindowTitle(contact.id.isEmpty() ? tr("New Contact") : tr("Edit Contact"));
    resize(640, 760);
    auto *lay = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(contact.displayName, this);
    m_name->setObjectName(QStringLiteral("editName"));
    m_name->setPlaceholderText(tr("Name"));
    form->addRow(tr("Name:"), m_name);
    m_nickname = new QLineEdit(contact.nickname, this);
    m_nickname->setObjectName(QStringLiteral("editNickname"));
    m_nickname->setPlaceholderText(tr("A short name to type in To instead of the address"));
    form->addRow(tr("Nickname:"), m_nickname);
    lay->addLayout(form);

    // ---- addresses ----
    auto *addresses = new QGroupBox(tr("Email addresses"), this);
    auto *al = new QHBoxLayout(addresses);
    m_emails = new QListWidget(addresses);
    m_emails->setObjectName(QStringLiteral("editEmails"));
    m_emails->setMaximumHeight(90);
    al->addWidget(m_emails, 1);
    auto *ab = new QVBoxLayout;
    auto *addEmailBtn = new QPushButton(tr("Add"), addresses);
    addEmailBtn->setObjectName(QStringLiteral("editAddEmail"));
    auto *removeEmailBtn = new QPushButton(tr("Remove"), addresses);
    removeEmailBtn->setObjectName(QStringLiteral("editRemoveEmail"));
    auto *primaryBtn = new QPushButton(tr("Make Primary"), addresses);
    primaryBtn->setObjectName(QStringLiteral("editPrimaryEmail"));
    primaryBtn->setToolTip(tr("The primary address is the one a nickname or category stands for"));
    for (QPushButton *b : {addEmailBtn, removeEmailBtn, primaryBtn}) {
        b->setAutoDefault(false);
        ab->addWidget(b);
    }
    ab->addStretch(1);
    al->addLayout(ab);
    lay->addWidget(addresses);
    for (const ContactEmail &e : contact.emails) {
        auto *item = new QListWidgetItem(e.email, m_emails);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(Qt::UserRole, e.primary);
    }
    refreshEmailMarks();

    // ---- categories ----
    auto *cats = new QGroupBox(tr("Categories"), this);
    auto *cl = new QHBoxLayout(cats);
    m_categories = new QListWidget(cats);
    m_categories->setObjectName(QStringLiteral("editCategories"));
    m_categories->setMaximumHeight(90);
    cl->addWidget(m_categories, 1);
    auto *newCat = new QPushButton(tr("New…"), cats);
    newCat->setObjectName(QStringLiteral("editNewCategory"));
    newCat->setAutoDefault(false);
    auto *cb = new QVBoxLayout;
    cb->addWidget(newCat);
    cb->addStretch(1);
    cl->addLayout(cb);
    lay->addWidget(cats);
    QStringList names = allCategories;
    for (const QString &mine : contact.categories) {
        if (!names.contains(mine, Qt::CaseInsensitive)) {
            names << mine;
        }
    }
    for (const QString &name : std::as_const(names)) {
        auto *item = new QListWidgetItem(name, m_categories);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(contact.categories.contains(name, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
    }

    // ---- fields ----
    auto *fields = new QGroupBox(tr("Fields"), this);
    auto *fl = new QHBoxLayout(fields);
    m_fields = new QTableWidget(0, 2, fields);
    m_fields->setObjectName(QStringLiteral("editFields"));
    m_fields->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    m_fields->horizontalHeader()->setStretchLastSection(true);
    m_fields->verticalHeader()->hide();
    m_fields->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fields->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fields->setMinimumHeight(130); // room for a few rows, whatever else is on the page
    fl->addWidget(m_fields, 1);
    auto *fb = new QVBoxLayout;
    auto *addFieldBtn = new QPushButton(tr("Add"), fields);
    addFieldBtn->setObjectName(QStringLiteral("editAddField"));
    addFieldBtn->setAutoDefault(false);
    auto *fieldMenu = new QMenu(addFieldBtn);
    for (const QString &preset : {tr("Phone"), tr("Mobile"), tr("Company"), tr("Title"), tr("Address"), tr("Birthday"),
                                  tr("Website")}) {
        fieldMenu->addAction(preset, this, [this, preset]() { addField(preset); });
    }
    fieldMenu->addSeparator();
    fieldMenu->addAction(tr("Other…"), this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Add Field"), tr("Field name:"), QLineEdit::Normal, {}, &ok);
        if (ok) {
            addField(name);
        }
    });
    addFieldBtn->setMenu(fieldMenu);
    auto *removeFieldBtn = new QPushButton(tr("Remove"), fields);
    removeFieldBtn->setObjectName(QStringLiteral("editRemoveField"));
    removeFieldBtn->setAutoDefault(false);
    fb->addWidget(addFieldBtn);
    fb->addWidget(removeFieldBtn);
    fb->addStretch(1);
    fl->addLayout(fb);
    lay->addWidget(fields, 1);
    for (const ContactField &f : contact.fields) {
        const int r = m_fields->rowCount();
        m_fields->insertRow(r);
        m_fields->setItem(r, 0, new QTableWidgetItem(f.name));
        m_fields->setItem(r, 1, new QTableWidgetItem(f.value));
    }

    lay->addWidget(new QLabel(tr("Comment:"), this));
    m_comment = new QPlainTextEdit(contact.comment, this);
    m_comment->setObjectName(QStringLiteral("editComment"));
    m_comment->setTabChangesFocus(true);
    m_comment->setMaximumHeight(110);
    lay->addWidget(m_comment);

    m_hidden = new QCheckBox(tr("Hide this contact"), this);
    m_hidden->setObjectName(QStringLiteral("editHidden"));
    m_hidden->setChecked(contact.hidden);
    m_hidden->setToolTip(tr("Hidden contacts are left out of the Contacts list and are not suggested when you write a message."));
    lay->addWidget(m_hidden);

    const bool fromGoogle = !contact.id.isEmpty() && contact.source != QLatin1String("local");
    if (fromGoogle) {
        auto *note = new QHBoxLayout;
        auto *text = new QLabel(contact.edited
                                    ? tr("From Google, with its name or addresses changed here. Google's copy is not changed.")
                                    : tr("From Google. Changes are kept on this computer; Google's copy is not changed."),
                                this);
        text->setObjectName(QStringLiteral("editSourceNote"));
        text->setWordWrap(true);
        text->setForegroundRole(QPalette::PlaceholderText);
        note->addWidget(text, 1);
        auto *revert = new QPushButton(tr("Use Google's"), this);
        revert->setObjectName(QStringLiteral("editRevert"));
        revert->setAutoDefault(false);
        revert->setToolTip(tr("Go back to the name and addresses Google has for this contact"));
        revert->setVisible(contact.edited);
        note->addWidget(revert);
        lay->addLayout(note);
        connect(revert, &QPushButton::clicked, this, [this]() {
            m_revert = true;
            QDialog::accept(); // the caller restores Google's name and addresses and keeps the rest
        });
    }

    m_problem = new QLabel(this);
    m_problem->setObjectName(QStringLiteral("editProblem"));
    m_problem->setWordWrap(true);
    QPalette warn = m_problem->palette();
    warn.setColor(QPalette::WindowText, QColor(0xb3, 0x26, 0x1e));
    m_problem->setPalette(warn);
    m_problem->hide();
    lay->addWidget(m_problem);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ContactEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(addEmailBtn, &QPushButton::clicked, this, [this]() {
        addEmail(QString());
        m_emails->editItem(m_emails->item(m_emails->count() - 1)); // straight to typing it
    });
    connect(removeEmailBtn, &QPushButton::clicked, this, &ContactEditDialog::removeCurrentEmail);
    connect(primaryBtn, &QPushButton::clicked, this, &ContactEditDialog::makeCurrentEmailPrimary);
    connect(newCat, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Category"), tr("Category name:"), QLineEdit::Normal, {}, &ok);
        if (ok) {
            addCategory(name);
        }
    });
    connect(removeFieldBtn, &QPushButton::clicked, this, &ContactEditDialog::removeCurrentField);
    m_name->setFocus();
}

// The primary address is shown in bold and says so.
void ContactEditDialog::refreshEmailMarks()
{
    bool anyPrimary = false;
    for (int r = 0; r < m_emails->count(); ++r) {
        anyPrimary = anyPrimary || m_emails->item(r)->data(Qt::UserRole).toBool();
    }
    for (int r = 0; r < m_emails->count(); ++r) {
        QListWidgetItem *item = m_emails->item(r);
        const bool primary = anyPrimary ? item->data(Qt::UserRole).toBool() : r == 0;
        item->setData(Qt::UserRole, primary);
        QFont f = item->font();
        f.setBold(primary);
        item->setFont(f);
        item->setToolTip(primary ? tr("Primary address") : QString());
        anyPrimary = true; // only one: the first found
        if (primary) {
            for (int later = r + 1; later < m_emails->count(); ++later) {
                m_emails->item(later)->setData(Qt::UserRole, false);
            }
        }
    }
}

void ContactEditDialog::addEmail(const QString &email)
{
    auto *item = new QListWidgetItem(email.trimmed(), m_emails);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setData(Qt::UserRole, m_emails->count() == 1);
    m_emails->setCurrentItem(item);
    refreshEmailMarks();
}

void ContactEditDialog::removeCurrentEmail()
{
    delete m_emails->takeItem(m_emails->currentRow());
    refreshEmailMarks();
}

void ContactEditDialog::makeCurrentEmailPrimary()
{
    const int row = m_emails->currentRow();
    if (row < 0) {
        return;
    }
    for (int r = 0; r < m_emails->count(); ++r) {
        m_emails->item(r)->setData(Qt::UserRole, r == row);
    }
    refreshEmailMarks();
}

void ContactEditDialog::addCategory(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        return;
    }
    for (int r = 0; r < m_categories->count(); ++r) {
        if (m_categories->item(r)->text().compare(n, Qt::CaseInsensitive) == 0) {
            m_categories->item(r)->setCheckState(Qt::Checked); // it exists: tick it
            return;
        }
    }
    auto *item = new QListWidgetItem(n, m_categories);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
}

void ContactEditDialog::addField(const QString &name)
{
    if (name.trimmed().isEmpty()) {
        return;
    }
    const int r = m_fields->rowCount();
    m_fields->insertRow(r);
    m_fields->setItem(r, 0, new QTableWidgetItem(name.trimmed()));
    m_fields->setItem(r, 1, new QTableWidgetItem);
    m_fields->setCurrentCell(r, 1);
    if (isVisible()) {
        m_fields->editItem(m_fields->item(r, 1));
    }
}

void ContactEditDialog::removeCurrentField()
{
    if (m_fields->currentRow() >= 0) {
        m_fields->removeRow(m_fields->currentRow());
    }
}

Contact ContactEditDialog::contact() const
{
    Contact c = m_contact;
    c.displayName = m_name->text().trimmed();
    c.nickname = m_nickname->text().trimmed();
    c.emails.clear();
    for (int r = 0; r < m_emails->count(); ++r) {
        const QString email = m_emails->item(r)->text().trimmed();
        if (!email.isEmpty()) {
            c.emails.append({email, m_emails->item(r)->data(Qt::UserRole).toBool()});
        }
    }
    c.categories.clear();
    for (int r = 0; r < m_categories->count(); ++r) {
        if (m_categories->item(r)->checkState() == Qt::Checked) {
            c.categories << m_categories->item(r)->text();
        }
    }
    c.fields.clear();
    for (int r = 0; r < m_fields->rowCount(); ++r) {
        const QTableWidgetItem *name = m_fields->item(r, 0);
        const QTableWidgetItem *value = m_fields->item(r, 1);
        if (name && !name->text().trimmed().isEmpty()) {
            c.fields.append({name->text().trimmed(), value ? value->text() : QString()});
        }
    }
    c.comment = m_comment->toPlainText();
    c.hidden = m_hidden->isChecked();
    return c;
}

void ContactEditDialog::accept()
{
    // Finish any cell or address still being typed.
    m_emails->setCurrentItem(nullptr);
    m_fields->setCurrentItem(nullptr);
    const Contact c = contact();
    QString problem;
    for (const ContactEmail &e : c.emails) {
        const int at = e.email.indexOf(QLatin1Char('@'));
        if (at <= 0 || at == e.email.size() - 1 || e.email.contains(QLatin1Char(' '))) {
            problem = tr("“%1” doesn't look like an email address.").arg(e.email);
            break;
        }
    }
    if (problem.isEmpty() && c.displayName.isEmpty() && c.emails.isEmpty()) {
        problem = tr("Give the contact a name or an address.");
    }
    if (!problem.isEmpty()) {
        m_problem->setText(problem);
        m_problem->show();
        return;
    }
    QDialog::accept();
}

} // namespace zmail::ui
