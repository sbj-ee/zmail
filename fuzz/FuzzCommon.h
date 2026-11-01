#pragma once

// Shared setup for zmail's libFuzzer harnesses: a headless Qt application
// (the sanitizer and HtmlFit use QTextDocument, which needs a GUI app) and
// silenced Qt logging so a fuzz run's output is only libFuzzer's.

#include <QGuiApplication>
#include <QLoggingCategory>

#include <cstdlib>

namespace zmail::fuzz {

inline void initQt()
{
    static int argc = 1;
    static char name[] = "zmail-fuzz";
    static char *argv[] = {name, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QLoggingCategory::setFilterRules(QStringLiteral("*=false"));
    static QGuiApplication app(argc, argv);
}

} // namespace zmail::fuzz
