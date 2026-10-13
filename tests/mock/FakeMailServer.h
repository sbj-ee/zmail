#pragma once
// Tiny scripted IMAP / SMTP fake (plain TCP, localhost) for the app-password
// account's Test button. Tests and screenshots only; never linked into zmail.
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

namespace zmail::test {

// Scripted line-based fake: plain TCP, one connection at a time.
class FakeServer : public QObject
{
public:
    enum Kind { Imap, Smtp };
    FakeServer(Kind kind, bool acceptLogin)
        : m_kind(kind)
        , m_accept(acceptLogin)
    {
        if (!m_server.listen(QHostAddress::LocalHost)) {
            qFatal("fake server: listen failed");
        }
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *s = m_server.nextPendingConnection();
            if (silent) {
                return;
            }
            s->write(m_kind == Imap ? "* OK Gimap ready for requests\r\n" : "220 smtp.fake ESMTP ready\r\n");
            QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
                while (s->canReadLine()) {
                    const QByteArray line = s->readLine().trimmed();
                    received.append(line);
                    reply(s, line);
                }
            });
        });
    }
    quint16 port() const { return m_server.serverPort(); }
    QList<QByteArray> received;
    bool silent = false;

private:
    void reply(QTcpSocket *s, const QByteArray &line)
    {
        if (m_kind == Imap) {
            if (line.startsWith("a1 LOGIN")) {
                s->write(m_accept ? "* CAPABILITY IMAP4rev1 X-GM-EXT-1 IDLE CONDSTORE\r\na1 OK user authenticated\r\n"
                                  : "a1 NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)\r\n");
            } else if (line.startsWith("a2 LOGOUT")) {
                s->write("* BYE LOGOUT Requested\r\na2 OK 73 good day\r\n");
                s->disconnectFromHost();
            }
        } else {
            if (line.startsWith("EHLO")) {
                s->write("250-smtp.fake at your service\r\n250-AUTH LOGIN PLAIN XOAUTH2\r\n250 SMTPUTF8\r\n");
            } else if (line.startsWith("AUTH PLAIN")) {
                s->write(m_accept ? "235 2.7.0 Accepted\r\n"
                                  : "535-5.7.8 Username and Password not accepted.\r\n535 5.7.8 BadCredentials\r\n");
            } else if (line == "QUIT") {
                s->write("221 2.0.0 closing connection\r\n");
                s->disconnectFromHost();
            }
        }
    }
    QTcpServer m_server;
    Kind m_kind;
    bool m_accept;
};

} // namespace zmail::test
