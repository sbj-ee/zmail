#include "StationeryDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace zmail::ui {

StationeryDialog::StationeryDialog(const QList<Stationery> &items, QWidget *parent)
    : QDialog(parent)
    , m_items(items)
{
    setObjectName(QStringLiteral("stationeryDialog"));
    setWindowTitle(tr("Stationery"));
    resize(860, 520);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Stationery is a message kept as a template. Start one from File › New Message With, "
                                "or answer with one from Message › Reply With."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    lay->addWidget(split, 1);

    auto *left = new QWidget(split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(left);
    m_list->setObjectName(QStringLiteral("stationeryList"));
    ll->addWidget(m_list, 1);
    auto *lb = new QHBoxLayout;
    auto *add = new QPushButton(tr("New"), left);
    add->setObjectName(QStringLiteral("stationeryNew"));
    add->setAutoDefault(false);
    m_delete = new QPushButton(tr("Delete"), left);
    m_delete->setObjectName(QStringLiteral("stationeryDelete"));
    m_delete->setAutoDefault(false);
    lb->addWidget(add);
    lb->addWidget(m_delete);
    lb->addStretch(1);
    ll->addLayout(lb);

    m_editor = new QWidget(split);
    m_editor->setObjectName(QStringLiteral("stationeryEditor"));
    auto *el = new QVBoxLayout(m_editor);
    el->setContentsMargins(6, 0, 0, 0);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(m_editor);
    m_name->setObjectName(QStringLiteral("stationeryName"));
    m_to = new QLineEdit(m_editor);
    m_to->setObjectName(QStringLiteral("stationeryTo"));
    m_to->setPlaceholderText(tr("Optional: who it usually goes to"));
    m_cc = new QLineEdit(m_editor);
    m_cc->setObjectName(QStringLiteral("stationeryCc"));
    m_subject = new QLineEdit(m_editor);
    m_subject->setObjectName(QStringLiteral("stationerySubject"));
    m_subject->setPlaceholderText(tr("Optional: used when the message has no subject yet"));
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("To:"), m_to);
    form->addRow(tr("Cc:"), m_cc);
    form->addRow(tr("Subject:"), m_subject);
    el->addLayout(form);
    m_body = new QPlainTextEdit(m_editor);
    m_body->setObjectName(QStringLiteral("stationeryBody"));
    m_body->setPlaceholderText(tr("The text of the message"));
    m_body->setTabChangesFocus(true);
    el->addWidget(m_body, 1);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 620});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_loading) {
            m_current = row;
            load();
        }
    });
    connect(add, &QPushButton::clicked, this, [this]() {
        this->add();
        m_name->setFocus();
        m_name->selectAll();
    });
    connect(m_delete, &QPushButton::clicked, this, &StationeryDialog::removeCurrent);
    for (QLineEdit *e : {m_name, m_to, m_cc, m_subject}) {
        connect(e, &QLineEdit::textChanged, this, [this]() { commit(); });
    }
    connect(m_body, &QPlainTextEdit::textChanged, this, [this]() { commit(); });
    refreshList();
    setCurrent(m_items.isEmpty() ? -1 : 0);
}

void StationeryDialog::refreshList()
{
    const bool was = std::exchange(m_loading, true);
    m_list->clear();
    for (const Stationery &s : std::as_const(m_items)) {
        m_list->addItem(s.name.trimmed().isEmpty() ? tr("(unnamed)") : s.name);
    }
    m_list->setCurrentRow(m_current);
    m_loading = was;
    m_delete->setEnabled(m_current >= 0);
}

void StationeryDialog::setCurrent(int index)
{
    m_current = index >= 0 && index < m_items.size() ? index : -1;
    refreshList();
    load();
}

int StationeryDialog::add(const Stationery &item)
{
    Stationery s = item;
    if (s.name.trimmed().isEmpty()) {
        s.name = tr("New stationery");
    }
    m_items.append(s);
    setCurrent(int(m_items.size()) - 1);
    return m_current;
}

void StationeryDialog::removeCurrent()
{
    if (m_current < 0) {
        return;
    }
    m_items.removeAt(m_current);
    setCurrent(std::min(m_current, int(m_items.size()) - 1));
}

void StationeryDialog::load()
{
    const bool was = std::exchange(m_loading, true);
    m_editor->setEnabled(m_current >= 0);
    const Stationery s = m_current >= 0 ? m_items.at(m_current) : Stationery();
    m_name->setText(s.name);
    m_to->setText(s.to);
    m_cc->setText(s.cc);
    m_subject->setText(s.subject);
    m_body->setPlainText(s.body);
    m_loading = was;
}

void StationeryDialog::commit()
{
    if (m_loading || m_current < 0) {
        return;
    }
    Stationery &s = m_items[m_current];
    s.name = m_name->text();
    s.to = m_to->text();
    s.cc = m_cc->text();
    s.subject = m_subject->text();
    s.body = m_body->toPlainText();
    const bool was = std::exchange(m_loading, true);
    if (QListWidgetItem *item = m_list->item(m_current)) {
        item->setText(s.name.trimmed().isEmpty() ? tr("(unnamed)") : s.name);
    }
    m_loading = was;
}

} // namespace zmail::ui
