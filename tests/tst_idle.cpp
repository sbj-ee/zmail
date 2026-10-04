// An idle zmail must stay idle: no re-render, repaint or relayout loop once a
// window is up, a mailbox has synced and a (long, table-heavy) message is open.
// 0.3.1 re-rendered the preview ~10x a second forever (scroll bar feedback).
#include "MainWindow.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"

#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTest>
#include <QTreeView>

#include <ctime>

using namespace zmail;
using zmail::test::MockGoogle;

namespace {

QString fixture(const char *name)
{
    QFile f(QStringLiteral(ZMAIL_FIXTURES "/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", name);
    }
    return QString::fromUtf8(f.readAll());
}

double threadCpuSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

// Counts every event the GUI thread delivers, by type.
class EventCounter : public QObject
{
public:
    explicit EventCounter() { qApp->installEventFilter(this); }
    ~EventCounter() override { qApp->removeEventFilter(this); }
    int total = 0;
    QHash<int, int> byType;
    bool eventFilter(QObject *, QEvent *e) override
    {
        ++total;
        ++byType[int(e->type())];
        return false;
    }
    QString summary() const
    {
        QStringList s;
        for (auto it = byType.begin(); it != byType.end(); ++it) {
            s << QStringLiteral("%1:%2").arg(it.key()).arg(it.value());
        }
        return s.join(QLatin1Char(' '));
    }
};

struct Idle {
    int events = 0;
    int paints = 0;
    int renders = 0;
    double cpuPercent = 0;
    QString summary;
};

Idle measureIdle(ui::MessageView *view, int ms)
{
    const int renders0 = view->renderCount();
    const double cpu0 = threadCpuSeconds();
    EventCounter c;
    QTest::qWait(ms);
    Idle r;
    r.cpuPercent = 100.0 * (threadCpuSeconds() - cpu0) / (ms / 1000.0);
    r.events = c.total;
    r.paints = c.byType.value(int(QEvent::Paint));
    r.renders = view->renderCount() - renders0;
    r.summary = c.summary();
    return r;
}

SessionOptions mockOptions(MockGoogle &g, MemoryTokenStore *store)
{
    SessionOptions o;
    o.client.status = ClientConfig::LoadStatus::Ok;
    o.client.config = g.clientConfig();
    o.store = store;
    o.apiBase = g.apiBase();
    o.revokeUri = g.revokeUri();
    o.cachePathOverride = QStringLiteral(":memory:");
    o.rememberAccount = false;
    o.pollIntervalMs = 3600 * 1000;
    o.backoffBaseMs = 5;
    o.browserOpener = [](const QUrl &u) {
        static QNetworkAccessManager nam;
        QNetworkReply *r = nam.get(QNetworkRequest(u));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
        return true;
    };
    return o;
}

} // namespace

class TstIdle : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void openMessageDoesNotKeepReRendering_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("width");
        for (const char *f : {"brokerage-confirm.html", "retail-rx.html", "meetup-concat.html", "unlayer-vet.html"}) {
            for (int w : {360, 900}) {
                QTest::addRow("%s@%d", f, w) << QString::fromLatin1(f) << w;
            }
        }
    }

    void openMessageDoesNotKeepReRendering()
    {
        QFETCH(QString, name);
        QFETCH(int, width);
        ui::MessageView v;
        v.resize(width, 500); // short pane: the body needs a scroll bar
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        ui::ViewMessage m;
        m.id = name;
        m.subject = name;
        m.bodyHtml = fixture(name.toLatin1().constData());
        v.setMessage(m);
        QTest::qWait(400); // let the first relayout (if any) settle
        const Idle idle = measureIdle(&v, 800);
        QVERIFY2(idle.renders == 0, qPrintable(QStringLiteral("%1 renders while idle").arg(idle.renders)));
        QVERIFY2(idle.paints <= 2, qPrintable(idle.summary));

        // A real resize still relayouts once, then goes quiet again.
        const int before = v.renderCount();
        v.resize(width + 120, 500);
        QTRY_VERIFY(v.renderCount() > before);
        QTest::qWait(400);
        QCOMPARE(measureIdle(&v, 600).renders, 0);
    }

    void idleWindowAfterSyncStaysIdle()
    {
        MockGoogle g;
        QVERIFY(g.listen());
        g.seedDemo(10);
        MockGoogle::Message big;
        big.from = QStringLiteral("Example Brokerage <confirms@brokerage.example>");
        big.subject = QStringLiteral("Trade confirmation");
        big.text = QStringLiteral("Your trade confirmation.");
        big.html = fixture("brokerage-confirm.html");
        big.labels = {QStringLiteral("INBOX")};
        big.date = QDateTime::currentDateTime().addSecs(60); // newest: row 0
        g.addMessage(big);

        MemoryTokenStore store;
        MailSession session(mockOptions(g, &store));
        MainWindow w;
        w.resize(1280, 800);
        w.setSession(&session);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        session.signIn();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), MailSession::State::SignedIn, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(session.cache()->count(QStringLiteral("INBOX")) > 10 && !session.sync()->isBusy(), 20000);

        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_COMPARE(list->model()->rowCount(), session.cache()->count(QStringLiteral("INBOX")));
        list->setFocus();
        QModelIndex target;
        for (int r = 0; r < list->model()->rowCount() && !target.isValid(); ++r) {
            for (int c = 0; c < list->model()->columnCount(); ++c) {
                const QModelIndex i = list->model()->index(r, c);
                if (i.data().toString().contains(QStringLiteral("Trade confirmation"))) {
                    target = list->model()->index(r, 0);
                    break;
                }
            }
        }
        QVERIFY(target.isValid());
        list->setCurrentIndex(target);
        auto *view = w.findChild<ui::MessageView *>();
        QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(view->body()->toPlainText().contains(QStringLiteral("Account ending in")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.sync()->isBusy(), 10000);
        QTest::qWait(1000);

        const Idle idle = measureIdle(view, 2000);
        qInfo("idle 2 s: %d events, %d paints, %d renders, main thread CPU %.2f%%", idle.events, idle.paints,
              idle.renders, idle.cpuPercent);
        QVERIFY2(idle.renders == 0, qPrintable(idle.summary));
        QVERIFY2(idle.paints <= 4, qPrintable(idle.summary));
        QVERIFY2(idle.events <= 60, qPrintable(idle.summary));
        QVERIFY2(idle.cpuPercent < 2.0, qPrintable(QString::number(idle.cpuPercent)));
    }
};

QTEST_MAIN(TstIdle)
#include "tst_idle.moc"
