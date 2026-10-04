#include "MockGoogle.h"

#include "ui/MessageListModel.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

namespace zmail::test {

namespace {
QByteArray b64url(const QByteArray &b)
{
    return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
QByteArray statusText(int s)
{
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}
QJsonObject gerror(int code, const QString &status, const QString &message)
{
    return {{QStringLiteral("error"),
             QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("status"), status},
                         {QStringLiteral("message"), message}}}};
}
} // namespace

MockGoogle::MockGoogle(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
            connect(s, &QTcpSocket::disconnected, this, [this, s] {
                m_buffers.remove(s);
                s->deleteLater();
            });
        }
    });
}

MockGoogle::~MockGoogle() = default;

bool MockGoogle::listen()
{
    return m_server->listen(QHostAddress::LocalHost, 0);
}

QUrl MockGoogle::baseUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_server->serverPort()));
}

ClientConfig MockGoogle::clientConfig() const
{
    ClientConfig c;
    c.clientId = QString::fromLatin1(kClientId);
    c.clientSecret = QString::fromLatin1(kClientSecret);
    c.authUri = authUri();
    c.tokenUri = tokenUri();
    return c;
}

int MockGoogle::count(const QString &prefix) const
{
    int n = 0;
    for (const QString &r : requests) {
        n += r.startsWith(prefix);
    }
    return n;
}

QString MockGoogle::addMessage(Message m, bool recordHistory)
{
    if (m.id.isEmpty()) {
        m.id = QString::number(0x190000000000LL + m_nextId++, 16);
    }
    if (m.threadId.isEmpty()) {
        m.threadId = m.id;
    }
    if (!m.date.isValid()) {
        m.date = QDateTime::currentDateTimeUtc();
    }
    if (m.snippet.isEmpty()) {
        m.snippet = m.text.left(120);
    }
    ++m_historyId;
    if (recordHistory) {
        m_history.append({m_historyId, QStringLiteral("messagesAdded"), m.id, m.labels});
    }
    m_messages.insert(m.id, m);
    return m.id;
}

void MockGoogle::deleteMessage(const QString &id)
{
    m_messages.remove(id);
    ++m_historyId;
    m_history.append({m_historyId, QStringLiteral("messagesDeleted"), id, {}});
}

void MockGoogle::setMessageLabels(const QString &id, const QStringList &add, const QStringList &remove)
{
    auto it = m_messages.find(id);
    if (it == m_messages.end()) {
        return;
    }
    for (const QString &r : remove) {
        if (it->labels.removeAll(r)) {
            ++m_historyId;
            m_history.append({m_historyId, QStringLiteral("labelsRemoved"), id, {r}});
        }
    }
    for (const QString &a : add) {
        if (!it->labels.contains(a)) {
            it->labels.append(a);
            ++m_historyId;
            m_history.append({m_historyId, QStringLiteral("labelsAdded"), id, {a}});
        }
    }
}

void MockGoogle::seedSystemLabels()
{
    for (const char *id : {"INBOX", "SENT", "SPAM", "TRASH", "DRAFT", "STARRED", "IMPORTANT", "UNREAD",
                           "CATEGORY_PERSONAL"}) {
        m_labels.append({QString::fromLatin1(id), QString::fromLatin1(id), QStringLiteral("system"), {}});
    }
}

