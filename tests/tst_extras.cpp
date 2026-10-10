// 0.6.19: one-click unsubscribe (List-Unsubscribe, RFC 2369 / 8058), the
// "mark as read" delay as a setting, Gmail's vacation responder, calendar
// invitations shown as a card in the preview, and the sent-mail swoosh.
// All mail here is fake (MockGoogle, example.com addresses).
#include "MainWindow.hpp"
#include "core/AuthManager.h"
#include "core/CalendarInvite.h"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "core/Unsubscribe.h"
#include "core/Vacation.h"
#include "mock/MockGoogle.h"
#include "ui/ComposeWindow.h"
#include "ui/MessageListModel.h"
#include "ui/MessageView.h"
#include "ui/MessageWindow.h"
#include "ui/NewMailSound.h"
#include "ui/SoundDialog.h"
#include "ui/Theme.h"
#include "ui/VacationDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QDateEdit>
#include <QFile>
#include <QFrame>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <QTreeView>
#include <QtTest>
#include <memory>

using namespace zmail;
using namespace zmail::ui;
using zmail::test::MockGoogle;

namespace {
const QString kSettingsScope = QStringLiteral("https://www.googleapis.com/auth/gmail.settings.basic");

QNetworkAccessManager *browserNam()
{
    static QNetworkAccessManager nam;
    return &nam;
}

// A Google Calendar style invitation: folded lines, escaped text, a TZID.
const char *kInvite =
    "BEGIN:VCALENDAR\r\n"
    "PRODID:-//Example//Calendar//EN\r\n"
    "VERSION:2.0\r\n"
    "METHOD:REQUEST\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:America/Chicago\r\n"
    "BEGIN:STANDARD\r\n"
    "DTSTART:19701101T020000\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=America/Chicago:20261013T140000\r\n"
    "DTEND;TZID=America/Chicago:20261013T150000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=TU\r\n"
    "ORGANIZER;CN=Priya Raman:mailto:priya.raman@example.com\r\n"
    "ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=ACCEPTED;CN=Priya Ram\r\n"
    " an:mailto:priya.raman@example.com\r\n"
    "ATTENDEE;PARTSTAT=NEEDS-ACTION;CN=\"User, Demo\":mailto:demo.user@example.com\r\n"
    "ATTENDEE;PARTSTAT=DECLINED:mailto:sam@example.com\r\n"
    "SUMMARY:Boat launch planning\\, round 2\r\n"
    "LOCATION:Dock 4\\; Lake Mendota\r\n"
    "DESCRIPTION:Bring the charts.\\nAnd coffee.\r\n"
    "STATUS:CONFIRMED\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "DESCRIPTION:This is an event reminder\r\n"
    "TRIGGER:-P0DT0H30M0S\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

struct Fixture
{
    MockGoogle g;
    MemoryTokenStore store;
    QTemporaryDir dir;
    QString cachePath = QStringLiteral(":memory:");
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    QTreeView *list = nullptr;
    QSortFilterProxyModel *proxy = nullptr;
    MessageListModel *model = nullptr;
    QList<QUrl> opened; // what "open in the browser" was asked for

    SessionOptions options()
    {
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Ok;
        o.client.config = g.clientConfig();
        o.store = &store;
        o.apiBase = g.apiBase();
        o.revokeUri = g.revokeUri();
        o.cachePathOverride = cachePath;
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.rememberAccount = false;
        o.pollIntervalMs = 3600 * 1000;
        o.backoffBaseMs = 5;
        o.browserOpener = [](const QUrl &u) {
            QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
            QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
            return true;
        };
        return o;
    }

    QString seed(MockGoogle::Message m, const QString &subject, int minutesAgo = 1)
    {
        if (m.from.isEmpty()) {
            m.from = QStringLiteral("Lake News <news@lists.example.com>");
        }
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        if (m.text.isEmpty()) {
            m.text = QStringLiteral("Fake test mail.");
        }
        if (m.labels.isEmpty()) {
            m.labels = {QStringLiteral("INBOX"), QStringLiteral("UNREAD")};
        }
        m.date = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
        return g.addMessage(m, true);
    }

    bool start()
    {
        if (!g.listen()) {
            return false;
        }
        g.seedSystemLabels();
        return true;
    }

