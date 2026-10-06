#include "ContactsWindow.h"
#include "core/ContactStore.h"
#include "core/PeopleClient.h"
#include "Icons.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QAbstractButton>
#include <QVBoxLayout>

namespace zmail::ui {

ContactsWindow::ContactsWindow(ContactStore *store, ContactsSync *sync, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_sync(sync)
{
    setObjectName(QStringLiteral("contactsWindow"));
    setWindowTitle(tr("Contacts"));
    resize(480, 560);
    auto *lay = new QVBoxLayout(this);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("contactsSearch"));
    m_search->setPlaceholderText(tr("Search contacts"));
    m_search->setClearButtonEnabled(true);
    lay->addWidget(m_search);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("contactsList"));
    lay->addWidget(m_list, 1);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("contactsStatus"));
    lay->addWidget(m_status);
    auto *row = new QHBoxLayout;
    auto *syncBtn = new QPushButton(tr("Sync from Google"), this);
    syncBtn->setObjectName(QStringLiteral("contactsSyncButton"));
    syncBtn->setEnabled(m_sync != nullptr);
    row->addWidget(syncBtn);
    row->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    row->addWidget(buttons);
    lay->addLayout(row);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refresh(); });
    connect(syncBtn, &QPushButton::clicked, this, &ContactsWindow::syncNow);
    if (m_sync) {
        connect(m_sync, &ContactsSync::finished, this, [this](bool ok, const QString &msg) {
            m_status->setText(msg);
            if (ok) {
                refresh();
            }
        });
        connect(m_sync, &ContactsSync::progress, this, [this](const QString &s) { m_status->setText(s); });
    }
    refresh();
}

void ContactsWindow::refresh()
{
    m_list->clear();
    if (!m_store || !m_store->isOpen()) {
        m_status->setText(tr("No contacts store."));
        return;
    }
    const auto list = m_store->contacts(m_search->text(), 500);
    for (const Contact &c : list) {
        QString emails;
        for (const ContactEmail &e : c.emails) {
            if (!emails.isEmpty()) {
                emails += QStringLiteral(", ");
            }
            emails += e.email;
        }
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(c.displayName, emails.isEmpty() ? tr("(no email)") : emails), m_list);
        item->setData(Qt::UserRole, c.id);
        QString tip = c.source;
        if (c.trusted) {
            tip += tr(" · trusted");
        }
        item->setToolTip(tip);
    }
    m_status->setText(tr("%n contact(s)", nullptr, list.size()));
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