void MockGoogle::seedDemo(int extraMessages)
{
    seedSystemLabels();
    const QHash<QString, QString> labelIds{{QStringLiteral("Work"), QStringLiteral("Label_1")},
                                           {QStringLiteral("Family"), QStringLiteral("Label_2")},
                                           {QStringLiteral("Receipts"), QStringLiteral("Label_3")},
                                           {QStringLiteral("Travel"), QStringLiteral("Label_4")},
                                           {QStringLiteral("Newsletters"), QStringLiteral("Label_5")}};
    m_labels.append({QStringLiteral("Label_1"), QStringLiteral("Work"), QStringLiteral("user"), QStringLiteral("#7b3fb5")});
    m_labels.append({QStringLiteral("Label_2"), QStringLiteral("Family"), QStringLiteral("user"), QStringLiteral("#00897b")});
    m_labels.append({QStringLiteral("Label_3"), QStringLiteral("Receipts"), QStringLiteral("user"), QStringLiteral("#e08e0b")});
    m_labels.append({QStringLiteral("Label_4"), QStringLiteral("Travel"), QStringLiteral("user"), QStringLiteral("#1e6fd9")});
    m_labels.append({QStringLiteral("Label_5"), QStringLiteral("Newsletters"), QStringLiteral("user"), QStringLiteral("#788088")});
    m_labels.append({QStringLiteral("Label_6"), QStringLiteral("Projects/zmail"), QStringLiteral("user"), QStringLiteral("#c2185b")});
    m_labels.append({QStringLiteral("Label_7"), QStringLiteral("Projects/Ham radio"), QStringLiteral("user"), QStringLiteral("#2e7d32")});

    for (const ui::MailItem &s : ui::sampleMail()) {
        Message m;
        for (const QString &mb : s.mailboxes) {
            if (mb == QLatin1String("In")) m.labels << QStringLiteral("INBOX");
            else if (mb == QLatin1String("Out")) m.labels << QStringLiteral("SENT");
            else if (mb == QLatin1String("Junk")) m.labels << QStringLiteral("SPAM");
            else if (mb == QLatin1String("Trash")) m.labels << QStringLiteral("TRASH");
        }
        if (m.labels.contains(QStringLiteral("SPAM"))) {
            m.labels.removeAll(QStringLiteral("INBOX"));
        }
        if (s.status == ui::MailStatus::Unread) m.labels << QStringLiteral("UNREAD");
        if (s.status == ui::MailStatus::Queued) continue; // not a Gmail concept
        if (!s.label.isEmpty()) m.labels << labelIds.value(s.label);
        m.from = s.mailboxes.contains(QStringLiteral("Out"))
                     ? QStringLiteral("Alex Morgan <alex.morgan@example.com>")
                     : QStringLiteral("%1 <%2>").arg(s.who, s.address);
        m.to = s.mailboxes.contains(QStringLiteral("Out")) ? QStringLiteral("%1 <%2>").arg(s.who, s.address)
                                                           : QStringLiteral("Alex Morgan <alex.morgan@example.com>");
        m.subject = s.subject;
        m.date = s.date;
        m.text = s.preview;
        if (s.who == QLatin1String("The Weekly Solder")) {
            m.html = QStringLiteral(
                "<html><head><style>h2{color:#1e6fd9}</style><script>track()</script></head><body>"
                "<h2>The Weekly Solder \u00b7 Issue 112</h2>"
                "<img src='https://pixel.weeklysolder.example/open.gif?u=123' width='1' height='1'>"
                "<p><b>This week:</b> measuring deep-sleep current on the ESP32, a 12&nbsp;km LoRa range test, "
                "and reader projects.</p><ul><li>Deep-sleep: 10&nbsp;\u00b5A is possible, if you cut the LED</li>"
                "<li>LoRa SF12 across the lake: 12.4&nbsp;km</li><li>Reader build: a QRP antenna tuner</li></ul>"
                "<p><a href='https://weeklysolder.example/112'>Read it on the web</a></p>"
                "<img src='https://cdn.weeklysolder.example/banner.png'></body></html>");
        }
        m.attachments = s.attachments;
        m.size = s.sizeBytes;
        addMessage(m, false);
    }
    const QStringList who{QStringLiteral("Dana Whitfield <dana.whitfield@example.org>"),
                          QStringLiteral("Tailspin Airlines <noreply@tailspin.example>"),
                          QStringLiteral("Wingtip Toys <hello@wingtip.example>"),
                          QStringLiteral("Omar Haddad <omar.haddad@example.net>"),
                          QStringLiteral("The Weekly Solder <issue@weeklysolder.example>")};
    const QStringList subj{QStringLiteral("Notes from Tuesday's call"), QStringLiteral("Fare alert: Denver from $129"),
                           QStringLiteral("Your order is ready for pickup"), QStringLiteral("Antenna tuner arrived"),
                           QStringLiteral("Issue %1: QRP field day recap")};
    const QDateTime base = ui::sampleMail().first().date;
    for (int i = 0; i < extraMessages; ++i) {
        Message m;
        m.labels = {QStringLiteral("INBOX")};
        if (i % 5 == 4) m.labels << QStringLiteral("Label_5");
        if (i % 7 == 3) m.labels << QStringLiteral("Label_6");
        if (i % 9 == 1) m.labels << QStringLiteral("Label_7");
        m.from = who.at(i % who.size());
        m.to = QStringLiteral("Alex Morgan <alex.morgan@example.com>");
        m.subject = subj.at(i % subj.size());
        if (m.subject.contains(QLatin1String("%1"))) {
            m.subject = m.subject.arg(100 - i);
        }
        m.date = base.addDays(-6 - i / 3).addSecs(-3600 * (i % 3));
        m.text = QStringLiteral("Message %1 of the demo mailbox. Nothing here is real.").arg(i + 1);
        m.html = i % 2 ? QStringLiteral("<p>Message <b>%1</b> of the demo mailbox.</p>"
                                        "<img src='https://tracker.example/pixel.gif'>").arg(i + 1)
                       : QString();
        m.size = 2000 + 397 * i;
        addMessage(m, false);
    }
}

