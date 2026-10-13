#pragma once
// In-process Gmail-flavoured SMTP server for tests and screenshots: EHLO,
// AUTH PLAIN, MAIL FROM, RCPT TO, DATA (dot-unstuffed), RSET, NOOP, QUIT.
// Plain TCP on localhost (the app uses implicit TLS on 465). Like Gmail, it
// files each accepted message in the IMAP fake's Sent Mail when given one.
// Never linked into zmail. Passwords are fakes.
#include "FakeImapServer.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

namespace zmail::test {

class FakeSmtpServer : public QObject
{
public:
    struct Delivery
    {
        QByteArray from;
        QList<QByteArray> rcpts;
        QByteArray data; // as received, unstuffed, without the final "."
    };

    explicit FakeSmtpServer(FakeImapServer *imap = nullptr)
        : m_imap(imap)
    {
        if (imap) {
            user = imap->user;
            password = imap->password;
        }
        if (!m_server.listen(QHostAddress::LocalHost)) {
            qFatal("fake smtp: listen failed");
        }
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection()) {
                auto st = std::make_shared<State>();
                ++connections;
                s->write("220 smtp.gmail.com ESMTP fake - gsmtp\r\n");
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, st] { onRead(s, *st); });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    quint16 port() const { return m_server.serverPort(); }

    QString user = QStringLiteral("alex.morgan@gmail.com");
    QString password = QStringLiteral("abcdefghijklmnop"); // made up
    QList<Delivery> delivered;
    QList<QByteArray> commands;      // command lines; AUTH's argument is masked
    int connections = 0;
    QByteArray rejectRecipient;      // RCPT TO for this address answers 550
    bool rejectData = false;         // after DATA's ".": 552
    bool hangUpAfterData = false;    // drop the connection instead of answering "."

private:
    struct State
    {
        QByteArray buffer;
        bool authed = false;
        bool inData = false;
        Delivery cur;
    };

    void onRead(QTcpSocket *s, State &st)
    {
        st.buffer += s->readAll();
        int nl;
        while ((nl = st.buffer.indexOf("\r\n")) >= 0) {
            QByteArray line = st.buffer.left(nl);
            st.buffer.remove(0, nl + 2);
            if (st.inData) {
                if (line == ".") {
                    st.inData = false;
                    if (hangUpAfterData) {
                        s->abort();
                        return;
                    }
                    if (rejectData) {
                        s->write("552 5.3.4 Message too big (fake)\r\n");
                    } else {
                        delivered.append(st.cur);
                        if (m_imap) {
                            m_imap->deliver(st.cur.data, {QStringLiteral("\\Sent")}, {QStringLiteral("\\Seen")});
                        }
                        s->write("250 2.0.0 OK 1760000000 fake.42 - gsmtp\r\n");
                    }
                    st.cur = {};
                    continue;
                }
                if (line.startsWith('.')) {
                    line.remove(0, 1);
                }
                st.cur.data += line + "\r\n";
                continue;
            }
            const QByteArray up = line.toUpper();
            commands.append(up.startsWith("AUTH PLAIN") ? QByteArray("AUTH PLAIN ****") : line);
            if (up.startsWith("EHLO") || up.startsWith("HELO")) {
                s->write("250-smtp.gmail.com at your service\r\n250-SIZE 35882577\r\n250-8BITMIME\r\n"
                         "250-AUTH LOGIN PLAIN XOAUTH2\r\n250-ENHANCEDSTATUSCODES\r\n250 SMTPUTF8\r\n");
            } else if (up.startsWith("AUTH PLAIN ")) {
                const QByteArray dec = QByteArray::fromBase64(line.mid(11));
                const QList<QByteArray> parts = dec.split('\0');
                st.authed = parts.size() == 3 && parts.at(1) == user.toUtf8() && parts.at(2) == password.toUtf8();
                s->write(st.authed ? "235 2.7.0 Accepted\r\n"
                                   : "535-5.7.8 Username and Password not accepted.\r\n535 5.7.8 BadCredentials (fake)\r\n");
            } else if (up.startsWith("MAIL FROM:")) {
                if (!st.authed) {
                    s->write("530-5.7.0 Authentication Required.\r\n530 5.7.0 (fake)\r\n");
                    continue;
                }
                st.cur = {};
                st.cur.from = angle(line);
                s->write("250 2.1.0 OK\r\n");
            } else if (up.startsWith("RCPT TO:")) {
                const QByteArray a = angle(line);
                if (!rejectRecipient.isEmpty() && a == rejectRecipient) {
                    s->write("550-5.1.1 The email account that you tried to reach does not exist.\r\n550 5.1.1 (fake)\r\n");
                    continue;
                }
                st.cur.rcpts.append(a);
                s->write("250 2.1.5 OK\r\n");
            } else if (up == "DATA") {
                if (st.cur.rcpts.isEmpty()) {
                    s->write("503 5.5.1 RCPT first.\r\n");
                    continue;
                }
                st.inData = true;
                s->write("354 Go ahead\r\n");
            } else if (up == "RSET" || up == "NOOP") {
                s->write("250 2.0.0 OK\r\n");
            } else if (up == "QUIT") {
                s->write("221 2.0.0 closing connection\r\n");
                s->disconnectFromHost();
            } else {
                s->write("502 5.5.1 Unrecognized command.\r\n");
            }
        }
    }
    static QByteArray angle(const QByteArray &line)
    {
        const int lt = line.indexOf('<');
        const int gt = line.indexOf('>', lt);
        return lt >= 0 && gt > lt ? line.mid(lt + 1, gt - lt - 1) : QByteArray();
    }

    QPointer<FakeImapServer> m_imap;
    QTcpServer m_server;
};

} // namespace zmail::test
