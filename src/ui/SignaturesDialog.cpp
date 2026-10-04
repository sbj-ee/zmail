#include "SignaturesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

using namespace zmail;

SignaturesDialog::SignaturesDialog(SignatureStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_sigs(store->all())
{
    setWindowTitle(tr("Signatures"));
    resize(640, 420);
    auto *root = new QHBoxLayout(this);

    auto *left = new QVBoxLayout;
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("signatureList"));
    left->addWidget(m_list);
    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("New"), this);
    auto *del = new QPushButton(tr("Delete"), this);
    btns->addWidget(add);
    btns->addWidget(del);
    left->addLayout(btns);
    root->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    form->addRow(tr("Name:"), m_name);
    right->addLayout(form);
    right->addWidget(new QLabel(tr("Rich (HTML) version:"), this));
    m_rich = new QTextEdit(this);
    m_rich->setAcceptRichText(true);
    right->addWidget(m_rich, 2);
    right->addWidget(new QLabel(tr("Plain-text version (empty = derived from the rich version):"), this));
    m_plain = new QPlainTextEdit(this);
    right->addWidget(m_plain, 1);
    auto *defRow = new QFormLayout;
    m_default = new QComboBox(this);
    defRow->addRow(tr("New messages start with:"), m_default);
    right->addLayout(defRow);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    right->addWidget(bb);
    root->addLayout(right, 3);

    for (const auto &s : m_sigs) {
        m_list->addItem(s.name);
    }
    refreshDefaults();
    m_default->setCurrentIndex(std::max(0, m_default->findText(m_store->defaultName())));

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        storeCurrent();
        load(row);
    });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_row >= 0) {
            m_list->item(m_row)->setText(t);
        }
    });
    connect(add, &QPushButton::clicked, this, [this] {
        storeCurrent();
        Signature s;
        s.name = tr("Signature %1").arg(m_sigs.size() + 1);
        m_sigs << s;
        m_list->addItem(s.name);
        m_list->setCurrentRow(int(m_sigs.size()) - 1);
        refreshDefaults();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0) {
            return;
        }
        m_row = -1;
        m_sigs.removeAt(row);
        delete m_list->takeItem(row);
        load(m_list->currentRow());
        refreshDefaults();
    });
    connect(bb, &QDialogButtonBox::accepted, this, &SignaturesDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &SignaturesDialog::reject);
    m_list->setCurrentRow(m_sigs.isEmpty() ? -1 : 0);
    load(m_list->currentRow());
}

void SignaturesDialog::load(int row)
{
    m_row = row;
    const bool on = row >= 0 && row < m_sigs.size();
    m_name->setEnabled(on);
    m_rich->setEnabled(on);
    m_plain->setEnabled(on);
    m_name->setText(on ? m_sigs[row].name : QString());
    m_rich->setHtml(on ? m_sigs[row].html : QString());
    m_plain->setPlainText(on ? m_sigs[row].text : QString());
}

void SignaturesDialog::storeCurrent()
{
    if (m_row < 0 || m_row >= m_sigs.size()) {
        return;
    }
    Signature &s = m_sigs[m_row];
    s.name = m_name->text().trimmed();
    s.html = m_rich->toPlainText().trimmed().isEmpty() ? QString() : m_rich->toHtml();
    s.text = m_plain->toPlainText();
    refreshDefaults();
}

void SignaturesDialog::refreshDefaults()
{
    const QString cur = m_default->currentText();
    m_default->clear();
    m_default->addItem(tr("(none)"));
    for (const auto &s : m_sigs) {
        m_default->addItem(s.name);
    }
    m_default->setCurrentIndex(std::max(0, m_default->findText(cur)));
}

void SignaturesDialog::accept()
{
    storeCurrent();
    QList<Signature> keep;
    for (const auto &s : m_sigs) {
        if (!s.name.isEmpty()) {
            keep << s;
        }
    }
    m_store->setAll(keep);
    m_store->setDefaultName(m_default->currentIndex() > 0 ? m_default->currentText() : QString());
    QDialog::accept();
}
