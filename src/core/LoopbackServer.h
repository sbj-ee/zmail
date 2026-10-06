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
    // The OAuth `state` sent with this sign-in. When set, a redirect whose
    // state is missing or different gets a 400 and is otherwise ignored: the
    // listener keeps waiting (until the timeout) for the real one, so a
    // stray or forged request to the port can't end the sign-in.
    void setExpectedState(const QByteArray &state);
    int rejectedCallbacks() const { return m_rejected; }

signals:
    // Exactly one of code / error is non-empty. `state` matched the expected
    // one if setExpectedState() was called; the caller checks it again.
    void callbackReceived(const QString &code, const QString &state, const QString &error);
    void timedOut();
    // A redirect with a missing or wrong state was answered with 400 and
    // ignored; still listening.
    void callbackRejected();

private:
    void onConnection();
    void handle(QTcpSocket *sock);

    QTcpServer *m_server = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_done = false;
    QByteArray m_expectedState;
    int m_rejected = 0;
};

} // namespace zmail
