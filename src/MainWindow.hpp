#pragma once

#include <QMainWindow>

class QColor;
class QListWidget;
class QSplitter;
class QTextBrowser;
class QTreeWidget;
class UpdateChecker;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // "zmail 0.1.0": built from project(VERSION), never hardcoded.
    static QString baseTitle();

    // Fixed warning colours for suspicious mail; colour rules cannot override.
    static QColor suspiciousForeground();
    static QColor suspiciousBackground();

public slots:
    void showAbout();
    void checkForUpdates();

private:
    void buildMenus();
    void buildPanes();

    QSplitter *m_splitter = nullptr;
    QTreeWidget *m_folders = nullptr;
    QListWidget *m_messages = nullptr;
    QTextBrowser *m_preview = nullptr;
    UpdateChecker *m_updates = nullptr;
};
