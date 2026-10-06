#include "MessageWindow.h"

#include "Icons.h"
#include "MessageView.h"
#include "core/ReplyBuilder.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QScreen>
#include <QSettings>
#include <QToolBar>

namespace zmail::ui {

namespace {
const char *kGeometryKey = "messageWindow/geometry";
}

MessageWindow::MessageWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("messageWindow"));

    auto *tb = addToolBar(tr("Message"));
    tb->setObjectName(QStringLiteral("messageToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    auto add = [&](const char *obj, const char *lucide, const QString &text, const QKeySequence &key) {
        QAction *a = tb->addAction(icon(QString::fromLatin1(lucide)), text);
        a->setObjectName(QString::fromLatin1(obj));
        a->setProperty("lucide", QString::fromLatin1(lucide));
        if (!key.isEmpty()) {
            a->setShortcut(key);
        }
        return a;
    };
    m_reply = add("actionReply", "reply", tr("Reply"), QKeySequence(Qt::CTRL | Qt::Key_R));
    m_replyAll = add("actionReplyAll", "reply-all", tr("Reply All"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    m_forward = add("actionForward", "forward", tr("Forward"), {});
    tb->addSeparator();
    m_delete = add("actionDelete", "trash", tr("Delete"), QKeySequence::Delete);
    m_delete->setToolTip(tr("Move to Trash"));
    m_reply->setToolTip(tr("Reply to sender"));
    m_replyAll->setToolTip(tr("Reply to all recipients"));
    m_forward->setToolTip(tr("Forward this message"));
    m_forward->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));

    m_view = new MessageView(this);
    connect(m_view, &MessageView::mailtoRequested, this, &MessageWindow::mailtoRequested);
    m_view->setObjectName(QStringLiteral("messageView"));
    setCentralWidget(m_view);

    // Viewer zoom, as in the main window.
    auto zoom = [this](const QList<QKeySequence> &keys, void (MessageView::*fn)()) {
        auto *a = new QAction(this);
        a->setShortcuts(keys);
        connect(a, &QAction::triggered, m_view, fn);
        addAction(a);
    };
    zoom({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)}, &MessageView::zoomIn);
    zoom({QKeySequence::ZoomOut}, &MessageView::zoomOut);
    zoom({QKeySequence(Qt::CTRL | Qt::Key_0)}, &MessageView::resetZoom);
    auto *close = new QAction(this);
    close->setShortcuts({QKeySequence::Close, QKeySequence(Qt::Key_Escape)});
    connect(close, &QAction::triggered, this, &QWidget::close);
    addAction(close);

    using Kind = zmail::ReplyBuilder::Kind;
    connect(m_reply, &QAction::triggered, this, [this]() { emit composeRequested(messageId(), int(Kind::Reply)); });
    connect(m_replyAll, &QAction::triggered, this,
            [this]() { emit composeRequested(messageId(), int(Kind::ReplyAll)); });
    connect(m_forward, &QAction::triggered, this, [this]() { emit composeRequested(messageId(), int(Kind::Forward)); });
    connect(m_delete, &QAction::triggered, this, [this]() { emit deleteRequested(messageId()); });

    // Size/position: the last one closed, cascaded past any still open.
    const QByteArray geo = QSettings().value(QLatin1String(kGeometryKey)).toByteArray();
    if (geo.isEmpty() || !restoreGeometry(geo)) {
        resize(820, 760);
    }
    int open = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w != this && qobject_cast<MessageWindow *>(w) && w->isVisible()) {
            ++open;
        }
    }
    if (open > 0) {
        QPoint p = pos() + QPoint(28, 28) * (open % 8);
        if (const QScreen *s = screen()) {
            const QRect avail = s->availableGeometry();
            if (!avail.contains(QRect(p, size()))) {
                p = avail.topLeft() + QPoint(28, 28) * (open % 8);
            }
        }
        move(p);
    }
}

void MessageWindow::setMessage(const ViewMessage &m)
{
    setWindowTitle(m.subject.isEmpty() ? tr("(no subject)") : m.subject);
    m_view->setMessage(m);
}

QString MessageWindow::messageId() const
{
    return m_view->message().id;
}

void MessageWindow::closeEvent(QCloseEvent *ev)
{
    QSettings().setValue(QLatin1String(kGeometryKey), saveGeometry());
    QMainWindow::closeEvent(ev);
}

} // namespace zmail::ui
