// Linked into every test program: the tests use the built-in sample mail,
// which a real zmail only shows with --demo.
#include "MainWindow.hpp"

#include <QCoreApplication>

static void zmailTestsUseDemoData()
{
    MainWindow::setDemoData(true);
}
Q_COREAPP_STARTUP_FUNCTION(zmailTestsUseDemoData)
