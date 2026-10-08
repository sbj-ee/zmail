#include "MailboxWindow.h"

#include "MessageListModel.h"
#include "MessageView.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QScrollBar>
#include <QSplitter>
#include <QItemSelectionModel>
#include <QShortcut>
#include <QStatusBar>
#include <QTreeView>

namespace zmail::ui {

MailboxWindow::MailboxWindow(MessageListModel *model, const QString &mailboxKey, const QString &title, QWidget *parent)
    : QMainWindow(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("mailboxWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("%1 — zmail").arg(title));
    resize(900, 520);

    m_proxy = new MessageFilterProxy(this);
    m_proxy->setSourceModel(model);
    m_proxy->setMailbox(mailboxKey);

    m_list = new QTreeView(this);
    m_list->setObjectName(QStringLiteral("mailboxWindowList"));
    m_list->setModel(m_proxy);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSortingEnabled(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true); // onto a folder in the main window's sidebar
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setIconSize(QSize(14, 14));
    m_list->sortByColumn(MessageListModel::Date, Qt::DescendingOrder);
    m_list->header()->setStretchLastSection(true);
    m_list->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("mailboxWindowSplitter"));
    split->setChildrenCollapsible(false);
    split->addWidget(m_list);
    m_preview = new MessageView(split);
    m_preview->setObjectName(QStringLiteral("mailboxWindowPreview"));
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    setCentralWidget(split);
    resize(900, 700);

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("mailboxWindowCount"));
    statusBar()->addPermanentWidget(m_count);

    connect(m_list, &QTreeView::doubleClicked, this, [this](const QModelIndex &i) {
        const QString id = m_model->item(m_proxy->mapToSource(i).row()).id;
        if (!id.isEmpty()) {
            emit openRequested(id);
        }
    });
    const auto openCurrent = [this]() {
        const QStringList ids = selectedIds();
        if (!ids.isEmpty()) {
            emit openRequested(ids.first());
        }
    };
    const auto deleteSelected = [this]() {
        const QStringList ids = selectedIds();
        if (!ids.isEmpty()) {
            emit deleteRequested(ids);
        }
    };
    for (const QKeySequence &k : {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter), QKeySequence(Qt::CTRL | Qt::Key_O)}) {
        connect(new QShortcut(k, m_list, nullptr, nullptr, Qt::WidgetShortcut), &QShortcut::activated, this, openCurrent);
    }
    for (const QKeySequence &k : {QKeySequence(QKeySequence::Delete), QKeySequence(Qt::CTRL | Qt::Key_D)}) {
        connect(new QShortcut(k, m_list, nullptr, nullptr, Qt::WidgetShortcut), &QShortcut::activated, this, deleteSelected);
    }
    connect(new QShortcut(QKeySequence::Close, this), &QShortcut::activated, this, &QWidget::close);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_list->indexAt(pos).isValid() && !selectedIds().isEmpty()) {
            emit menuRequested(selectedIds(), m_list->viewport()->mapToGlobal(pos));
        }
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        const QStringList ids = selectedIds();
        if (ids.size() == 1) {
            emit showRequested(ids.first());
        } else {
            m_preview->clear(); // none, or several
        }
    });
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        QScrollBar *sb = m_list->verticalScrollBar();
        if (sb->maximum() > 0 && v >= sb->maximum() - 2) {
            emit moreRequested();
        }
    });
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &MailboxWindow::updateCount);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &MailboxWindow::updateCount);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &MailboxWindow::updateCount);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &MailboxWindow::updateCount);
    connect(m_proxy, &QAbstractItemModel::dataChanged, this, &MailboxWindow::updateCount);
    updateCount();
}

QString MailboxWindow::mailbox() const
{
    return m_proxy->mailbox();
}

QStringList MailboxWindow::selectedIds() const
{
    QStringList ids;
    for (const QModelIndex &i : m_list->selectionModel()->selectedRows(0)) {
        const QString id = m_model->item(m_proxy->mapToSource(i).row()).id;
        if (!id.isEmpty() && !ids.contains(id)) {
            ids << id;
        }
    }
    return ids;
}

void MailboxWindow::updateCount()
{
    int unread = 0;
    const int total = m_proxy->rowCount();
    for (int r = 0; r < total; ++r) {
        unread += m_model->item(m_proxy->mapToSource(m_proxy->index(r, 0)).row()).status == MailStatus::Unread;
    }
    m_count->setText((total == 1 ? tr("1 message") : tr("%1 messages").arg(total)) + tr(", %1 unread ").arg(unread));
}

} // namespace zmail::ui
