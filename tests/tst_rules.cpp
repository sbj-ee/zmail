// Filters (core/Rules, ui/RulesDialog, and what MainWindow does with them):
// matching, first match wins, the JSON file, the editor, row colours, and
// the actions taken when mail arrives or on Message > Filter Messages.
// All mail here is fake (MockGoogle, example.com addresses).
#include "MainWindow.hpp"
#include "core/MailCache.h"
#include "core/MailSession.h"
#include "core/Rules.h"
#include "core/SyncEngine.h"
#include "core/TokenStore.h"
#include "mock/MockGoogle.h"
#include "ui/MessageListModel.h"
#include "ui/NewMailSound.h"
#include "ui/RulesDialog.h"
#include "ui/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>
#include <memory>

using namespace zmail;
using namespace zmail::ui;
using zmail::test::MockGoogle;

namespace {
QNetworkAccessManager *browserNam()
{
    static QNetworkAccessManager nam;
    return &nam;
}

Rule rule(const QString &name, const QString &field, const QString &op, const QString &value)
{
    Rule r;
    r.name = name;
    r.conditions.append({field, op, value});
    return r;
}

// A signed-in main window on the mock server.
struct Live
{
    MockGoogle g;
    MemoryTokenStore store;
    std::unique_ptr<MailSession> session;
    std::unique_ptr<MainWindow> w;
    MessageListModel *model = nullptr;

    QString seed(const QString &from, const QString &subject, const QStringList &labels, int minutesAgo = 1)
    {
        MockGoogle::Message m;
        m.from = from;
        m.to = QStringLiteral("Demo User <demo.user@example.com>");
        m.subject = subject;
        m.text = QStringLiteral("Fake test mail.");
        m.labels = labels;
        m.date = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
        return g.addMessage(m, true);
    }
    bool open()
    {
        SessionOptions o;
        o.client.status = ClientConfig::LoadStatus::Ok;
        o.client.config = g.clientConfig();
        o.store = &store;
        o.apiBase = g.apiBase();
        o.revokeUri = g.revokeUri();
        o.cachePathOverride = QStringLiteral(":memory:");
        o.contactsPathOverride = QStringLiteral(":memory:");
        o.rememberAccount = false;
        o.pollIntervalMs = 3600 * 1000;
        o.backoffBaseMs = 5;
        o.browserOpener = [](const QUrl &u) {
            QNetworkReply *r = browserNam()->get(QNetworkRequest(u));
            QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
            return true;
        };
        session = std::make_unique<MailSession>(o);
        session->signIn();
        if (!QTest::qWaitFor([this] { return session->state() == MailSession::State::SignedIn; }, 10000) ||
            !QTest::qWaitFor([this] { return session->sync() && !session->sync()->isBusy(); }, 15000)) {
            return false;
        }
        w = std::make_unique<MainWindow>();
        w->setSession(session.get());
        QMetaObject::invokeMethod(session.get(), "ready");
        w->show();
        model = w->findChild<MessageListModel *>();
        return QTest::qWaitFor([this] { return w->isLive(); }, 5000);
    }
    MailItem item(const QString &id) const { return model->item(model->rowForId(id)); }
    QStringList labels(const QString &id) const { return g.messages().value(id).labels; }
};
} // namespace