QString MockGoogle::issueAccessToken()
{
    ++tokensIssued;
    const QString t = QString::fromLatin1(kAccessPrefix) + QString::number(tokensIssued);
    m_validAccess.append(t);
    return t;
}

QString MockGoogle::idToken() const
{
    const QByteArray header = b64url(R"({"alg":"RS256","typ":"JWT"})");
    const QByteArray payload = b64url(QJsonDocument(QJsonObject{{QStringLiteral("iss"), QStringLiteral("https://accounts.google.com")},
                                                                {QStringLiteral("aud"), QString::fromLatin1(kClientId)},
                                                                {QStringLiteral("email"), email},
                                                                {QStringLiteral("email_verified"), true}})
                                          .toJson(QJsonDocument::Compact));
    return QString::fromLatin1(header + '.' + payload + ".mock-signature");
}

bool MockGoogle::authorized(const QHash<QByteArray, QByteArray> &headers) const
{
    const QByteArray auth = headers.value("authorization");
    return auth.startsWith("Bearer ") && m_validAccess.contains(QString::fromLatin1(auth.mid(7)));
}

void MockGoogle::onReadyRead(QTcpSocket *s)
{
    QByteArray &buf = m_buffers[s];
    buf += s->readAll();
    const int end = buf.indexOf("\r\n\r\n");
    if (end < 0) {
        return;
    }
    const QList<QByteArray> lines = buf.left(end).split('\n');
    const QList<QByteArray> req = lines.value(0).trimmed().split(' ');
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int c = lines[i].indexOf(':');
        if (c > 0) {
            headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
        }
    }
    const int len = headers.value("content-length").toInt();
    if (buf.size() < end + 4 + len) {
        return;
    }
    const QByteArray body = buf.mid(end + 4, len);
    buf.clear();
    handle(s, req.value(0), QUrl(QString::fromLatin1(req.value(1))), headers, body);
}

void MockGoogle::reply(QTcpSocket *s, int status, const QByteArray &body, const QByteArray &contentType,
                       const QList<QPair<QByteArray, QByteArray>> &extra)
{
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    for (const auto &h : extra) {
        out += h.first + ": " + h.second + "\r\n";
    }
    out += "Connection: close\r\n\r\n" + body;
    s->write(out);
    s->disconnectFromHost();
}

