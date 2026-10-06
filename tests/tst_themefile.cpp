// Custom themes (*.ztheme.json, shared with zterminal): JSON round trip,
// validation, derivation of missing roles / terminal colours, cross-app
// files (tests/data/themes, identical in sbj-ee/zterminal), and the Theme Editor.
#include "MainWindow.hpp"
#include "ui/BrandThemes.h"
#include "ui/Theme.h"
#include "ui/ThemeEditorDialog.h"
#include "ui/ThemeFile.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using namespace zmail::ui;
using namespace sbj::theme;

namespace {
QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QString fixture(const char *name)
{
    return QStringLiteral(ZMAIL_TEST_DATA "/themes/") + QLatin1String(name);
}
void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}
// The theme zmail makes in tests/data/themes/zmail-made.ztheme.json: Dark
// duplicated, renamed, two roles, UI font and row stripes changed; zmail
// writes no terminal block.
Theme zmailMade()
{
    Theme t = *themeFileFor(QStringLiteral("dark"));
    t.name = QStringLiteral("Lakeside Dusk");
    t.basedOn = QStringLiteral("dark");
    t.roles[Accent] = 0x3FA796;
    t.roles[Selection] = 0x1F6F63;
    t.fonts.ui = QStringLiteral("DejaVu Sans");
    t.fonts.uiSize = 11;
    t.ui.rowStripes = 55;
    return t;
}
std::uint32_t rgbOf(const QColor &c)
{
    return c.rgb() & 0xFFFFFF;
}
} // namespace

