// 1.1.2: screen readers get a name (and, where useful, a description) for the
// main widgets of the main window, the message view and the compose window.
#include "MainWindow.hpp"
#include "ui/ComposeWindow.h"
#include "ui/MessageView.h"
#include "ui/SafeHtmlView.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QtTest>

using namespace zmail::ui;

namespace {
QString nameOf(QWidget *w)
{
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
    return iface ? iface->text(QAccessible::Name) : QString();
}

// Every text field, combo box and item view in `root` needs an accessible name,
// unless it lives inside another widget's internals (a combo's line edit, etc.).
QStringList unnamedInputs(QWidget *root)
{
    QStringList missing;
    const auto all = root->findChildren<QWidget *>();
    for (QWidget *w : all) {
        const bool input = qobject_cast<QLineEdit *>(w) || qobject_cast<QComboBox *>(w)
                           || qobject_cast<QAbstractItemView *>(w) || qobject_cast<QTextEdit *>(w)
                           || qobject_cast<QPlainTextEdit *>(w);
        if (!input || w->objectName().isEmpty() || w->objectName().startsWith(QLatin1String("qt_"))) {
            continue; // anonymous helpers (popups, completers) are named by their owner
        }
        if (qobject_cast<QComboBox *>(w->parentWidget())) {
            continue;
        }
        if (w->accessibleName().isEmpty()) {
            missing << w->objectName();
        }
    }
    return missing;
}
} // namespace

class TstAccessibility : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { applyTheme(ThemeMode::Light); }

    void mainWindowWidgetsAreNamed()
    {
        MainWindow w;
        w.show();
        const QList<QPair<const char *, QString>> expected = {
            {"searchBox", QStringLiteral("Search mail")},
            {"mailboxTree", QStringLiteral("Mailboxes")},
            {"messageList", QStringLiteral("Message list")},
            {"messageView", QStringLiteral("Message")},
            {"previewPane", QStringLiteral("Message body")},
            {"syncLabel", QStringLiteral("Sync status")},
            {"countLabel", QStringLiteral("Message count")},
        };
        for (const auto &[obj, name] : expected) {
            auto *wid = w.findChild<QWidget *>(QString::fromLatin1(obj));
            QVERIFY2(wid, obj);
            QCOMPARE(wid->accessibleName(), name);
            QCOMPARE(nameOf(wid), name); // what AT-SPI actually reports
        }
        for (const char *obj : {"searchBox", "mailboxTree", "messageList", "previewPane"}) {
            QVERIFY2(!w.findChild<QWidget *>(QString::fromLatin1(obj))->accessibleDescription().isEmpty(), obj);
        }
        QCOMPARE(unnamedInputs(&w), QStringList());
    }

    void composeFieldsAreNamed()
    {
        ComposeWindow c;
        c.show();
        const QList<QPair<const char *, QString>> expected = {
            {"fieldTo", QStringLiteral("To")},
            {"fieldSubject", QStringLiteral("Subject")},
            {"fieldCc", QStringLiteral("Cc")},
            {"fieldBcc", QStringLiteral("Bcc")},
            {"composeBody", QStringLiteral("Message body")},
        };
        for (const auto &[obj, name] : expected) {
            auto *wid = c.findChild<QWidget *>(QString::fromLatin1(obj));
            QVERIFY2(wid, obj);
            QCOMPARE(wid->accessibleName(), name);
        }
        QVERIFY(!c.findChild<QWidget *>(QStringLiteral("fieldTo"))->accessibleDescription().isEmpty());
        QCOMPARE(unnamedInputs(&c), QStringList());
    }

    void messageViewControlsAreNamed()
    {
        MessageView v;
        QCOMPARE(v.body()->accessibleName(), QString()); // the owner (MainWindow / MessageWindow) names the pane
        for (QPushButton *b : v.findChildren<QPushButton *>()) {
            if (b->text().isEmpty()) {
                QVERIFY2(!b->accessibleName().isEmpty(), qPrintable(b->objectName()));
            }
        }
        QStringList names;
        for (QWidget *w : v.findChildren<QWidget *>()) {
            names << w->accessibleName();
        }
        for (const char *n : {"Subject", "Attachments", "Load remote images", "Unsubscribe from this list"}) {
            QVERIFY2(names.contains(QString::fromLatin1(n)), n);
        }
    }
};

QTEST_MAIN(TstAccessibility)
#include "tst_accessibility.moc"