void MockGoogle::replyJson(QTcpSocket *s, int status, const QJsonObject &o)
{
    reply(s, status, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject MockGoogle::messageJson(const Message &m, const QString &format) const
{
    QJsonArray headers{QJsonObject{{QStringLiteral("name"), QStringLiteral("From")}, {QStringLiteral("value"), m.from}},
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("To")}, {QStringLiteral("value"), m.to}},
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("Subject")}, {QStringLiteral("value"), m.subject}},
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("Date")},
                                   {QStringLiteral("value"), m.date.toString(Qt::RFC2822Date)}}};
    if (!m.cc.isEmpty()) {
        headers.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Cc")}, {QStringLiteral("value"), m.cc}});
    }
    const bool mixed = !m.attachments.isEmpty();
    headers.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Content-Type")},
                               {QStringLiteral("value"), mixed ? QStringLiteral("multipart/mixed; boundary=x")
                                                               : QStringLiteral("multipart/alternative; boundary=y")}});
    QJsonObject payload{{QStringLiteral("mimeType"), mixed ? QStringLiteral("multipart/mixed") : QStringLiteral("multipart/alternative")},
                        {QStringLiteral("headers"), headers}};
    if (format == QLatin1String("full")) {
        auto textPart = [](const QString &mime, const QString &data) {
            return QJsonObject{
                {QStringLiteral("mimeType"), mime},
                {QStringLiteral("filename"), QString()},
                {QStringLiteral("headers"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Content-Type")},
                                                                   {QStringLiteral("value"), mime + QStringLiteral("; charset=UTF-8")}}}},
                {QStringLiteral("body"), QJsonObject{{QStringLiteral("size"), int(data.toUtf8().size())},
                                                     {QStringLiteral("data"), QString::fromLatin1(b64url(data.toUtf8()))}}}};
        };
        QJsonArray alt{textPart(QStringLiteral("text/plain"), m.text)};
        if (!m.html.isEmpty()) {
            alt.append(textPart(QStringLiteral("text/html"), m.html));
        }
        QJsonArray parts;
        if (mixed) {
            parts.append(QJsonObject{{QStringLiteral("mimeType"), QStringLiteral("multipart/alternative")},
                                     {QStringLiteral("parts"), alt}});
            for (const QString &a : m.attachments) {
                parts.append(QJsonObject{{QStringLiteral("mimeType"), QStringLiteral("application/octet-stream")},
                                         {QStringLiteral("filename"), a.section(QStringLiteral(" ("), 0, 0)},
                                         {QStringLiteral("body"), QJsonObject{{QStringLiteral("attachmentId"), QStringLiteral("att1")},
                                                                              {QStringLiteral("size"), 1000}}}});
            }
        } else {
            parts = alt;
        }
        payload.insert(QStringLiteral("parts"), parts);
    }
    return {{QStringLiteral("id"), m.id},
            {QStringLiteral("threadId"), m.threadId},
            {QStringLiteral("labelIds"), QJsonArray::fromStringList(m.labels)},
            {QStringLiteral("snippet"), m.snippet.toHtmlEscaped()},
            {QStringLiteral("historyId"), QString::number(m_historyId)},
            {QStringLiteral("internalDate"), QString::number(m.date.toMSecsSinceEpoch())},
            {QStringLiteral("sizeEstimate"), m.size},
            {QStringLiteral("payload"), payload}};
}