class TstRules : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        applyTheme(ThemeMode::Light);
    }
    void init() { QFile::remove(Rules::defaultPath()); }
    void cleanupTestCase() { QFile::remove(Rules::defaultPath()); }

    void conditionsMatchHeaders()
    {
        const RuleMessage m{QStringLiteral("Priya Raman <priya.raman@example.com>"),
                            QStringLiteral("Demo User <demo.user@example.com>, team@example.org"),
                            QStringLiteral("Q4 Budget review")};
        const auto hit = [&m](const char *field, const char *op, const char *value) {
            return Rules::matches(RuleCondition{QString::fromLatin1(field), QString::fromLatin1(op), QString::fromLatin1(value)}, m);
        };
        QVERIFY(hit("from", "contains", "PRIYA"));          // case doesn't matter
        QVERIFY(hit("from", "contains", "@example.com"));
        QVERIFY(!hit("from", "contains", "example.org"));
        QVERIFY(hit("to", "contains", "team@example.org"));
        QVERIFY(hit("subject", "is", "q4 budget review"));
        QVERIFY(!hit("subject", "is", "budget"));
        QVERIFY(hit("subject", "startsWith", "Q4"));
        QVERIFY(hit("subject", "endsWith", "review"));
        QVERIFY(hit("subject", "notContains", "invoice"));
        QVERIFY(!hit("subject", "notContains", "budget"));
        QVERIFY(hit("any", "contains", "team@"));            // found in To
        QVERIFY(hit("any", "notContains", "newsletter"));    // in none of the three
        QVERIFY(!hit("any", "notContains", "budget"));
        QVERIFY(hit("subject", "regex", "^q\\d+\\s"));
        QVERIFY(!hit("subject", "regex", "(unclosed"));      // a broken pattern matches nothing
        QVERIFY(hit("nonsense", "contains", "priya"));       // an unknown field reads as From
    }

    void allAnyAndFirstMatchWins()
    {
        const RuleMessage m{QStringLiteral("Billing <billing@isp.example.com>"), QStringLiteral("demo.user@example.com"),
                            QStringLiteral("Your invoice")};
        Rule both = rule(QStringLiteral("both"), QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("isp.example.com"));
        both.conditions.append({QStringLiteral("subject"), QStringLiteral("contains"), QStringLiteral("receipt")});
        QVERIFY(!Rules::matches(both, m)); // all: the subject doesn't
        both.matchAny = true;
        QVERIFY(Rules::matches(both, m)); // any: the sender does
        QVERIFY(Rules::matches(Rule(), m)); // no conditions: everything

        Rules rs;
        rs.rules = {rule(QStringLiteral("off"), QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("billing")),
                    rule(QStringLiteral("invoices"), QStringLiteral("subject"), QStringLiteral("contains"), QStringLiteral("invoice")),
                    rule(QStringLiteral("isp"), QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("isp")),
                    Rule()};
        rs.rules[0].enabled = false;
        rs.rules[3].name = QStringLiteral("everything else");
        QCOMPARE(rs.match(m)->name, QStringLiteral("invoices")); // the disabled one is passed over; the later one never runs
        const RuleMessage other{QStringLiteral("a@example.com"), {}, QStringLiteral("hello")};
        QCOMPARE(rs.match(other)->name, QStringLiteral("everything else"));
        rs.rules.removeLast();
        QVERIFY(!rs.match(other));
    }

    void savesAndLoadsJson()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sub/rules.json"));
        Rules rs;
        QVERIFY(rs.load(path)); // no file: no rules
        QVERIFY(rs.rules.isEmpty());
        Rule r = rule(QStringLiteral("Family \"home\""), QStringLiteral("from"), QStringLiteral("endsWith"), QStringLiteral("@example.net>"));
        r.matchAny = true;
        r.conditions.append({QStringLiteral("subject"), QStringLiteral("regex"), QStringLiteral("^re:")});
        r.color = QStringLiteral("#188038");
        r.flag = QStringLiteral("green");
        r.sound = QStringLiteral("/tmp/thunk.wav");
        r.moveTo = QStringLiteral("Label_7");
        r.markRead = true;
        Rule quiet = rule(QStringLiteral("Quiet"), QStringLiteral("any"), QStringLiteral("contains"), QStringLiteral("newsletter"));
        quiet.sound = QString::fromLatin1(kRuleSoundNone);
        quiet.enabled = false;
        rs.rules = {r, quiet};
        QVERIFY(rs.save(path));

        Rules back;
        QVERIFY(back.load(path));
        QCOMPARE(back.rules, rs.rules);

        // Hand-edited: unknown words fall back, missing parts get defaults.
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(R"({"rules":[{"name":"x","conditions":[{"field":"body","op":"sounds like","value":"v"}]}]})");
        f.close();
        QVERIFY(back.load(path));
        QCOMPARE(back.rules.size(), 1);
        QVERIFY(back.rules.first().enabled);
        QCOMPARE(back.rules.first().conditions.first(),
                 (RuleCondition{QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("v")}));
        // Not JSON: refused, and the rules in memory are kept.
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{ nope");
        f.close();
        QVERIFY(!back.load(path));
        QCOMPARE(back.rules.size(), 1);
    }

    void editorBuildsAndReordersRules()
    {
        const QList<RulesDialog::Folder> folders{{QStringLiteral("Label_7"), QStringLiteral("Projects")},
                                                 {QStringLiteral("Label_8"), QStringLiteral("Receipts")}};
        RulesDialog d({}, folders);
        auto *list = d.findChild<QListWidget *>(QStringLiteral("ruleList"));
        auto *editor = d.findChild<QWidget *>(QStringLiteral("ruleEditor"));
        QVERIFY(!editor->isEnabled()); // nothing to edit yet
        QVERIFY(!d.findChild<QPushButton *>(QStringLiteral("ruleDelete"))->isEnabled());

        // New: a named rule with one empty condition, ready to fill in.
        d.findChild<QPushButton *>(QStringLiteral("ruleNew"))->click();
        QCOMPARE(d.rules().size(), 1);
        QVERIFY(editor->isEnabled());
        d.findChild<QLineEdit *>(QStringLiteral("ruleName"))->setText(QStringLiteral("Boss"));
        QCOMPARE(list->item(0)->text(), QStringLiteral("Boss"));
        auto *field = d.findChild<QComboBox *>(QStringLiteral("condField_0"));
        auto *op = d.findChild<QComboBox *>(QStringLiteral("condOp_0"));
        field->setCurrentIndex(field->findData(QStringLiteral("subject")));
        op->setCurrentIndex(op->findData(QStringLiteral("startsWith")));
        d.findChild<QLineEdit *>(QStringLiteral("condValue_0"))->setText(QStringLiteral("URGENT"));
        d.addCondition();
        d.findChild<QLineEdit *>(QStringLiteral("condValue_1"))->setText(QStringLiteral("boss@example.com"));
        auto *match = d.findChild<QComboBox *>(QStringLiteral("ruleMatch"));
        match->setCurrentIndex(1);
        auto *color = d.findChild<QComboBox *>(QStringLiteral("ruleColor"));
        color->setCurrentIndex(color->findText(QStringLiteral("Red")));
        auto *flag = d.findChild<QComboBox *>(QStringLiteral("ruleFlag"));
        flag->setCurrentIndex(flag->findData(QStringLiteral("orange")));
        auto *move = d.findChild<QComboBox *>(QStringLiteral("ruleMoveTo"));
        QCOMPARE(move->count(), 3);
        move->setCurrentIndex(move->findData(QStringLiteral("Label_8")));
        d.findChild<QCheckBox *>(QStringLiteral("ruleMarkRead"))->setChecked(true);
        auto *sound = d.findChild<QComboBox *>(QStringLiteral("ruleSound"));
        sound->setCurrentIndex(sound->findData(QString::fromLatin1(kRuleSoundNone)));
        emit sound->activated(sound->currentIndex());

        Rule boss = d.rules().first();
        QCOMPARE(boss.name, QStringLiteral("Boss"));
        QVERIFY(boss.matchAny);
        QCOMPARE(boss.conditions, (QList<RuleCondition>{{QStringLiteral("subject"), QStringLiteral("startsWith"), QStringLiteral("URGENT")},
                                                         {QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("boss@example.com")}}));
        QCOMPARE(QColor(boss.color), QColor(0xd9, 0x30, 0x25));
        QCOMPARE(boss.flag, QStringLiteral("orange"));
        QCOMPARE(boss.moveTo, QStringLiteral("Label_8"));
        QVERIFY(boss.markRead);
        QCOMPARE(boss.sound, QString::fromLatin1(kRuleSoundNone));
        QVERIFY(!list->item(0)->icon().isNull()); // its colour shows in the list

        // Take a condition out; none left means "every message".
        d.removeCondition(0);
        QCOMPARE(d.rules().first().conditions.size(), 1);
        QCOMPARE(d.findChild<QLineEdit *>(QStringLiteral("condValue_0"))->text(), QStringLiteral("boss@example.com"));
        d.removeCondition(0);
        QVERIFY(d.findChild<QWidget *>(QStringLiteral("ruleNoConditions")));

        // A second rule; order is what decides, so it can be moved.
        Rule news = rule(QStringLiteral("News"), QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("news@"));
        QCOMPARE(d.addRule(news), 1);
        QCOMPARE(d.findChild<QLineEdit *>(QStringLiteral("ruleName"))->text(), QStringLiteral("News"));
        QVERIFY(!d.findChild<QPushButton *>(QStringLiteral("ruleDown"))->isEnabled());
        d.findChild<QPushButton *>(QStringLiteral("ruleUp"))->click();
        QCOMPARE(d.rules().first().name, QStringLiteral("News"));
        QCOMPARE(d.currentRule(), 0);
        d.moveCurrentRule(1);
        QCOMPARE(d.rules().last().name, QStringLiteral("News"));
        // Unticking switches a rule off without losing it.
        list->item(0)->setCheckState(Qt::Unchecked);
        QVERIFY(!d.rules().first().enabled);
        // Editing one rule doesn't leak into the other.
        d.setCurrentRule(0);
        QCOMPARE(d.findChild<QLineEdit *>(QStringLiteral("ruleName"))->text(), QStringLiteral("Boss"));
        QCOMPARE(d.rules().last(), news);
        d.setCurrentRule(1);
        d.removeCurrentRule();
        QCOMPARE(d.rules().size(), 1);
        QCOMPARE(d.currentRule(), 0);
    }

    void filtersColourRowsAndActOnArrival()
    {
        Live f;
        f.g.listen();
        f.g.seedSystemLabels();
        f.g.addLabel({QStringLiteral("Label_7"), QStringLiteral("Receipts"), QStringLiteral("user"), {}});
        const QString old = f.seed(QStringLiteral("Billing <billing@isp.example.com>"), QStringLiteral("Your invoice"),
                                   {QStringLiteral("INBOX")}, 30);
        const QString plain = f.seed(QStringLiteral("Ruth <ruth@example.net>"), QStringLiteral("Dinner?"), {QStringLiteral("INBOX")}, 20);
        QVERIFY(f.open());
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionFilters")));
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionFilterMessages")));
        QTRY_VERIFY(f.model->rowForId(old) >= 0);
        QVERIFY(!f.item(old).ruleColor.isValid());

        // Through the editor: OK saves the rules and recolours the list.
        RulesDialog *dlg = f.w->showRulesDialog();
        Rule bills = rule(QStringLiteral("Bills"), QStringLiteral("from"), QStringLiteral("contains"), QStringLiteral("billing@"));
        bills.color = QStringLiteral("#1a73e8");
        bills.flag = QStringLiteral("blue");
        bills.moveTo = QStringLiteral("Label_7");
        bills.markRead = true;
        bills.sound = QString::fromLatin1(kRuleSoundNone);
        dlg->addRule(bills);
        QVERIFY(dlg->findChild<QComboBox *>(QStringLiteral("ruleMoveTo"))->findData(QStringLiteral("Label_7")) > 0);
        dlg->accept();
        QCOMPARE(f.w->rules().rules.size(), 1);
        QVERIFY(QFile::exists(Rules::defaultPath()));
        QTRY_COMPARE(f.item(old).ruleColor, QColor(0x1a, 0x73, 0xe8)); // every matching message, old ones too
        QVERIFY(!f.item(plain).ruleColor.isValid());
        // ...but nothing has been done to mail that was already here.
        QVERIFY(f.labels(old).contains(QStringLiteral("INBOX")));
        QVERIFY(!f.labels(old).contains(QStringLiteral("STARRED")));

        // New mail: the matching one is flagged, read, moved, and silent;
        // the other is untouched and gets the usual sound.
        const int plays = f.w->newMailSound()->playCount();
        const QString bill = f.seed(QStringLiteral("Billing <billing@isp.example.com>"), QStringLiteral("October invoice"),
                                    {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 0);
        f.session->sync()->pollNow(true);
        QTRY_VERIFY_WITH_TIMEOUT(f.labels(bill).contains(QStringLiteral("Label_7")), 10000);
        QTRY_VERIFY(!f.labels(bill).contains(QStringLiteral("INBOX")));
        QTRY_VERIFY(!f.labels(bill).contains(QStringLiteral("UNREAD")));
        QTRY_VERIFY(f.labels(bill).contains(QStringLiteral("STARRED")));
        QCOMPARE(f.session->cache()->flags().value(bill), QStringLiteral("blue"));
        QCOMPARE(f.w->newMailSound()->playCount(), plays); // "No sound"
        const QString hello = f.seed(QStringLiteral("Ruth <ruth@example.net>"), QStringLiteral("Hello again"),
                                     {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 0);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(f.w->newMailSound()->playCount(), plays + 1, 10000);
        QVERIFY(f.labels(hello).contains(QStringLiteral("INBOX")));
        QVERIFY(f.labels(hello).contains(QStringLiteral("UNREAD")));

        // A desktop notification for mail that arrives while zmail isn't in
        // front (never for mail a filter said to keep quiet about), and the
        // Inbox's unread count in the window title.
        QStringList notes;
        f.w->setNotifySink([&notes](const QString &summary, const QString &body) { notes << summary + QStringLiteral(" | ") + body; });
        QVERIFY(f.w->notifyOn());
        QVERIFY(f.w->findChild<QAction *>(QStringLiteral("actionNotify"))->isChecked());
        f.w->hide(); // not the active window (offscreen windows are active while shown)
        QTRY_VERIFY(!QApplication::activeWindow());
        f.seed(QStringLiteral("Ruth <ruth@example.net>"), QStringLiteral("Third"), {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 0);
        f.session->sync()->pollNow(true);
        QTRY_COMPARE_WITH_TIMEOUT(notes.size(), 1, 10000);
        QCOMPARE(notes.first(), QStringLiteral("1 new message | Ruth: Third"));
        QTRY_VERIFY2(f.w->windowTitle().startsWith(QStringLiteral("(2) zmail ")), qPrintable(f.w->windowTitle()));
        f.seed(QStringLiteral("Billing <billing@isp.example.com>"), QStringLiteral("Quiet invoice"),
               {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 0);
        f.session->sync()->pollNow(true);
        QTest::qWait(600);
        QCOMPARE(notes.size(), 1); // the filter's "No sound" covers the notification too
        f.w->setNotifyOn(false);
        QVERIFY(!QSettings().value(QStringLiteral("notify/desktop")).toBool());
        f.seed(QStringLiteral("Ruth <ruth@example.net>"), QStringLiteral("Fourth"), {QStringLiteral("INBOX"), QStringLiteral("UNREAD")}, 0);
        f.session->sync()->pollNow(true);
        QTest::qWait(600);
        QCOMPARE(notes.size(), 1); // switched off
        f.w->setNotifyOn(true);
        f.w->show();

        // Message > Filter Messages does it for mail that was already here.
        auto *list = f.w->findChild<QTreeView *>(QStringLiteral("messageList"));
        QTRY_VERIFY(f.model->rowForId(old) >= 0);
        list->selectAll();
        f.w->findChild<QAction *>(QStringLiteral("actionFilterMessages"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(f.labels(old).contains(QStringLiteral("Label_7")), 10000);
        QTRY_VERIFY(f.labels(old).contains(QStringLiteral("STARRED")));
        QVERIFY(f.labels(plain).contains(QStringLiteral("INBOX"))); // no rule for it

        // A restart keeps the filters; Cancel in the editor changes nothing.
        MainWindow again;
        QCOMPARE(again.rules().rules, f.w->rules().rules);
        dlg = f.w->showRulesDialog();
        dlg->removeCurrentRule();
        dlg->reject();
        QCOMPARE(f.w->rules().rules.size(), 1);
        f.w->setRules({});
        QTRY_VERIFY(!f.item(plain).ruleColor.isValid());
    }
};

QTEST_MAIN(TstRules)
#include "tst_rules.moc"