class TstThemeFile : public QObject
{
    Q_OBJECT
    QTemporaryDir m_config;

private slots:
    void initTestCase()
    {
        QVERIFY(m_config.isValid());
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_config.path()));
        applyTheme(ThemeMode::Light);
        if (qEnvironmentVariableIsSet("ZTHEME_WRITE_FIXTURES")) { // maintainers: regenerate the fixture of this app
            writeFile(fixture("zmail-made.ztheme.json"), serialize(zmailMade()));
        }
    }

    void roundTrip()
    {
        for (const sbj::brand::Theme &b : sbj::brand::kThemes) {
            const Theme t = fromBrand(b);
            QString err;
            const std::optional<Theme> back = parse(serialize(t), &err);
            QVERIFY2(back, qPrintable(err));
            QVERIFY(*back == t);
            QCOMPARE(serialize(*back), serialize(t));
        }
        Theme full = zmailMade();
        full.terminal = deriveTerminal(full.roles);
        full.terminal->cursorShape = QStringLiteral("bar");
        full.terminal->cursorBlink = true;
        full.fonts.terminal = QStringLiteral("DejaVu Sans Mono");
        full.fonts.terminalSize = 13;
        const std::optional<Theme> back = parse(serialize(full));
        QVERIFY(back && *back == full);
        const QString path = m_config.filePath(QStringLiteral("rt") + QLatin1String(kSuffix));
        QVERIFY(save(full, path));
        QVERIFY(load(path) == full);
    }

    void zmailWritesItsFixture()
    {
        // The cross-app fixture is exactly what this code writes (no terminal block).
        QCOMPARE(serialize(zmailMade()), readFile(fixture("zmail-made.ztheme.json")));
        QVERIFY(!serialize(zmailMade()).contains("\"terminal\""));
    }

    void readsAThemeMadeByZterminal()
    {
        // Written by zterminal's editor code (zterminal tst_themefile checks
        // that): Badgers + terminal block (ANSI, cursor style) + terminal font.
        const std::optional<Theme> t = load(fixture("zterminal-made.ztheme.json"));
        QVERIFY(t);
        QCOMPARE(t->name, QStringLiteral("Badgers Night"));
        QVERIFY(t->terminal); // carried, unused by zmail
        QVERIFY(t->fonts.ui.isEmpty());
        // zmail uses the shared roles: same palette as the Badgers brand theme.
        const sbj::brand::Theme *b = sbj::brand::findTheme("badgers");
        QVERIFY(t->roles == fromBrand(*b).roles);
        QCOMPARE(rolesPalette(t->roles), brandPalette(*b));
        const QFont before = QApplication::font();
        applyTheme(*t, QStringLiteral("custom:x"));
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::Base)), b->background);
        QCOMPARE(QApplication::font().pointSize(), before.pointSize()); // no UI font: unchanged
        applyTheme(ThemeMode::Light);
    }

    void missingRolesAndTerminalAreDerived_data()
    {
        QTest::addColumn<QString>("bg");
        QTest::addColumn<QString>("fg");
        QTest::newRow("dark") << "#1E1E2E" << "#CDD6F4";
        QTest::newRow("light") << "#FFFFFF" << "#202020";
        QTest::newRow("sepia") << "#F4ECD8" << "#5B4636";
        QTest::newRow("green") << "#203731" << "#FFB612";
        QTest::newRow("black") << "#000000" << "#FFFFFF";
    }
    void missingRolesAndTerminalAreDerived()
    {
        QFETCH(QString, bg);
        QFETCH(QString, fg);
        const QByteArray json = QStringLiteral("{\"format\":\"ztheme\",\"version\":1,\"name\":\"Minimal\","
                                               "\"palette\":{\"background\":\"%1\",\"foreground\":\"%2\"}}").arg(bg, fg).toUtf8();
        QString err;
        const std::optional<Theme> t = parse(json, &err);
        QVERIFY2(t, qPrintable(err));
        const Roles &r = t->roles;
        QCOMPARE(hex(r[Background]), bg);
        QVERIFY(contrast(r[Muted], r[Background]) >= 4.5);
        QVERIFY(contrast(r[Link], r[Background]) >= 4.5);
        QVERIFY(contrast(r[SelectionText], r[Selection]) >= 4.5);
        QVERIFY(contrast(r[ChromeText], r[Chrome]) >= 4.5);
        QVERIFY(contrast(r[HeaderText], r[Header]) >= 4.5);
        const Terminal term = terminalOf(*t);
        for (size_t i = 1; i < 16; ++i) {
            QVERIFY(contrast(term.ansi[i], r[Background]) >= 4.5);
        }
        // The derived palette is what zmail shows.
        applyTheme(*t, QStringLiteral("custom:minimal"));
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::Highlight)), r[Selection]);
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::HighlightedText)), r[SelectionText]);
        applyTheme(ThemeMode::Light);
    }

    void badFilesAreRejected_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("why");
        const QByteArray ok = "\"format\":\"ztheme\",\"version\":1,\"name\":\"X\"";
        const QByteArray pal = "\"palette\":{\"background\":\"#000000\",\"foreground\":\"#FFFFFF\"}";
        QTest::newRow("not json") << QByteArray("{nope") << "JSON";
        QTest::newRow("array") << QByteArray("[1,2]") << "object";
        QTest::newRow("format") << QByteArray("{\"format\":\"zthemes\",\"version\":1,\"name\":\"X\"," + pal + "}") << "format";
        QTest::newRow("no version") << QByteArray("{\"format\":\"ztheme\",\"name\":\"X\"," + pal + "}") << "version";
        QTest::newRow("newer version") << QByteArray("{\"format\":\"ztheme\",\"version\":2,\"name\":\"X\"," + pal + "}") << "newer";
        QTest::newRow("no name") << QByteArray("{\"format\":\"ztheme\",\"version\":1," + pal + "}") << "name";
        QTest::newRow("no palette") << QByteArray("{" + ok + "}") << "palette";
        QTest::newRow("no foreground") << QByteArray("{" + ok + ",\"palette\":{\"background\":\"#000000\"}}") << "foreground";
        QTest::newRow("colour name") << QByteArray("{" + ok + ",\"palette\":{\"background\":\"black\",\"foreground\":\"#FFFFFF\"}}") << "palette.background";
        QTest::newRow("font size") << QByteArray("{" + ok + "," + pal + ",\"fonts\":{\"uiSize\":99}}") << "uiSize";
        QTest::newRow("font not string") << QByteArray("{" + ok + "," + pal + ",\"fonts\":{\"ui\":5}}") << "fonts.ui";
        QTest::newRow("stripes") << QByteArray("{" + ok + "," + pal + ",\"ui\":{\"rowStripes\":-1}}") << "rowStripes";
        QTest::newRow("huge") << QByteArray("{" + ok + "," + pal + ",\"pad\":\"" + QByteArray(kMaxFileBytes, 'x') + "\"}") << "large";
    }
    void badFilesAreRejected()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, why);
        QString err;
        QVERIFY(!parse(json, &err));
        QVERIFY2(err.contains(why, Qt::CaseInsensitive), qPrintable(err));
        // Broken files in the themes dir are skipped, not listed.
        writeFile(userThemesDir() + QStringLiteral("/broken.ztheme.json"), json);
        for (const CustomTheme &c : customThemes()) {
            QVERIFY(c.id != QStringLiteral("custom:broken"));
        }
        QVERIFY(!applyThemeId(QStringLiteral("custom:broken")));
        QCOMPARE(currentTheme(), ThemeMode::Light);
        QFile::remove(userThemesDir() + QStringLiteral("/broken.ztheme.json"));
    }

    void editorDuplicatesEditsSavesAndApplies()
    {
        MainWindow w;
        w.show();
        ThemeEditorDialog *ed = w.showThemeEditor();
        QVERIFY(ed->select(QStringLiteral("boilermakers")));
        QVERIFY(!ed->currentEditable()); // built-in: read-only
        QVERIFY(!ed->saveCurrent());
        QVERIFY(ed->duplicateCurrent());
        const QString id = ed->currentId();
        QCOMPARE(id, QStringLiteral("custom:boilermakers-copy"));
        QVERIFY(ed->currentEditable());
        QCOMPARE(ed->theme().basedOn, QStringLiteral("boilermakers"));
        const QString file = userThemesDir() + QStringLiteral("/boilermakers-copy.ztheme.json");
        QVERIFY(QFile::exists(file));
        // Listed in View > Theme with the built-ins.
        auto *menu = w.findChild<QMenu *>(QStringLiteral("menuTheme"));
        emit menu->aboutToShow();
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionTheme:") + id));

        // Live preview (palette, stripes, font), then save.
        Theme t = ed->theme();
        t.roles[Background] = 0x102030;
        t.roles[Header] = 0x445566;
        t.fonts.ui = QApplication::font().family();
        t.fonts.uiSize = 15;
        t.ui.rowStripes = 0;
        ed->setTheme(t);
        QVERIFY(ed->isDirty());
        QCOMPARE(rgbOf(ed->preview()->palette().color(QPalette::Base)), 0x102030u);
        auto *list = ed->preview()->findChild<QTreeWidget *>();
        QCOMPARE(rgbOf(list->header()->palette().color(QPalette::Button)), 0x445566u);
        QVERIFY(!list->alternatingRowColors());
        QCOMPARE(ed->preview()->font().pointSize(), 15);
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::Base)), 0xFFFFFFu); // not applied yet
        QVERIFY(ed->saveCurrent());
        QCOMPARE(load(file)->roles[Background], 0x102030u);

        // Apply: the app uses it (palette, font, stripes) and remembers it.
        const int fontBefore = QApplication::font().pointSize();
        ed->applyCurrent();
        QCOMPARE(currentThemeId(), id);
        QCOMPARE(QSettings().value(QLatin1String(kThemeSettingKey)).toString(), id);
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::Base)), 0x102030u);
        QCOMPARE(QApplication::font().pointSize(), 15);
        QCOMPARE(w.stripeStrength(), 0);
        QVERIFY(w.findChild<QAction *>(QStringLiteral("actionTheme:") + id)->isChecked());
        // Start-up reads the id back.
        applyTheme(ThemeMode::Light);
        QCOMPARE(QApplication::font().pointSize(), fontBefore); // default font back
        QVERIFY(applyThemeId(QSettings().value(QLatin1String(kThemeSettingKey)).toString()));
        QCOMPARE(currentThemeId(), id);
        w.setThemeId(id);

        // Rename moves the file and keeps the app on it.
        QVERIFY(ed->renameCurrent(QStringLiteral("Purdue Night")));
        QCOMPARE(ed->currentId(), QStringLiteral("custom:purdue-night"));
        QVERIFY(!QFile::exists(file));
        QCOMPARE(currentThemeId(), QStringLiteral("custom:purdue-night"));
        QCOMPARE(QSettings().value(QLatin1String(kThemeSettingKey)).toString(), QStringLiteral("custom:purdue-night"));

        // Export (a built-in too) and delete: back to the theme it came from.
        const QString out = m_config.filePath(QStringLiteral("export.ztheme.json"));
        QVERIFY(ed->exportCurrent(out));
        QCOMPARE(load(out)->name, QStringLiteral("Purdue Night"));
        QVERIFY(ed->select(QStringLiteral("light")));
        QVERIFY(ed->exportCurrent(out));
        QCOMPARE(load(out)->roles[Background], 0xFFFFFFu);
        QVERIFY(ed->select(QStringLiteral("custom:purdue-night")));
        QVERIFY(ed->deleteCurrent());
        QVERIFY(!QFile::exists(userThemesDir() + QStringLiteral("/purdue-night.ztheme.json")));
        QCOMPARE(currentThemeId(), QStringLiteral("boilermakers"));
        QCOMPARE(ed->currentId(), QStringLiteral("boilermakers"));
        QVERIFY(!w.findChild<QAction *>(QStringLiteral("actionTheme:custom:purdue-night")));
        ed->close();
        w.setTheme(ThemeMode::Light);
    }

    void editorImportsAndListsZterminalThemes()
    {
        ThemeEditorDialog ed(QStringLiteral("light"), kStripeDefault);
        // A bad file is refused with a reason; nothing is added.
        const QString bad = m_config.filePath(QStringLiteral("bad.ztheme.json"));
        writeFile(bad, "{\"format\":\"ztheme\",\"version\":9,\"name\":\"Future\"}");
        QString err;
        QVERIFY(!ed.importFile(bad, &err));
        QVERIFY(err.contains(QStringLiteral("newer")));
        // zterminal's file imports as an editable copy; its terminal block is kept.
        QVERIFY2(ed.importFile(fixture("zterminal-made.ztheme.json"), &err), qPrintable(err));
        QCOMPARE(ed.currentId(), QStringLiteral("custom:badgers-night"));
        QVERIFY(ed.currentEditable());
        QVERIFY(ed.theme().terminal);
        QCOMPARE(ed.theme().terminal->ansi[4], 0x7AB0F0u);
        QVERIFY(ed.exportCurrent(m_config.filePath(QStringLiteral("again.ztheme.json"))));
        QCOMPARE(readFile(m_config.filePath(QStringLiteral("again.ztheme.json"))),
                 readFile(fixture("zterminal-made.ztheme.json"))); // nothing lost on the way through zmail

        // Themes in zterminal's own dir are listed read-only.
        writeFile(zterminalThemesDir() + QStringLiteral("/night.ztheme.json"), readFile(fixture("zterminal-made.ztheme.json")));
        bool listed = false;
        for (const CustomTheme &c : customThemes()) {
            if (c.id == QStringLiteral("zterminal:night")) {
                listed = true;
                QVERIFY(!c.editable);
                QVERIFY(c.name.endsWith(QStringLiteral("(zterminal)")));
            }
        }
        QVERIFY(listed);
        ThemeEditorDialog ed2(QStringLiteral("light"), kStripeDefault);
        QVERIFY(ed2.select(QStringLiteral("zterminal:night")));
        QVERIFY(!ed2.currentEditable());
        QVERIFY(!ed2.renameCurrent(QStringLiteral("Mine")));
        QVERIFY(!ed2.deleteCurrent());
        QVERIFY(QFile::exists(zterminalThemesDir() + QStringLiteral("/night.ztheme.json")));
        QVERIFY(applyThemeId(QStringLiteral("zterminal:night")));
        QCOMPARE(rgbOf(QApplication::palette().color(QPalette::Base)), sbj::brand::findTheme("badgers")->background);
        // A missing custom theme falls back to Light.
        QVERIFY(!applyThemeId(QStringLiteral("custom:gone")));
        QCOMPARE(currentTheme(), ThemeMode::Light);
    }
};

QTEST_MAIN(TstThemeFile)
#include "tst_themefile.moc"
