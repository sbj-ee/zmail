// Message list and theme fixes (0.3.3): search operators, the Size column,
// one size format everywhere, the dark-theme sidebar, selected-row contrast
// and hidden placeholders.
#include "MainWindow.hpp"
#include "core/SizeFormat.h"
#include "ui/ComposeWindow.h"
#include "ui/MessageListModel.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QtTest>

using namespace zmail::ui;

namespace {
MailItem item(const QString &who, const QString &subject, qint64 size = 4000)
{
    MailItem m;
    m.who = who;
    m.address = who.toLower().section(QLatin1Char(' '), 0, 0) + QStringLiteral("@example.com");
    m.subject = subject;
    m.sizeBytes = size;
    m.mailboxes = {QStringLiteral("In")};
    m.date = QDateTime(QDate(2026, 10, 4), QTime(9, 0));
    return m;
}

QStringList visibleSubjects(QAbstractItemModel *m)
{
    QStringList out;
    for (int r = 0; r < m->rowCount(); ++r) {
        out << m->index(r, MessageListModel::Subject).data().toString();
    }
    out.sort();
    return out;
}
} // namespace

class TstListFixes : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { applyTheme(ThemeMode::Light); }

    void sizeFormat_data()
    {
        QTest::addColumn<qint64>("bytes");
        QTest::addColumn<QString>("text");
        QTest::newRow("zero") << qint64(0) << QStringLiteral("1 KB");
        QTest::newRow("tiny") << qint64(12) << QStringLiteral("1 KB");
        QTest::newRow("3 KB") << qint64(2957) << QStringLiteral("3 KB");
        QTest::newRow("97 KB") << qint64(96'400) << QStringLiteral("97 KB");
        QTest::newRow("999 KB") << qint64(999'000) << QStringLiteral("999 KB");
        QTest::newRow("edge") << qint64(999'600) << QStringLiteral("1 MB");
        QTest::newRow("1.2 MB") << qint64(1'200'000) << QStringLiteral("1.2 MB");
        QTest::newRow("18.5 MB") << qint64(18'500'000) << QStringLiteral("18.5 MB");
        QTest::newRow("25 MB") << qint64(25'000'000) << QStringLiteral("25 MB");
        QTest::newRow("GB") << qint64(2'345'000'000) << QStringLiteral("2.3 GB");
    }
    void sizeFormat()
    {
        QFETCH(qint64, bytes);
        QFETCH(QString, text);
        QCOMPARE(zmail::formatSize(bytes), text);
        QCOMPARE(MessageListModel::formatSize(bytes), text);
    }

    void sizeColumnHeaderValuesTooltipAndSort()
    {
        MessageListModel model;
        model.setItems({item(QStringLiteral("Jordan Lee"), QStringLiteral("short"), 2957),
                        item(QStringLiteral("Riley Chen"), QStringLiteral("deck"), 1'250'000),
                        item(QStringLiteral("Sam Ortiz"), QStringLiteral("photos"), 99'000)});
        QCOMPARE(model.headerData(MessageListModel::Size, Qt::Horizontal, Qt::DisplayRole).toString(),
                 QStringLiteral("Size"));
        QCOMPARE(model.headerData(MessageListModel::Size, Qt::Horizontal, Qt::TextAlignmentRole).toInt(),
                 int(Qt::AlignRight | Qt::AlignVCenter));
        QVERIFY(model.headerData(MessageListModel::Size, Qt::Horizontal, Qt::ToolTipRole)
                    .toString().contains(QStringLiteral("Gmail's estimate")));
        const QModelIndex s0 = model.index(0, MessageListModel::Size);
        QCOMPARE(s0.data().toString(), QStringLiteral("3 KB"));
        QCOMPARE(s0.data(Qt::TextAlignmentRole).toInt(), int(Qt::AlignRight | Qt::AlignVCenter));
        QCOMPARE(s0.data(Qt::ToolTipRole).toString(), QStringLiteral("2,957 bytes (Gmail's estimate)"));
        QCOMPARE(model.index(1, MessageListModel::Size).data().toString(), QStringLiteral("1.3 MB"));
        QCOMPARE(model.index(2, MessageListModel::Size).data().toString(), QStringLiteral("99 KB"));

        // Sorting uses the real byte count, not the text ("99 KB" > "1.3 MB" as strings).
        MessageFilterProxy proxy;
        proxy.setSourceModel(&model);
        proxy.sort(MessageListModel::Size, Qt::DescendingOrder);
        QCOMPARE(proxy.index(0, MessageListModel::Subject).data().toString(), QStringLiteral("deck"));
        QCOMPARE(proxy.index(1, MessageListModel::Subject).data().toString(), QStringLiteral("photos"));
        QCOMPARE(proxy.index(2, MessageListModel::Subject).data().toString(), QStringLiteral("short"));
    }

    void sizeColumnIsWideEnoughAndStatusBarUsesUnits()
    {
        MainWindow w;
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        const int need = list->fontMetrics().horizontalAdvance(QStringLiteral("888.8 MB"));
        QVERIFY2(list->header()->sectionSize(MessageListModel::Size) > need,
                 qPrintable(QString::number(list->header()->sectionSize(MessageListModel::Size))));
        const QString counts = w.findChild<QLabel *>(QStringLiteral("countLabel"))->text();
        QVERIFY2(counts.contains(QRegularExpression(QStringLiteral("unread, [0-9.]+ (KB|MB|GB) "))), qPrintable(counts));
        QVERIFY2(!counts.contains(QStringLiteral(" K ")), qPrintable(counts));
    }

    void searchParsesOperators()
    {
        using T = SearchTerm;
        const auto terms = MessageFilterProxy::parseSearch(
            QStringLiteral("from:priya subject:\"crew schedule\" has:attachment is:unread -label:news 8:30 depot to:"));
        QCOMPARE(terms.size(), 7); // "to:" with no value is ignored while typing
        QCOMPARE(terms[0], (T{T::From, QStringLiteral("priya"), false}));
        QCOMPARE(terms[1], (T{T::Subject, QStringLiteral("crew schedule"), false}));
        QCOMPARE(terms[2].field, T::HasAttachment);
        QCOMPARE(terms[3].field, T::IsUnread);
        QCOMPARE(terms[4], (T{T::Label, QStringLiteral("news"), true}));
        QCOMPARE(terms[5], (T{T::Any, QStringLiteral("8:30"), false})); // unknown "op:" is plain text
        QCOMPARE(terms[6], (T{T::Any, QStringLiteral("depot"), false}));
        QVERIFY(MessageFilterProxy::parseSearch(QStringLiteral("   ")).isEmpty());
        QCOMPARE(MessageFilterProxy::parseSearch(QStringLiteral("FROM:Priya")).value(0).field, T::From);
    }

    void searchOperatorsFilterTheList_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QStringList>("subjects");
        QTest::newRow("from") << QStringLiteral("from:priya") << QStringList{"budget", "crew"};
        QTest::newRow("from address") << QStringLiteral("from:kenji@") << QStringList{"grill"};
        QTest::newRow("subject") << QStringLiteral("subject:budget") << QStringList{"budget"};
        QTest::newRow("subject phrase") << QStringLiteral("subject:\"crew sch\"") << QStringList{};
        QTest::newRow("has:attachment") << QStringLiteral("has:attachment") << QStringList{"budget", "grill"};
        QTest::newRow("is:unread") << QStringLiteral("is:unread") << QStringList{"budget"};
        QTest::newRow("is:read") << QStringLiteral("is:read") << QStringList{"crew", "grill"};
        QTest::newRow("label") << QStringLiteral("label:work") << QStringList{"budget"};
        QTest::newRow("to") << QStringLiteral("to:dana") << QStringList{"crew"};
        QTest::newRow("negate") << QStringLiteral("-from:priya") << QStringList{"grill"};
        QTest::newRow("combined") << QStringLiteral("from:priya has:attachment") << QStringList{"budget"};
        QTest::newRow("plain word") << QStringLiteral("depot") << QStringList{"crew"};
        QTest::newRow("no match") << QStringLiteral("zzzqqq") << QStringList{};
    }
    void searchOperatorsFilterTheList()
    {
        QFETCH(QString, query);
        QFETCH(QStringList, subjects);
        MailItem a = item(QStringLiteral("Priya Raman"), QStringLiteral("budget"));
        a.address = QStringLiteral("priya.raman@example.com");
        a.status = MailStatus::Unread;
        a.hasAttachment = true;
        a.label = QStringLiteral("Work");
        MailItem b = item(QStringLiteral("Priya Raman"), QStringLiteral("crew"));
        b.preview = QStringLiteral("Saturday 8am at the depot");
        b.to = QStringLiteral("Dana Whitfield <dana@example.org>");
        MailItem c = item(QStringLiteral("Kenji Watanabe"), QStringLiteral("grill"));
        c.address = QStringLiteral("kenji@example.net");
        c.hasAttachment = true;
        MessageListModel model;
        model.setItems({a, b, c});
        MessageFilterProxy proxy;
        proxy.setSourceModel(&model);
        proxy.setSearchText(query);
        subjects.sort();
        QCOMPARE(visibleSubjects(&proxy), subjects);
    }

    void searchBoxDrivesOperatorsInTheWindow()
    {
        MainWindow w; // sample data
        auto *search = w.findChild<QLineEdit *>(QStringLiteral("searchBox"));
        auto *list = w.findChild<QTreeView *>(QStringLiteral("messageList"));
        QVERIFY(search->toolTip().contains(QStringLiteral("has:attachment")));
        search->setText(QStringLiteral("from:priya"));
        QVERIFY(list->model()->rowCount() >= 1);
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            QCOMPARE(list->model()->index(r, MessageListModel::Who).data().toString(), QStringLiteral("Priya Raman"));
        }
        search->setText(QStringLiteral("has:attachment"));
        QVERIFY(list->model()->rowCount() >= 1);
        for (int r = 0; r < list->model()->rowCount(); ++r) {
            QVERIFY(list->model()->index(r, MessageListModel::Attachment).data(Qt::DecorationRole).isValid());
        }
        search->clear();
        QCOMPARE(list->model()->rowCount(), 11); // Hide Spam filters Contoso out of In
    }

    void selectedRowsMeetAaContrast()
    {
        for (const QPalette &p : {lightPalette(), darkPalette()}) {
            for (auto g : {QPalette::Active, QPalette::Inactive}) {
                const double r = contrastRatio(p.color(g, QPalette::HighlightedText), p.color(g, QPalette::Highlight));
                QVERIFY2(r >= 4.5, qPrintable(QString::number(r)));
            }
        }
    }

    void darkThemeRepaintsTheSidebar()
    {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *tree = w.findChild<QTreeWidget *>(QStringLiteral("mailboxTree"));
        auto bottomPixel = [tree]() {
            const QImage img = tree->viewport()->grab().toImage();
            return img.pixelColor(img.width() / 2, img.height() - 4);
        };
        QCOMPARE(bottomPixel(), lightPalette().color(QPalette::Base));
        w.setTheme(ThemeMode::Dark);
        QTest::qWait(50); // palette change events
        QCOMPARE(tree->palette().color(QPalette::Base), darkPalette().color(QPalette::Base));
        QCOMPARE(bottomPixel(), darkPalette().color(QPalette::Base));
        w.setTheme(ThemeMode::Light);
        QTest::qWait(50);
        QCOMPARE(bottomPixel(), lightPalette().color(QPalette::Base));
    }

    void placeholdersAndSendLaterAreHidden()
    {
        MainWindow w;
        // Not-yet-implemented menu items: hidden, not greyed out.
        int placeholders = 0;
        for (QMenu *m : w.menuBar()->findChildren<QMenu *>()) {
            for (QAction *a : m->actions()) {
                if (a->property("placeholder").toBool()) {
                    ++placeholders;
                    QVERIFY2(!a->isVisible(), qPrintable(a->text()));
                }
            }
        }
        QCOMPARE(placeholders, 5); // Mark as Suspicious became Mark as Junk; Snooze and Filters (Rules) are implemented
        for (const char *gone : {"&Save Attachments\u2026", "&Print\u2026", "&Copy", "View as &Plain Text",
                                 "&Account\u2026"}) {
            for (QAction *a : w.findChildren<QAction *>()) {
                if (a->text() == QString::fromUtf8(gone)) {
                    QVERIFY2(!a->isVisible(), gone);
                }
            }
        }
        ComposeWindow c;
        QAction *later = c.findChild<QAction *>(QStringLiteral("actionSendLater"));
        QVERIFY(later);
        QVERIFY(!later->isVisible());
    }
};

QTEST_MAIN(TstListFixes)
#include "tst_listfixes.moc"
