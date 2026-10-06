#pragma once

#include <QMainWindow>

class QAction;

namespace zmail::ui {

class MessageView;
struct ViewMessage;

// A message in its own window (double-click in the list). Several can be open
// at once; new ones cascade from the last remembered size and position
// (QSettings "messageWindow/geometry").
class MessageWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MessageWindow(QWidget *parent = nullptr);

    MessageView *view() const { return m_view; }
    void setMessage(const ViewMessage &m);
    QString messageId() const;

    QAction *replyAction() const { return m_reply; }
    QAction *replyAllAction() const { return m_replyAll; }
    QAction *forwardAction() const { return m_forward; }
    QAction *deleteAction() const { return m_delete; }

signals:
    // kind is zmail::ReplyBuilder::Kind (Reply, ReplyAll, Forward).
    void composeRequested(const QString &id, int kind);
    void deleteRequested(const QString &id);
    void mailtoRequested(const QUrl &url);

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    MessageView *m_view = nullptr;
    QAction *m_reply = nullptr;
    QAction *m_replyAll = nullptr;
    QAction *m_forward = nullptr;
    QAction *m_delete = nullptr;
};

} // namespace zmail::ui
