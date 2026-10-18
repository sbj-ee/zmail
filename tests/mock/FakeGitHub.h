#pragma once
// A tiny HTTP/1.1 server standing in for GitHub (releases/latest and the asset
// downloads) in the self-update tests and screenshots: one response per
// connection, then close. Never touches the network.
#include <QHostAddress>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <map>

// A tiny HTTP/1.1 server: one response per connection, then close.
class MockGitHub : public QObject
{
public:
    struct Route {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        bool hang = false; // accept and never answer (timeouts)
        // Send the headers and only the first stallAfter bytes of the body, a
        // chunk every few milliseconds (so a progress bar moves), then go quiet
        // with the connection open (a download stuck part-way; -1: off).
        qint64 stallAfter = -1;
    };
    std::map<QString, Route> routes;
    QStringList requests;
    QByteArray lastUserAgent;
    QByteArray lastAccept;
    QTcpServer server;

    MockGitHub()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buf = new QByteArray;
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QObject::destroyed, [buf]() { delete buf; });
                connect(s, &QTcpSocket::readyRead, this, [this, s, buf]() {
                    buf->append(s->readAll());
                    if (!buf->contains("\r\n\r\n")) {
                        return;
                    }
                    handle(s, *buf);
                });
            }
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    QString url(const QString &path) const { return base() + path; }

    void handle(QTcpSocket *s, const QByteArray &req)
    {
        const QList<QByteArray> lines = req.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(first.value(1));
        requests << path;
        for (const QByteArray &l : lines) {
            const QByteArray low = l.toLower();
            if (low.startsWith("user-agent:")) {
                lastUserAgent = l.mid(11).trimmed();
            } else if (low.startsWith("accept:")) {
                lastAccept = l.mid(7).trimmed();
            }
        }
        Route r;
        if (auto it = routes.find(path); it != routes.end()) {
            r = it->second;
        } else {
            r.status = 404;
            r.body = R"({"message":"Not Found"})";
        }
        if (r.hang) {
            return;
        }
        const bool stall = r.stallAfter >= 0;
        QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + (r.status == 200 ? " OK" : " Error") + "\r\n";
        out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        for (const auto &h : r.headers) {
            out += h.first + ": " + h.second + "\r\n";
        }
        if (stall) {
            s->write(out + "\r\n");
            auto *sent = new qint64(0);
            auto *timer = new QTimer(s);
            connect(s, &QObject::destroyed, [sent]() { delete sent; });
            connect(timer, &QTimer::timeout, s, [s, sent, timer, body = r.body, limit = r.stallAfter]() {
                if (*sent >= limit) {
                    timer->stop();
                    return;
                }
                const qint64 n = qMin<qint64>(65536, limit - *sent);
                s->write(body.mid(*sent, n));
                s->flush();
                *sent += n;
            });
            timer->start(15);
            return;
        }
        out += "\r\n" + r.body;
        s->write(out);
        s->disconnectFromHost();
    }
};