    bool open()
    {
        session = std::make_unique<MailSession>(options());
        session->signIn();
        if (!QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) ||
            !QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000)) {
            return false;
        }
        w = std::make_unique<MainWindow>();
        w->setUrlOpener([this](const QUrl &u) {
            opened.append(u);
            return true;
        });
        w->setSession(session.get());
        QMetaObject::invokeMethod(session.get(), "ready");
        w->show();
        if (!QTest::qWaitForWindowActive(w.get()) || !QTest::qWaitFor([this] { return w->isLive(); }, 5000)) {
            return false;
        }
        list = w->findChild<QTreeView *>(QStringLiteral("messageList"));
        proxy = qobject_cast<QSortFilterProxyModel *>(list->model());
        model = w->findChild<MessageListModel *>();
        return list && proxy && model;
    }

    // Select the message in the Inbox, and wait until its body is on show.
    bool show(const QString &id)
    {
        if (!QTest::qWaitFor([&] { return model->rowForId(id) >= 0; }, 10000)) {
            return false;
        }
        const QModelIndex idx = proxy->mapFromSource(model->index(model->rowForId(id), 0));
        list->setCurrentIndex(idx);
        return QTest::qWaitFor([&] {
            return w->shownMessageId() == id && !w->messageView()->message().loading &&
                   session->cache()->message(id).extrasVersion >= MailCache::kExtrasVersion;
        }, 10000);
    }

    QFrame *bar() const { return w->messageView()->findChild<QFrame *>(QStringLiteral("unsubscribeBar")); }
    QPushButton *button() const { return w->messageView()->findChild<QPushButton *>(QStringLiteral("unsubscribeButton")); }
    QString barText() const { return w->messageView()->findChild<QLabel *>(QStringLiteral("unsubscribeText"))->text(); }
    QString status() const { return w->statusBar()->currentMessage(); }
};
} // namespace

