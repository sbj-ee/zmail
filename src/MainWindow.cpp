#include "MainWindow.hpp"

#include "AboutDialog.hpp"
#include "UpdateChecker.hpp"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(baseTitle());

    // Stephen's desktop runs the Fildem global-menu extension, which swallowed
    // zterminal's menu bar. Keep the menu bar inside the window.
    menuBar()->setNativeMenuBar(false);

    m_updates = new UpdateChecker(this);
    connect(m_updates, &UpdateChecker::updateAvailable, this,
            [this](const QString &tag, const QString &url) {
                const auto choice = QMessageBox::question(
                    this, tr("Update available"),
                    tr("zmail %1 is available (you have %2).\n\nOpen the release page?")
                        .arg(tag, QString::fromLatin1(zmail::kVersionString)));
                if (choice == QMessageBox::Yes) {
                    QDesktopServices::openUrl(QUrl(url));
                }
            });
    connect(m_updates, &UpdateChecker::upToDate, this, [this]() {
        statusBar()->showMessage(
            tr("zmail %1 is up to date.").arg(QString::fromLatin1(zmail::kVersionString)), 5000);
    });
    connect(m_updates, &UpdateChecker::checkFailed, this, [this](const QString &reason) {
        statusBar()->showMessage(tr("Update check failed: %1").arg(reason), 8000);
    });

    buildMenus();
    buildPanes();
    statusBar()->showMessage(tr("Not signed in. Gmail sign-in arrives in milestone M1."));
    resize(1100, 700);
}

QString MainWindow::baseTitle()
{
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(zmail::kAppName),
                                       QString::fromLatin1(zmail::kVersionString));
}

QColor MainWindow::suspiciousForeground()
{
    return QColor(0xb7, 0x1c, 0x1c);
}

QColor MainWindow::suspiciousBackground()
{
    return QColor(0xff, 0xeb, 0xee);
}

void MainWindow::buildMenus()
{
    auto addMenu = [this](const char *objName, const QString &title) {
        QMenu *m = menuBar()->addMenu(title);
        m->setObjectName(QString::fromLatin1(objName));
        return m;
    };
    auto later = [](QMenu *menu, const QString &text) {
        QAction *a = menu->addAction(text);
        a->setEnabled(false); // placeholder until the feature lands
        return a;
    };

    QMenu *file = addMenu("menuFile", tr("&File"));
    later(file, tr("&New Message"));
    later(file, tr("&Save Attachments\u2026"));
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *edit = addMenu("menuEdit", tr("&Edit"));
    later(edit, tr("&Copy"));
    later(edit, tr("&Find\u2026"));

    QMenu *view = addMenu("menuView", tr("&View"));
    later(view, tr("View as &Plain Text"));
    later(view, tr("&Scheduled"));

    QMenu *message = addMenu("menuMessage", tr("&Message"));
    later(message, tr("&Reply"));
    later(message, tr("Reply &All"));
    later(message, tr("&Forward"));
    later(message, tr("Mark as &Suspicious"));

    QMenu *settings = addMenu("menuSettings", tr("&Settings"));
    later(settings, tr("&Account\u2026"));
    later(settings, tr("Sound &Rules\u2026"));
    later(settings, tr("Si&gnatures\u2026"));

    QMenu *help = addMenu("menuHelp", tr("&Help"));
    QAction *about = help->addAction(tr("&About zmail"), this, &MainWindow::showAbout);
    about->setObjectName(QStringLiteral("actionAbout"));
    QAction *upd = help->addAction(tr("Check for &Updates\u2026"), this, &MainWindow::checkForUpdates);
    upd->setObjectName(QStringLiteral("actionCheckForUpdates"));
}

void MainWindow::buildPanes()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("mainSplitter"));

    m_folders = new QTreeWidget(m_splitter);
    m_folders->setObjectName(QStringLiteral("folderPane"));
    m_folders->setHeaderLabel(tr("Labels"));
    for (const QString &name : {tr("Inbox"), tr("Starred"), tr("Sent"), tr("Drafts"),
                                tr("Scheduled"), tr("All Mail"), tr("Spam"), tr("Trash")}) {
        new QTreeWidgetItem(m_folders, QStringList{name});
    }

    m_messages = new QListWidget(m_splitter);
    m_messages->setObjectName(QStringLiteral("messageList"));
    m_messages->addItem(tr("(placeholder) Welcome to zmail"));
    m_messages->addItem(tr("(placeholder) Sign-in, sync and sounds arrive in M1\u2013M3"));
    // Eudora-style colour coding preview: a rule-coloured row and the fixed
    // "suspicious" warning colour that rules cannot override (see PLAN.md).
    auto *ruleRow = new QListWidgetItem(tr("(placeholder) Family \u2014 rule colour: teal"), m_messages);
    ruleRow->setForeground(QColor(0x00, 0x7a, 0x7a));
    ruleRow->setData(Qt::UserRole, QStringLiteral("rule"));
    auto *labelRow = new QListWidgetItem(tr("(placeholder) Label work \u2014 rule colour: purple"), m_messages);
    labelRow->setForeground(QColor(0x6a, 0x1b, 0x9a));
    labelRow->setData(Qt::UserRole, QStringLiteral("rule"));
    auto *warnRow = new QListWidgetItem(tr("\u26a0 (placeholder) Suspicious \u2014 fixed warning colour"), m_messages);
    warnRow->setForeground(suspiciousForeground());
    warnRow->setBackground(suspiciousBackground());
    warnRow->setData(Qt::UserRole, QStringLiteral("suspicious"));

    m_preview = new QTextBrowser(m_splitter);
    m_preview->setObjectName(QStringLiteral("previewPane"));
    m_preview->setOpenLinks(false);
    m_preview->setHtml(tr("<h3>zmail %1</h3><p>Preview pane placeholder. Messages will "
                          "render here with remote images blocked by default.</p>")
                           .arg(QString::fromLatin1(zmail::kVersionString)));

    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setStretchFactor(2, 3);
    m_splitter->setSizes({200, 360, 540});
    setCentralWidget(m_splitter);
}

void MainWindow::showAbout()
{
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::checkForUpdates()
{
    statusBar()->showMessage(tr("Checking for updates\u2026"));
    m_updates->checkForUpdates(QString::fromLatin1(zmail::kVersionString),
                               QString::fromLatin1(zmail::kRepo));
}
