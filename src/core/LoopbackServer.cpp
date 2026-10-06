#include "LoopbackServer.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

namespace zmail {

namespace {
bool sameState(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}
} // namespace

LoopbackServer::LoopbackServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kDefaultTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        close();
        emit timedOut();
    });
    connect(m_server, &QTcpServer::newConnection, this, &LoopbackServer::onConnection);
}

LoopbackServer::~LoopbackServer() = default;

bool LoopbackServer::listen()
{
    m_done = false;
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        return false;
    }
    m_timeout->start();
    return true;
}

void LoopbackServer::close()
{
    m_timeout->stop();
    m_server->close();
}

quint16 LoopbackServer::port() const
{
    return m_server->serverPort();
}

QUrl LoopbackServer::redirectUri() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port()));
}

void LoopbackServer::setTimeoutMs(int ms)
{
    m_timeout->setInterval(ms);
}

int LoopbackServer::timeoutMs() const
{
    return m_timeout->interval();
}

void LoopbackServer::setExpectedState(const QByteArray &state)
{
    m_expectedState = state;
}

void LoopbackServer::onConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, this, [this, s]() { handle(s); });
    }
}

void LoopbackServer::handle(QTcpSocket *sock)
{
    // Wait for the full request head; cap the size to keep this robust.
    QByteArray buf = sock->property("buf").toByteArray() + sock->readAll();
    sock->setProperty("buf", buf);
    if (buf.size() > 16 * 1024) {
        sock->abort();
        return;
    }
    if (!buf.contains("\r\n\r\n")) {
        return;
    }
    const QList<QByteArray> line = buf.left(buf.indexOf("\r\n")).split(' ');
    auto reply = [sock](int code, const QByteArray &status, const QByteArray &body) {
        sock->write("HTTP/1.1 " + QByteArray::number(code) + ' ' + status +
                    "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                    "Connection: close\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\n\r\n" + body);
        sock->flush();
        sock->disconnectFromHost();
    };
    if (line.size() < 2 || line[0] != "GET") {
        reply(405, "Method Not Allowed", "");
        return;
    }
    const QUrl url(QString::fromLatin1(line[1]));
    const QUrlQuery q(url);
    if (url.path() != QLatin1String("/") || m_done || (!q.hasQueryItem(QStringLiteral("code")) &&
                                                       !q.hasQueryItem(QStringLiteral("error")))) {
        reply(404, "Not Found", "");
        return;
    }
    const QString error = q.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
    const QString code = q.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    const QString state = q.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
    if (!m_expectedState.isEmpty() &&
        (!q.hasQueryItem(QStringLiteral("state")) || !sameState(state.toLatin1(), m_expectedState))) {
        // Not our redirect: answer it, keep listening, keep the timeout.
        ++m_rejected;
        reply(400, "Bad Request",
              QByteArrayLiteral("<!doctype html><meta charset=utf-8><title>zmail</title>"
                                "<body style='font-family:sans-serif;margin:3em'><h2>This sign-in link isn't valid.</h2>"
                                "<p>It doesn't match the sign-in zmail started. zmail is still waiting for "
                                "the browser.</p>"));
        emit callbackRejected();
        return;
    }
    m_done = true;
    const QByteArray page = error.isEmpty()
        ? QByteArrayLiteral("<!doctype html><meta charset=utf-8><title>zmail</title>"
                            "<body style='font-family:sans-serif;margin:3em'><h2>zmail is signed in.</h2>"
                            "<p>You can close this tab and go back to zmail.</p>")
        : QByteArrayLiteral("<!doctype html><meta charset=utf-8><title>zmail</title>"
                            "<body style='font-family:sans-serif;margin:3em'><h2>Sign-in was not completed.</h2>"
                            "<p>Go back to zmail to try again.</p>");
    reply(200, "OK", page);
    close();
    emit callbackReceived(code, state, error);
}

} // namespace zmail