void MockGoogle::handle(QTcpSocket *s, const QByteArray &method, const QUrl &url,
                        const QHash<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    const QString path = url.path();
    const QUrlQuery q(url);
    requests.append(QString::fromLatin1(method) + QLatin1Char(' ') + path);
    requestBodies.append(body);

    for (int i = 0; i < m_faults.size(); ++i) {
        Fault &f = m_faults[i];
        if (f.remaining > 0 && path.startsWith(f.pathPrefix)) {
            --f.remaining;
            QList<QPair<QByteArray, QByteArray>> extra;
            if (f.retryAfter >= 0) {
                extra.append({"Retry-After", QByteArray::number(f.retryAfter)});
            }
            reply(s, f.status, QJsonDocument(gerror(f.status, QStringLiteral("INJECTED"), QStringLiteral("injected fault")))
                                   .toJson(QJsonDocument::Compact),
                  "application/json", extra);
            return;
        }
    }

    // --- OAuth -------------------------------------------------------------
    if (path == QLatin1String("/o/oauth2/v2/auth")) {
        const QString redirect = q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded);
        if (q.queryItemValue(QStringLiteral("client_id")) != QLatin1String(kClientId) ||
            q.queryItemValue(QStringLiteral("code_challenge_method")) != QLatin1String("S256") ||
            q.queryItemValue(QStringLiteral("response_type")) != QLatin1String("code") ||
            !redirect.startsWith(QLatin1String("http://127.0.0.1:"))) {
            reply(s, 400, "bad auth request", "text/plain");
            return;
        }
        const QString code = QStringLiteral("4/mock-auth-code-") + QString::number(m_codes.size() + 1);
        m_codes.insert(code, {q.queryItemValue(QStringLiteral("code_challenge")).toLatin1(), redirect,
                              q.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded)});
        QUrl back(redirect);
        QUrlQuery bq;
        bq.addQueryItem(QStringLiteral("state"), q.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded));
        bq.addQueryItem(QStringLiteral("code"), code);
        bq.addQueryItem(QStringLiteral("scope"), q.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded));
        back.setQuery(bq);
        reply(s, 302, {}, "text/plain", {{"Location", back.toEncoded()}});
        return;
    }
    if (path == QLatin1String("/token") && method == "POST") {
        const QUrlQuery f(QString::fromLatin1(body));
        auto v = [&](const char *k) { return f.queryItemValue(QString::fromLatin1(k), QUrl::FullyDecoded); };
        if (v("client_id") != QLatin1String(kClientId) || v("client_secret") != QLatin1String(kClientSecret)) {
            replyJson(s, 401, {{QStringLiteral("error"), QStringLiteral("invalid_client")}});
            return;
        }
        const QString grant = v("grant_type");
        if (grant == QLatin1String("authorization_code")) {
            const QString code = v("code");
            if (!m_codes.contains(code)) {
                replyJson(s, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")}});
                return;
            }
            const Pending p = m_codes.take(code);
            lastVerifierChecked = v("code_verifier");
            const QByteArray expect = b64url(QCryptographicHash::hash(lastVerifierChecked.toLatin1(), QCryptographicHash::Sha256));
            pkceVerified = !lastVerifierChecked.isEmpty() && expect == p.challenge;
            if (!pkceVerified || v("redirect_uri") != p.redirect) {
                replyJson(s, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")},
                                   {QStringLiteral("error_description"), QStringLiteral("PKCE or redirect mismatch")}});
                return;
            }
            m_refreshRevoked = false;
            replyJson(s, 200, {{QStringLiteral("access_token"), issueAccessToken()},
                               {QStringLiteral("expires_in"), accessTokenLifetime},
                               {QStringLiteral("refresh_token"), QString::fromLatin1(kRefreshToken)},
                               {QStringLiteral("scope"), grantGmailScope ? p.scope : QStringLiteral("openid email")},
                               {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                               {QStringLiteral("id_token"), idToken()}});
            return;
        }
        if (grant == QLatin1String("refresh_token")) {
            if (m_refreshRevoked || v("refresh_token") != QLatin1String(kRefreshToken)) {
                replyJson(s, 400, {{QStringLiteral("error"), QStringLiteral("invalid_grant")},
                                   {QStringLiteral("error_description"), QStringLiteral("Token has been expired or revoked.")}});
                return;
            }
            replyJson(s, 200, {{QStringLiteral("access_token"), issueAccessToken()},
                               {QStringLiteral("expires_in"), accessTokenLifetime},
                               {QStringLiteral("token_type"), QStringLiteral("Bearer")}});
            return;
        }
        replyJson(s, 400, {{QStringLiteral("error"), QStringLiteral("unsupported_grant_type")}});
        return;
    }
    if (path == QLatin1String("/revoke")) {
        m_refreshRevoked = true;
        reply(s, 200, "{}");
        return;
    }

    // --- Gmail ---------------------------------------------------------------
    const QString api = QStringLiteral("/gmail/v1/users/me");
    if (!path.startsWith(api)) {
        reply(s, 404, "not found", "text/plain");
        return;
    }
    if (!authorized(headers)) {
        replyJson(s, 401, gerror(401, QStringLiteral("UNAUTHENTICATED"), QStringLiteral("Invalid Credentials")));
        return;
    }
    const QString rest = path.mid(api.size());
    if (rest == QLatin1String("/profile")) {
        replyJson(s, 200, {{QStringLiteral("emailAddress"), email},
                           {QStringLiteral("messagesTotal"), int(m_messages.size())},
                           {QStringLiteral("historyId"), QString::number(m_historyId)}});
        return;
    }
    if (rest == QLatin1String("/labels")) {
        QJsonArray arr;
        for (const Label &l : m_labels) {
            int total = 0, unread = 0;
            for (const Message &m : m_messages) {
                if (m.labels.contains(l.id)) {
                    ++total;
                    unread += m.labels.contains(QStringLiteral("UNREAD"));
                }
            }
            QJsonObject o{{QStringLiteral("id"), l.id}, {QStringLiteral("name"), l.name}, {QStringLiteral("type"), l.type},
                          {QStringLiteral("messagesTotal"), total}, {QStringLiteral("messagesUnread"), unread}};
            if (!l.color.isEmpty()) {
                o.insert(QStringLiteral("color"), QJsonObject{{QStringLiteral("backgroundColor"), l.color},
                                                              {QStringLiteral("textColor"), QStringLiteral("#ffffff")}});
            }
            arr.append(o);
        }
        replyJson(s, 200, {{QStringLiteral("labels"), arr}});
        return;
    }
    if (rest == QLatin1String("/messages")) {
        const QString label = q.queryItemValue(QStringLiteral("labelIds"));
        const int max = std::clamp(q.queryItemValue(QStringLiteral("maxResults")).toInt(), 1, 500);
        const int offset = q.queryItemValue(QStringLiteral("pageToken")).toInt();
        QList<Message> sel;
        for (const Message &m : m_messages) {
            if (label.isEmpty() || m.labels.contains(label)) {
                sel.append(m);
            }
        }
        std::sort(sel.begin(), sel.end(), [](const Message &a, const Message &b) { return a.date > b.date; });
        QJsonArray arr;
        for (int i = offset; i < std::min<int>(int(sel.size()), offset + max); ++i) {
            arr.append(QJsonObject{{QStringLiteral("id"), sel[i].id}, {QStringLiteral("threadId"), sel[i].threadId}});
        }
        QJsonObject o{{QStringLiteral("messages"), arr}, {QStringLiteral("resultSizeEstimate"), int(sel.size())}};
        if (offset + max < sel.size()) {
            o.insert(QStringLiteral("nextPageToken"), QString::number(offset + max));
        }
        replyJson(s, 200, o);
        return;
    }
    if (rest.startsWith(QLatin1String("/messages/"))) {
        QString id = rest.mid(10);
        const bool modify = id.endsWith(QLatin1String("/modify"));
        if (modify) {
            id.chop(7);
        }
        const bool trash = id.endsWith(QLatin1String("/trash"));
        if (trash) {
            id.chop(6);
        }
        if (!m_messages.contains(id)) {
            replyJson(s, 404, gerror(404, QStringLiteral("NOT_FOUND"), QStringLiteral("Requested entity was not found.")));
            return;
        }
        if (modify && method == "POST") {
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            QStringList add, remove;
            for (const auto &v : o.value(QStringLiteral("addLabelIds")).toArray()) add << v.toString();
            for (const auto &v : o.value(QStringLiteral("removeLabelIds")).toArray()) remove << v.toString();
            for (const QString &r : remove) modifyCalls << id + QStringLiteral(":-") + r;
            for (const QString &a : add) modifyCalls << id + QStringLiteral(":+") + a;
            setMessageLabels(id, add, remove);
            replyJson(s, 200, {{QStringLiteral("id"), id},
                               {QStringLiteral("labelIds"), QJsonArray::fromStringList(m_messages.value(id).labels)}});
            return;
        }
        if (trash && method == "POST") {
            trashCalls << id;
            setMessageLabels(id, {QStringLiteral("TRASH")}, {QStringLiteral("INBOX"), QStringLiteral("UNREAD")});
            replyJson(s, 200, {{QStringLiteral("id"), id},
                               {QStringLiteral("labelIds"), QJsonArray::fromStringList(m_messages.value(id).labels)}});
            return;
        }
        replyJson(s, 200, messageJson(m_messages.value(id), q.queryItemValue(QStringLiteral("format"))));
        return;
    }
    if (rest == QLatin1String("/history")) {
        const qint64 start = q.queryItemValue(QStringLiteral("startHistoryId")).toLongLong();
        if (start < m_minHistoryId) {
            replyJson(s, 404, gerror(404, QStringLiteral("NOT_FOUND"), QStringLiteral("Requested entity was not found.")));
            return;
        }
        QList<HistoryRecord> sel;
        for (const HistoryRecord &h : m_history) {
            if (h.id > start) {
                sel.append(h);
            }
        }
        const int offset = q.queryItemValue(QStringLiteral("pageToken")).toInt();
        QJsonArray arr;
        for (int i = offset; i < std::min<int>(int(sel.size()), offset + historyPageSize); ++i) {
            const HistoryRecord &h = sel[i];
            const QJsonObject msg{{QStringLiteral("id"), h.messageId},
                                  {QStringLiteral("threadId"), h.messageId},
                                  {QStringLiteral("labelIds"), QJsonArray::fromStringList(
                                       m_messages.contains(h.messageId) ? m_messages.value(h.messageId).labels : h.labels)}};
            QJsonObject entry{{QStringLiteral("message"), msg}};
            if (h.type.startsWith(QLatin1String("labels"))) {
                entry.insert(QStringLiteral("labelIds"), QJsonArray::fromStringList(h.labels));
            }
            arr.append(QJsonObject{{QStringLiteral("id"), QString::number(h.id)},
                                   {QStringLiteral("messages"), QJsonArray{msg}},
                                   {h.type, QJsonArray{entry}}});
        }
        QJsonObject o{{QStringLiteral("historyId"), QString::number(m_historyId)}};
        if (!arr.isEmpty()) {
            o.insert(QStringLiteral("history"), arr);
        }
        if (offset + historyPageSize < sel.size()) {
            o.insert(QStringLiteral("nextPageToken"), QString::number(offset + historyPageSize));
        }
        replyJson(s, 200, o);
        return;
    }
    replyJson(s, 404, gerror(404, QStringLiteral("NOT_FOUND"), QStringLiteral("no such endpoint")));
}

} // namespace zmail::test
