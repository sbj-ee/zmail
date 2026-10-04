#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace zmail {

// One-shot OAuth redirect listener on 127.0.0.1:<random port> (RFC 8252 §7.3).
class LoopbackServer : public QObject
{
    Q_OBJECT

public:
    explicit LoopbackServer(QObject *parent = nullptr);
    ~LoopbackServer() override;

    bool listen();              // random free port on 127.0.0.1
    void close();
    quint16 port() const;
    QUrl redirectUri() const;   // http://127.0.0.1:<port>
    // How long to wait for the browser to come back. Long enough for a
    // slow login (2FA on a phone, account chooser, consent screen) and
    // for finding the browser again on desktops (Wayland) where zmail
    // can't raise its own window.
    static constexpr int kDefaultTimeoutMs = 15 * 60 * 1000;
    void setTimeoutMs(int ms);  // default kDefaultTimeoutMs
    int timeoutMs() const;

signals:
    // Exactly one of code / error is non-empty. Query values are passed through
    // unvalidated; the caller checks `state`.
    void callbackReceived(const QString &code, const QString &state, const QString &error);
    void timedOut();

private:
    void onConnection();
    void handle(QTcpSocket *sock);

    QTcpServer *m_server = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_done = false;
};

} // namespace zmail