class TstExtras : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        applyTheme(ThemeMode::Light);
        Unsubscribe::setLoopbackAllowedForTests(true);
    }
    void init() { QSettings().clear(); }

    // ---- List-Unsubscribe ------------------------------------------------

    void unsubscribeHeaderIsReadCarefully()
    {
        using Method = UnsubscribeInfo::Method;
        // Both headers: one click.
        UnsubscribeInfo i = Unsubscribe::parse(
            QStringLiteral("<mailto:leave@lists.example.com?subject=unsubscribe>, <https://lists.example.com/u?id=42>"),
            QStringLiteral("List-Unsubscribe=One-Click"));
        QCOMPARE(i.method(), Method::OneClick);
        QCOMPARE(i.https, QUrl(QStringLiteral("https://lists.example.com/u?id=42")));
        QCOMPARE(i.target(), QStringLiteral("lists.example.com"));
        // No Post header: the mail way is the automatic one left.
        i = Unsubscribe::parse(QStringLiteral("<https://lists.example.com/u?id=42>, <mailto:leave@lists.example.com>"), {});
        QCOMPARE(i.method(), Method::Mail);
        QCOMPARE(i.target(), QStringLiteral("leave@lists.example.com"));
        // A web page only: the browser.
        i = Unsubscribe::parse(QStringLiteral("<https://lists.example.com/u?id=42>"), {});
        QCOMPARE(i.method(), Method::Web);
        // A header folded in the middle of a long URL.
        i = Unsubscribe::parse(QStringLiteral("<https://lists.example.com/u?id=4 2>"), QStringLiteral("List-Unsubscribe=One-Click"));
        QCOMPARE(i.https, QUrl(QStringLiteral("https://lists.example.com/u?id=42")));
        // Not https, the machine itself, the LAN, credentials in the URL,
        // a mailto with nobody in it, other schemes: none of them is used.
        for (const char *bad : {"<http://lists.example.com/u>", "<https://127.0.0.1/u>", "<https://192.168.1.1/u>",
                                "<https://localhost/u>", "<https://user:pw@lists.example.com/u>", "<mailto:?subject=x>",
                                "<javascript:alert(1)>", "<file:///etc/passwd>", "https://lists.example.com/u", ""}) {
            Unsubscribe::setLoopbackAllowedForTests(false);
            const UnsubscribeInfo none = Unsubscribe::parse(QString::fromLatin1(bad), QStringLiteral("List-Unsubscribe=One-Click"));
            Unsubscribe::setLoopbackAllowedForTests(true);
            QVERIFY2(!none.available(), bad);
        }
        // One-click is only for https, even with the Post header.
        i = Unsubscribe::parse(QStringLiteral("<mailto:leave@lists.example.com>"), QStringLiteral("List-Unsubscribe=One-Click"));
        QCOMPARE(i.method(), Method::Mail);
    }

    void oneClickUnsubscribe()
    {
        Fixture f;
        QVERIFY(f.start());
        MockGoogle::Message m;
        m.listUnsubscribe = QStringLiteral("<%1>, <mailto:leave@lists.example.com>").arg(f.g.unsubscribeUrl(QStringLiteral("tok42")).toString());
        m.listUnsubscribePost = QStringLiteral("List-Unsubscribe=One-Click");
        const QString id = f.seed(m, QStringLiteral("Lake news, October"));
        const QString plain = f.seed({}, QStringLiteral("From a person"), 2);
        QVERIFY(f.open());

        // An ordinary message has no bar.
        QVERIFY(f.show(plain));
        QVERIFY(!f.bar()->isVisible());
        // The list mail has, naming where the request goes.
        QVERIFY(f.show(id));
        QVERIFY(f.bar()->isVisible());
        QVERIFY(f.button()->isVisible());
        QVERIFY2(f.barText().contains(QStringLiteral("127.0.0.1")), qPrintable(f.barText()));
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("menuActionUnsubscribe"))->isEnabled());

        const int sendsBefore = f.g.sendCalls;
        f.w->unsubscribe(id, /*confirm=*/false);
        QTRY_COMPARE(f.g.unsubscribePosts.size(), 1);
        // RFC 8058: exactly this body, form-encoded, to the URL as written.
        QCOMPARE(f.g.unsubscribePosts.first(),
                 QStringLiteral("/unsubscribe/tok42|application/x-www-form-urlencoded|List-Unsubscribe=One-Click"));
        QCOMPARE(f.g.sendCalls, sendsBefore); // no mail went out as well
        QTRY_VERIFY(f.session->cache()->message(id).unsubscribedMs > 0);
        QTRY_VERIFY2(f.barText().startsWith(QStringLiteral("You unsubscribed from this mailing list on")), qPrintable(f.barText()));
        QVERIFY(f.bar()->isVisible());
        QVERIFY(!f.button()->isVisible()); // nothing left to press
        QVERIFY2(f.status().startsWith(QStringLiteral("Unsubscribed from Lake News.")), qPrintable(f.status()));
        // It is remembered with the message: still said after looking elsewhere.
        QVERIFY(f.show(plain));
        QVERIFY(f.show(id));
        QVERIFY(!f.button()->isVisible());
        QVERIFY(f.opened.isEmpty());
    }

    void unsubscribeThatTheListRefusesIsSaidSo()
    {
        Fixture f;
        QVERIFY(f.start());
        f.g.unsubscribeStatus = 500;
        MockGoogle::Message m;
        m.listUnsubscribe = QStringLiteral("<%1>").arg(f.g.unsubscribeUrl(QStringLiteral("x")).toString());
        m.listUnsubscribePost = QStringLiteral("List-Unsubscribe=One-Click");
        const QString id = f.seed(m, QStringLiteral("Lake news, November"));
        QVERIFY(f.open());
        QVERIFY(f.show(id));
        // The button asks first; the test answers Yes.
        QString asked;
        QTimer::singleShot(200, [&asked]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box) {
                asked = box->text();
                box->button(QMessageBox::Yes)->click();
            }
        });
        f.button()->click();
        QVERIFY2(asked.startsWith(QStringLiteral("Unsubscribe from the mailing list of Lake News?")), qPrintable(asked));
        QVERIFY2(asked.contains(QStringLiteral("zmail will tell 127.0.0.1 to take you off the list.")), qPrintable(asked));
        QTRY_COMPARE(f.g.unsubscribePosts.size(), 1);
        QTRY_VERIFY2(f.status().startsWith(QStringLiteral("Couldn't unsubscribe: its server answered HTTP 500")), qPrintable(f.status()));
        QCOMPARE(f.session->cache()->message(id).unsubscribedMs, qint64(0));
        QVERIFY(f.button()->isVisible()); // still on offer
    }

    void unsubscribeByMailAndByWebPage()
    {
        Fixture f;
        QVERIFY(f.start());
        MockGoogle::Message byMail;
        byMail.listUnsubscribe = QStringLiteral("<mailto:leave@lists.example.com?subject=unsubscribe%20demo.user>");
        const QString mailId = f.seed(byMail, QStringLiteral("Mail way"));
        MockGoogle::Message byWeb;
        byWeb.listUnsubscribe = QStringLiteral("<https://lists.example.com/preferences?u=7>");
        const QString webId = f.seed(byWeb, QStringLiteral("Web way"), 2);
        MockGoogle::Message spam;
        spam.listUnsubscribe = byMail.listUnsubscribe;
        spam.labels = {QStringLiteral("SPAM"), QStringLiteral("UNREAD")};
        const QString spamId = f.seed(spam, QStringLiteral("Spam with a way out"), 3);
        QVERIFY(f.open());

        QVERIFY(f.show(mailId));
        QVERIFY2(f.barText().contains(QStringLiteral("leave@lists.example.com")), qPrintable(f.barText()));
        const int sendsBefore = f.g.sendCalls;
        f.w->unsubscribe(mailId, false);
        QTRY_COMPARE(f.g.sendCalls, sendsBefore + 1);
        const QString raw = QString::fromUtf8(f.g.lastRaw);
        QVERIFY2(raw.contains(QStringLiteral("To: leave@lists.example.com")), qPrintable(raw));
        QVERIFY2(raw.contains(QStringLiteral("Subject: unsubscribe demo.user")), qPrintable(raw));
        QTRY_VERIFY(f.session->cache()->message(mailId).unsubscribedMs > 0);
        QVERIFY(f.g.unsubscribePosts.isEmpty());

        // A page only: the browser gets it, and zmail doesn't claim it is done.
        QVERIFY(f.show(webId));
        f.w->unsubscribe(webId, false);
        QCOMPARE(f.opened, QList<QUrl>{QUrl(QStringLiteral("https://lists.example.com/preferences?u=7"))});
        QCOMPARE(f.session->cache()->message(webId).unsubscribedMs, qint64(0));
        QVERIFY(f.button()->isVisible());

        // Spam is never answered, whatever its header offers.
        f.session->sync()->fetchBody(spamId, [](const CachedMessage &, const QString &) {});
        QTRY_VERIFY(f.session->cache()->message(spamId).extrasVersion >= MailCache::kExtrasVersion);
        QVERIFY(!f.session->cache()->message(spamId).listUnsubscribe.isEmpty());
        const int sends = f.g.sendCalls;
        f.w->unsubscribe(spamId, false);
        QVERIFY2(f.status().startsWith(QStringLiteral("zmail doesn't unsubscribe from spam")), qPrintable(f.status()));
        QTest::qWait(150);
        QCOMPARE(f.g.sendCalls, sends);
    }

    // A body cached by an older zmail has no unsubscribe header or invitation
    // stored: it is shown at once as it is, and read again from Gmail.
    void bodiesCachedBeforeAreReadAgainOnce()
    {
        Fixture f;
        QVERIFY(f.dir.isValid());
        f.cachePath = f.dir.filePath(QStringLiteral("zmail.db"));
        QVERIFY(f.start());
        MockGoogle::Message m;
        m.listUnsubscribe = QStringLiteral("<mailto:leave@lists.example.com>");
        m.calendar = QString::fromLatin1(kInvite);
        const QString id = f.seed(m, QStringLiteral("Cached long ago"));
        QVERIFY(f.open());
        bool fetched = false;
        f.session->sync()->fetchBody(id, [&](const CachedMessage &, const QString &) { fetched = true; });
        QTRY_VERIFY(fetched);
        {
            // As 0.6.18 left it: a body, and none of the newer columns filled.
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("tst-extras-old"));
            db.setDatabaseName(f.cachePath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("UPDATE messages SET extras_ver = 0, list_unsubscribe = NULL, calendar = NULL")));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("tst-extras-old"));
        QCOMPARE(f.session->cache()->message(id).extrasVersion, 0);
        QVERIFY(f.session->cache()->message(id).hasBody);

        const int fullBefore = f.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/") + id);
        QVERIFY(f.show(id));
        QVERIFY(f.bar()->isVisible());
        QVERIFY(f.w->messageView()->calendarText().contains(QStringLiteral("Boat launch planning")));
        QCOMPARE(f.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/") + id), fullBefore + 1);
        // ...and not a second time.
        const QString other = f.seed({}, QStringLiteral("Another"), 5);
        f.session->sync()->pollNow(true);
        QVERIFY(f.show(other));
        QVERIFY(f.show(id));
        QTest::qWait(150);
        QCOMPARE(f.g.count(QStringLiteral("GET /gmail/v1/users/me/messages/") + id), fullBefore + 1);
    }

    // ---- Calendar invitations ------------------------------------------------

    void calendarIsDecoded()
    {
        const CalendarEvent ev = CalendarInvite::parse(QString::fromLatin1(kInvite));
        QVERIFY(ev.valid);
        QCOMPARE(ev.method, QStringLiteral("REQUEST"));
        QCOMPARE(ev.kind(), CalendarEvent::Kind::Invitation);
        QCOMPARE(ev.kindText(), QStringLiteral("Invitation"));
        QCOMPARE(ev.summary, QStringLiteral("Boat launch planning, round 2"));
        QCOMPARE(ev.location, QStringLiteral("Dock 4; Lake Mendota"));
        QCOMPARE(ev.description, QStringLiteral("Bring the charts.\nAnd coffee.")); // not the alarm's
        QVERIFY(!ev.allDay);
        if (QTimeZone("America/Chicago").isValid()) {
            // 2 pm Central Daylight Time is 19:00 UTC.
            QCOMPARE(ev.start.toUTC(), QDateTime(QDate(2026, 10, 13), QTime(19, 0), Qt::UTC));
            QCOMPARE(ev.end.toUTC(), QDateTime(QDate(2026, 10, 13), QTime(20, 0), Qt::UTC));
        }
        QCOMPARE(ev.recurrence, QStringLiteral("Repeats weekly"));
        QCOMPARE(ev.organizerText(), QStringLiteral("Priya Raman <priya.raman@example.com>"));
        QCOMPARE(ev.attendees.size(), 3);
        QCOMPARE(ev.attendees.at(0).name, QStringLiteral("Priya Raman")); // unfolded
        QCOMPARE(ev.attendees.at(1).name, QStringLiteral("User, Demo"));  // quoted, with a comma
        QCOMPARE(ev.attendees.at(1).address, QStringLiteral("demo.user@example.com"));
        QCOMPARE(ev.attendeesText(), QStringLiteral("Priya Raman (accepted), User, Demo, sam@example.com (declined)"));
        QCOMPARE(ev.attendeesText(2), QStringLiteral("Priya Raman (accepted), User, Demo, and 1 more"));
        const QString when = ev.whenText();
        QVERIFY2(when.contains(QStringLiteral("2026")) && when.contains(QStringLiteral(" – ")), qPrintable(when));
    }

    void calendarDatesAndKinds()
    {
        // All day, three days: DTEND is the day after the last.
        CalendarEvent ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nMETHOD:PUBLISH\nBEGIN:VEVENT\nDTSTART;VALUE=DATE:20261224\nDTEND;VALUE=DATE:20261227\n"
            "SUMMARY:Cabin\nEND:VEVENT\nEND:VCALENDAR\n"));
        QVERIFY(ev.valid);
        QVERIFY(ev.allDay);
        QCOMPARE(ev.kindText(), QStringLiteral("Event"));
        QCOMPARE(ev.whenText(), QStringLiteral("Thursday, December 24, 2026 – Saturday, December 26, 2026 (all day)"));
        // One day.
        ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART;VALUE=DATE:20261224\nDTEND;VALUE=DATE:20261225\nEND:VEVENT\nEND:VCALENDAR"));
        QCOMPARE(ev.whenText(), QStringLiteral("Thursday, December 24, 2026 (all day)"));
        // UTC start with a duration, cancelled.
        ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nMETHOD:CANCEL\nBEGIN:VEVENT\nDTSTART:20261013T190000Z\nDURATION:PT1H30M\nSUMMARY:Off\n"
            "STATUS:CANCELLED\nEND:VEVENT\nEND:VCALENDAR"));
        QVERIFY(ev.valid);
        QCOMPARE(ev.kind(), CalendarEvent::Kind::Cancelled);
        QCOMPARE(ev.start, QDateTime(QDate(2026, 10, 13), QTime(19, 0), Qt::UTC));
        QCOMPARE(ev.start.secsTo(ev.end), qint64(5400));
        // Someone's answer.
        ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nMETHOD:REPLY\nBEGIN:VEVENT\nDTSTART:20261013T190000Z\n"
            "ATTENDEE;PARTSTAT=TENTATIVE;CN=Sam:mailto:sam@example.com\nEND:VEVENT\nEND:VCALENDAR"));
        QCOMPARE(ev.kindText(), QStringLiteral("Reply to an invitation"));
        QCOMPARE(ev.attendeesText(), QStringLiteral("Sam (maybe)"));
        // Outlook's Windows zone names.
        ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART;TZID=\"Central Standard Time\":20261013T140000\nEND:VEVENT\nEND:VCALENDAR"));
        QVERIFY(ev.valid);
        if (QTimeZone("America/Chicago").isValid()) {
            QCOMPARE(ev.start.toUTC(), QDateTime(QDate(2026, 10, 13), QTime(19, 0), Qt::UTC));
        }
        // Only the first event; junk and an event with no start are nothing.
        ev = CalendarInvite::parse(QStringLiteral(
            "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART:20261013T190000Z\nSUMMARY:One\nEND:VEVENT\n"
            "BEGIN:VEVENT\nDTSTART:20261113T190000Z\nSUMMARY:Two\nEND:VEVENT\nEND:VCALENDAR"));
        QCOMPARE(ev.summary, QStringLiteral("One"));
        QVERIFY(!CalendarInvite::parse(QStringLiteral("hello")).valid);
        QVERIFY(!CalendarInvite::parse({}).valid);
        QVERIFY(!CalendarInvite::parse(QStringLiteral("BEGIN:VCALENDAR\nBEGIN:VEVENT\nSUMMARY:x\nEND:VEVENT\nEND:VCALENDAR")).valid);
        QVERIFY(CalendarInvite::isCalendarPart(QStringLiteral("TEXT/Calendar"), {}));
        QVERIFY(CalendarInvite::isCalendarPart(QStringLiteral("application/octet-stream"), QStringLiteral("Invite.ICS")));
        QVERIFY(!CalendarInvite::isCalendarPart(QStringLiteral("text/plain"), QStringLiteral("notes.txt")));
    }

    void invitationShowsAsACardInThePreview()
    {
        Fixture f;
        QVERIFY(f.start());
        MockGoogle::Message inlineInvite;
        inlineInvite.from = QStringLiteral("Priya Raman <priya.raman@example.com>");
        inlineInvite.calendar = QString::fromLatin1(kInvite);
        const QString id = f.seed(inlineInvite, QStringLiteral("Invitation: Boat launch planning"));
        // The same thing sent only as a file.
        MockGoogle::Message attached;
        attached.from = inlineInvite.from;
        attached.attachments = {QStringLiteral("invite.ics")};
        attached.attachmentData = {QByteArray(kInvite).replace("round 2", "round 3")};
        const QString fileId = f.seed(attached, QStringLiteral("Calendar file"), 2);
        const QString plain = f.seed({}, QStringLiteral("No event here"), 3);
        QVERIFY(f.open());

        auto *card = f.w->messageView()->findChild<QFrame *>(QStringLiteral("calendarCard"));
        QVERIFY(card);
        QVERIFY(f.show(plain));
        QVERIFY(!card->isVisible());
        QCOMPARE(f.w->messageView()->calendarText(), QString());

        QVERIFY(f.show(id));
        QVERIFY(card->isVisible());
        const QStringList lines = f.w->messageView()->calendarText().split(QLatin1Char('\n'));
        QCOMPARE(lines.value(0), QStringLiteral("Invitation: Boat launch planning, round 2"));
        QVERIFY2(lines.value(1).startsWith(QStringLiteral("When: ")) && lines.value(1).endsWith(QStringLiteral("Repeats weekly")),
                 qPrintable(lines.value(1)));
        QCOMPARE(lines.value(2), QStringLiteral("Where: Dock 4; Lake Mendota"));
        QCOMPARE(lines.value(3), QStringLiteral("Organizer: Priya Raman <priya.raman@example.com>"));
        QCOMPARE(lines.value(4), QStringLiteral("Guests: Priya Raman (accepted), User, Demo, sam@example.com (declined)"));
        // The card's text is the sender's: shown as text, never as markup.
        QVERIFY(card->findChild<QLabel *>(QStringLiteral("calendarCardText"))->text().contains(QStringLiteral("Dock 4; Lake Mendota")));

        QVERIFY(f.show(fileId));
        QTRY_VERIFY(card->isVisible());
        QVERIFY(f.w->messageView()->calendarText().startsWith(QStringLiteral("Invitation: Boat launch planning, round 3")));

        // In a window of its own, too.
        MessageWindow *mw = f.w->openMessageWindowFor(id);
        QVERIFY(mw);
        QTRY_VERIFY(mw->view()->findChild<QFrame *>(QStringLiteral("calendarCard"))->isVisible());
        mw->close();
        QVERIFY(f.show(plain));
        QVERIFY(!card->isVisible());
    }

    // ---- Mark as read after... ------------------------------------------------

    void markReadDelayIsASetting()
    {
        Fixture f;
        QVERIFY(f.start());
        const QString a = f.seed({}, QStringLiteral("First"));
        const QString b = f.seed({}, QStringLiteral("Second"), 2);
        const QString c = f.seed({}, QStringLiteral("Third"), 3);
        QVERIFY(f.open());
        auto *menu = f.w->findChild<QMenu *>(QStringLiteral("menuMarkReadDelay"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 5);
        const auto checked = [menu]() {
            QStringList on;
            for (QAction *act : menu->actions()) {
                if (act->isChecked()) {
                    on << act->objectName();
                }
            }
            return on.join(QLatin1Char(' '));
        };
        // The tests run with the delay forced to 0: "Immediately".
        QCOMPARE(checked(), QStringLiteral("actionMarkReadDelay0"));
        const auto unread = [&](const QString &id) { return f.session->cache()->message(id).unread(); };

        // Only When I Mark Them: a look leaves it unread, however long.
        f.w->findChild<QAction *>(QStringLiteral("actionMarkReadDelayNever"))->trigger();
        QCOMPARE(f.w->markReadDelayMs(), MainWindow::kMarkReadNever);
        QCOMPARE(checked(), QStringLiteral("actionMarkReadDelayNever"));
        QCOMPARE(QSettings().value(QStringLiteral("viewer/markReadDelayMs")).toInt(), -1);
        QVERIFY(f.show(a));
        QTest::qWait(300);
        QVERIFY(unread(a));
        QCOMPARE(f.g.modifyCalls.filter(a).size(), 0);
        // ...in a window of its own as well.
        MessageWindow *mw = f.w->openMessageWindowFor(a);
        QVERIFY(mw);
        QTest::qWait(200);
        QVERIFY(unread(a));
        mw->close();

        // After 3 Seconds is remembered; a window follows the delay too.
        f.w->findChild<QAction *>(QStringLiteral("actionMarkReadDelay3000"))->trigger();
        QCOMPARE(QSettings().value(QStringLiteral("viewer/markReadDelayMs")).toInt(), 3000);
        QCOMPARE(checked(), QStringLiteral("actionMarkReadDelay3000"));
        f.w->setMarkReadDelayMs(400); // the same path, without a three-second test
        mw = f.w->openMessageWindowFor(b);
        QVERIFY(mw);
        QTest::qWait(150);
        QVERIFY(unread(b));
        QTRY_VERIFY_WITH_TIMEOUT(!unread(b), 3000);
        mw->close();
        // Closed before the delay is up: it stays unread.
        mw = f.w->openMessageWindowFor(c);
        QVERIFY(mw);
        mw->close();
        QTest::qWait(700);
        QVERIFY(unread(c));

        // Immediately.
        f.w->findChild<QAction *>(QStringLiteral("actionMarkReadDelay0"))->trigger();
        QVERIFY(f.show(c));
        QTRY_VERIFY(!unread(c));
    }

    // ---- Vacation responder -------------------------------------------------

    void vacationSettingsAsGmailStoresThem()
    {
        VacationSettings v;
        v.enabled = true;
        v.subject = QStringLiteral("Away");
        v.text = QStringLiteral("Back on the 20th.");
        v.contactsOnly = true;
        v.firstDay = QDate(2026, 10, 12);
        v.lastDay = QDate(2026, 10, 19);
        const QJsonObject o = v.toJson();
        QCOMPARE(o.value(QStringLiteral("enableAutoReply")).toBool(), true);
        QCOMPARE(o.value(QStringLiteral("responseSubject")).toString(), QStringLiteral("Away"));
        QCOMPARE(o.value(QStringLiteral("responseBodyPlainText")).toString(), QStringLiteral("Back on the 20th."));
        QCOMPARE(o.value(QStringLiteral("restrictToContacts")).toBool(), true);
        // Epoch milliseconds, as strings: midnight UTC of the first day, and
        // the midnight that ends the last one.
        QCOMPARE(o.value(QStringLiteral("startTime")).toString(), QStringLiteral("1791763200000"));
        QCOMPARE(o.value(QStringLiteral("endTime")).toString(), QStringLiteral("1792454400000"));
        QCOMPARE(VacationSettings::fromJson(o), v);
        // No dates: none sent.
        v.firstDay = {};
        v.lastDay = {};
        QVERIFY(!v.toJson().contains(QStringLiteral("startTime")));
        QVERIFY(!v.toJson().contains(QStringLiteral("endTime")));
        QCOMPARE(v.summary(), QStringLiteral("On"));
        // An HTML-only reply (written in Gmail) reads as its text.
        const VacationSettings html = VacationSettings::fromJson(
            {{QStringLiteral("enableAutoReply"), true}, {QStringLiteral("responseBodyHtml"), QStringLiteral("<p>Gone <b>fishing</b>.</p>")}});
        QCOMPARE(html.text, QStringLiteral("Gone fishing."));
        QCOMPARE(VacationSettings().summary(), QStringLiteral("Off"));
    }

    void vacationResponderIsReadAndSaved()
    {
        Fixture f;
        QVERIFY(f.start());
        f.g.vacation = {{QStringLiteral("enableAutoReply"), false},
                        {QStringLiteral("responseSubject"), QStringLiteral("Old subject")},
                        {QStringLiteral("responseBodyPlainText"), QStringLiteral("Old text")}};
        f.seed({}, QStringLiteral("Mail"));
        QVERIFY(f.open());
        auto *label = f.w->statusBar()->findChild<QLabel *>(QStringLiteral("vacationLabel"));
        QVERIFY(label);
        QVERIFY(!label->isVisible());
        QVERIFY(!f.session->auth()->hasScopes(AuthManager::settingsScopes()));

        f.w->findChild<QAction *>(QStringLiteral("actionVacation"))->trigger();
        auto *dlg = f.w->findChild<VacationDialog *>();
        QVERIFY(dlg);
        QTRY_VERIFY(!dlg->saving()); // what Gmail has is in the form
        QCOMPARE(dlg->settings().subject, QStringLiteral("Old subject"));
        QCOMPARE(dlg->settings().text, QStringLiteral("Old text"));
        QVERIFY(!dlg->settings().enabled);
        QCOMPARE(dlg->statusText(), QStringLiteral("Now: Off"));
        QVERIFY(!dlg->findChild<QLineEdit *>(QStringLiteral("vacationSubject"))->isEnabled()); // off: nothing to fill in

        // On, with nothing to say: not sent.
        dlg->findChild<QCheckBox *>(QStringLiteral("vacationEnable"))->setChecked(true);
        dlg->findChild<QLineEdit *>(QStringLiteral("vacationSubject"))->clear();
        dlg->findChild<QPlainTextEdit *>(QStringLiteral("vacationText"))->clear();
        dlg->save();
        QCOMPARE(dlg->statusText(), QStringLiteral("Write a subject or a message for the reply."));
        QCOMPARE(f.g.vacationPuts, 0);
        // Dates the wrong way round: not sent either.
        dlg->findChild<QLineEdit *>(QStringLiteral("vacationSubject"))->setText(QStringLiteral("Out sailing"));
        dlg->findChild<QPlainTextEdit *>(QStringLiteral("vacationText"))->setPlainText(QStringLiteral("Back soon."));
        dlg->findChild<QCheckBox *>(QStringLiteral("vacationHasFirst"))->setChecked(true);
        dlg->findChild<QDateEdit *>(QStringLiteral("vacationFirst"))->setDate(QDate(2030, 6, 10));
        dlg->findChild<QCheckBox *>(QStringLiteral("vacationHasLast"))->setChecked(true);
        dlg->findChild<QDateEdit *>(QStringLiteral("vacationLast"))->setDate(QDate(2030, 6, 1));
        dlg->save();
        QCOMPARE(dlg->statusText(), QStringLiteral("The last day is before the first day."));
        QCOMPARE(f.g.vacationPuts, 0);

        // Right: the first save asks Google for the settings scope (the test's
        // "browser" agrees), keeps what was already granted, then writes.
        dlg->findChild<QDateEdit *>(QStringLiteral("vacationLast"))->setDate(QDate(2030, 6, 20));
        const int authBefore = f.g.count(QStringLiteral("GET /o/oauth2/v2/auth"));
        QPointer<VacationDialog> open(dlg);
        dlg->save();
        QTRY_COMPARE(f.g.vacationPuts, 1);
        QTRY_VERIFY(!open); // stored: the dialog closed
        QCOMPARE(f.g.count(QStringLiteral("GET /o/oauth2/v2/auth")), authBefore + 1);
        QVERIFY(f.session->auth()->lastAuthUrl().query().contains(QStringLiteral("include_granted_scopes=true")));
        QVERIFY(f.session->auth()->hasScopes(AuthManager::settingsScopes()));
        QVERIFY(f.g.grantedScope.contains(QStringLiteral("gmail.modify")));
        QCOMPARE(f.g.vacation.value(QStringLiteral("enableAutoReply")).toBool(), true);
        QCOMPARE(f.g.vacation.value(QStringLiteral("responseSubject")).toString(), QStringLiteral("Out sailing"));
        QCOMPARE(f.g.vacation.value(QStringLiteral("responseBodyPlainText")).toString(), QStringLiteral("Back soon."));
        QCOMPARE(VacationSettings::fromJson(f.g.vacation).firstDay, QDate(2030, 6, 10));
        QCOMPARE(VacationSettings::fromJson(f.g.vacation).lastDay, QDate(2030, 6, 20));
        // Still signed in, with the mail on show, and the status bar says it is on.
        QCOMPARE(f.session->state(), MailSession::State::SignedIn);
        QTRY_VERIFY(f.w->isLive());
        QTRY_VERIFY(label->isVisible());
        QVERIFY2(label->text().contains(QStringLiteral("Starts June 10, until June 20")), qPrintable(label->text()));

        // Turning it off again needs no second trip to the browser.
        QTRY_VERIFY(f.session->sync() && !f.session->sync()->isBusy());
        dlg = f.w->showVacationDialog();
        QVERIFY(dlg);
        QTRY_VERIFY(!dlg->saving());
        QVERIFY(dlg->settings().enabled);
        dlg->findChild<QCheckBox *>(QStringLiteral("vacationEnable"))->setChecked(false);
        dlg->save();
        QTRY_COMPARE(f.g.vacationPuts, 2);
        QCOMPARE(f.g.count(QStringLiteral("GET /o/oauth2/v2/auth")), authBefore + 1);
        QCOMPARE(f.g.vacation.value(QStringLiteral("enableAutoReply")).toBool(), false);
        QTRY_VERIFY(!label->isVisible());
    }

    // Saying no on Google's page (or leaving its box unticked) changes
    // nothing: zmail stays signed in, and the dialog says what happened.
    void refusedSettingsAccessLeavesTheAccountSignedIn()
    {
        Fixture f;
        QVERIFY(f.start());
        f.seed({}, QStringLiteral("Mail"));
        QVERIFY(f.open());
        const QStringList before = f.session->auth()->grantedScopes();
        f.g.grantGmailScope = false; // the consent comes back without what was asked
        VacationDialog *dlg = f.w->showVacationDialog();
        QVERIFY(dlg);
        QTRY_VERIFY(!dlg->saving());
        dlg->findChild<QCheckBox *>(QStringLiteral("vacationEnable"))->setChecked(true);
        dlg->findChild<QLineEdit *>(QStringLiteral("vacationSubject"))->setText(QStringLiteral("Away"));
        dlg->save();
        QVERIFY(dlg->saving());
        QTRY_VERIFY(!dlg->saving());
        QVERIFY2(dlg->statusText().startsWith(QStringLiteral("Gmail didn't save it: ")), qPrintable(dlg->statusText()));
        QVERIFY(dlg->isVisible()); // what was typed is still there to try again
        QCOMPARE(dlg->settings().subject, QStringLiteral("Away"));
        QCOMPARE(f.g.vacationPuts, 0);
        QCOMPARE(f.session->state(), MailSession::State::SignedIn);
        QVERIFY(f.w->isLive());
        QCOMPARE(f.session->auth()->grantedScopes(), before);
        QVERIFY(f.session->sync());
        dlg->close();
    }

    // ---- Sent swoosh ----------------------------------------------------------

    void sentMailPlaysASwoosh()
    {
        QVERIFY(QFile::exists(SentSound::resourcePath()));
        QCOMPARE(NewMailSound::outputVolume(), 0.0f); // a test run makes no noise
        Fixture f;
        QVERIFY(f.start());
        f.seed({}, QStringLiteral("Mail"));
        QVERIFY(f.open());
        SentSound *sound = f.w->sentSound();
        QVERIFY(sound->isEnabled()); // on unless turned off
        QCOMPARE(sound->playCount(), 0);

        ComposeWindow *c = f.w->openCompose();
        emit c->sent(QStringLiteral("m1"), QStringLiteral("t1"));
        QCOMPARE(sound->playCount(), 1);
        delete c; // (closing would ask about saving a draft)

        // Settings > Sounds: its own box, and a Test that plays either way.
        SoundDialog *dlg = f.w->showSoundDialog();
        auto *box = dlg->findChild<QCheckBox *>(QStringLiteral("sentSoundCheck"));
        QVERIFY(box && box->isChecked());
        box->setChecked(false);
        dlg->findChild<QPushButton *>(QStringLiteral("testSentSoundButton"))->click();
        QCOMPARE(sound->playCount(), 2);
        QVERIFY(sound->isEnabled()); // nothing is saved until OK
        dlg->save();
        QVERIFY(!sound->isEnabled());
        QCOMPARE(QSettings().value(QLatin1String(SentSound::kEnabledKey)).toBool(), false);
        delete dlg;

        c = f.w->openCompose();
        emit c->sent(QStringLiteral("m1"), QStringLiteral("t1"));
        QCOMPARE(sound->playCount(), 2); // silent now
        delete c; // (closing would ask about saving a draft)
        QVERIFY(!SentSound().isEnabled()); // and remembered
    }
};

QTEST_MAIN(TstExtras)
#include "tst_extras.moc"
