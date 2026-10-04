#include "PrivacyDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace zmail::ui {

PrivacyDialog::PrivacyDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("privacyDialog"));
    setWindowTitle(tr("Privacy"));
    auto *lay = new QVBoxLayout(this);

    auto *images = new QGroupBox(tr("Remote images"), this);
    images->setObjectName(QStringLiteral("remoteImagesGroup"));
    auto *il = new QVBoxLayout(images);
    m_always = new QRadioButton(tr("&Always load"), images);
    m_always->setObjectName(QStringLiteral("remoteImagesAlways"));
    m_ask = new QRadioButton(tr("As&k: show a \u201cLoad images\u201d bar, except for senders below"), images);
    m_ask->setObjectName(QStringLiteral("remoteImagesAsk"));
    m_never = new QRadioButton(tr("&Never load"), images);
    m_never->setObjectName(QStringLiteral("remoteImagesNever"));
    auto *group = new QButtonGroup(this);
    for (QRadioButton *b : {m_always, m_ask, m_never}) {
        group->addButton(b);
        il->addWidget(b);
    }
    auto *note = new QLabel(tr("Loading images lets a sender see when you open their mail. zmail never sends cookies "
                               "with image requests and caps their size."),
                            images);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    il->addWidget(note);
    m_trackers = new QCheckBox(tr("Block &tracking pixels (1\u00d71 images, known trackers) even when loading"), images);
    m_trackers->setObjectName(QStringLiteral("blockTrackers"));
    il->addWidget(m_trackers);
    lay->addWidget(images);

    auto *senders = new QGroupBox(tr("Always load images from these senders (Ask mode)"), this);
    senders->setObjectName(QStringLiteral("remoteImageSendersGroup"));
    auto *sl = new QVBoxLayout(senders);
    m_list = new QListWidget(senders);
    m_list->setObjectName(QStringLiteral("remoteImageSenders"));
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setMinimumHeight(110);
    sl->addWidget(m_list);
    auto *row = new QHBoxLayout;
    m_add = new QLineEdit(senders);
    m_add->setObjectName(QStringLiteral("addSenderEdit"));
    m_add->setPlaceholderText(tr("name@example.com"));
    row->addWidget(m_add, 1);
    m_addButton = new QPushButton(tr("A&dd"), senders);
    m_addButton->setObjectName(QStringLiteral("addSenderButton"));
    m_addButton->setAutoDefault(false);
    row->addWidget(m_addButton);
    m_remove = new QPushButton(tr("&Remove"), senders);
    m_remove->setObjectName(QStringLiteral("removeSenderButton"));
    m_remove->setAutoDefault(false);
    row->addWidget(m_remove);
    m_clear = new QPushButton(tr("C&lear all"), senders);
    m_clear->setObjectName(QStringLiteral("clearSendersButton"));
    m_clear->setAutoDefault(false);
    row->addWidget(m_clear);
    sl->addLayout(row);
    lay->addWidget(senders);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    setMode(RemoteImages::mode());
    m_trackers->setChecked(RemoteImages::blockTrackers());
    m_list->addItems(RemoteImages::allowedSenders());

    connect(m_addButton, &QPushButton::clicked, this, &PrivacyDialog::addSender);
    connect(m_add, &QLineEdit::returnPressed, this, &PrivacyDialog::addSender);
    connect(m_add, &QLineEdit::textChanged, this, &PrivacyDialog::updateButtons);
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        qDeleteAll(m_list->selectedItems());
        updateButtons();
    });
    connect(m_clear, &QPushButton::clicked, this, [this]() {
        m_list->clear();
        updateButtons();
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, &PrivacyDialog::updateButtons);
    updateButtons();
}

RemoteImageMode PrivacyDialog::mode() const
{
    if (m_ask->isChecked()) {
        return RemoteImageMode::Ask;
    }
    if (m_never->isChecked()) {
        return RemoteImageMode::Never;
    }
    return RemoteImageMode::Always;
}

void PrivacyDialog::setMode(RemoteImageMode m)
{
    (m == RemoteImageMode::Ask ? m_ask : m == RemoteImageMode::Never ? m_never : m_always)->setChecked(true);
}

QStringList PrivacyDialog::senders() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i) {
        out << m_list->item(i)->text();
    }
    return out;
}

void PrivacyDialog::addSender()
{
    const QString a = RemoteImages::senderAddress(m_add->text());
    if (a.isEmpty()) {
        return;
    }
    if (m_list->findItems(a, Qt::MatchFixedString).isEmpty()) {
        m_list->addItem(a);
        m_list->sortItems();
    }
    m_add->clear();
    updateButtons();
}

void PrivacyDialog::updateButtons()
{
    m_addButton->setEnabled(!RemoteImages::senderAddress(m_add->text()).isEmpty());
    m_remove->setEnabled(!m_list->selectedItems().isEmpty());
    m_clear->setEnabled(m_list->count() > 0);
}

void PrivacyDialog::save() const
{
    RemoteImages::setMode(mode());
    RemoteImages::setBlockTrackers(m_trackers->isChecked());
    RemoteImages::setAllowedSenders(senders());
}

} // namespace zmail::ui
