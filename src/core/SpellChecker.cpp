#include "SpellChecker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#ifdef ZMAIL_HAVE_HUNSPELL
#include <hunspell/hunspell.hxx>
#endif

namespace zmail {

namespace {
QString findDictBase(const QString &lang)
{
    const QStringList dirs{QStringLiteral("/usr/share/hunspell"), QStringLiteral("/usr/share/myspell/dicts"),
                           QStringLiteral("/usr/local/share/hunspell"), QStringLiteral("/usr/share/myspell")};
    for (const QString &dir : dirs) {
        const QString base = dir + QLatin1Char('/') + lang;
        if (QFileInfo::exists(base + QStringLiteral(".aff")) && QFileInfo::exists(base + QStringLiteral(".dic"))) {
            return base;
        }
    }
    return {};
}
} // namespace

SpellChecker::SpellChecker(QObject *parent)
    : QObject(parent)
{
#ifdef ZMAIL_HAVE_HUNSPELL
    const QString base = findDictBase(m_dictId);
    if (!base.isEmpty()) {
        m_hunspell = new Hunspell(QFile::encodeName(base + QStringLiteral(".aff")).constData(),
                                  QFile::encodeName(base + QStringLiteral(".dic")).constData());
    }
#endif
    loadUserDictionary();
}

SpellChecker::~SpellChecker()
{
#ifdef ZMAIL_HAVE_HUNSPELL
    delete m_hunspell;
#endif
}

bool SpellChecker::isAvailable() const
{
    return m_hunspell != nullptr;
}

QString SpellChecker::userDictionaryPath()
{
    const QString env = qEnvironmentVariable("ZMAIL_USER_DICTIONARY");
    if (!env.isEmpty()) {
        return env;
    }
    // zwriter's AppDataLocation is <GenericData>/sbj-ee/zwriter.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/sbj-ee/zwriter/user-dictionary.txt");
}

void SpellChecker::setEnabled(bool enabled)
{
    if (m_enabled != enabled) {
        m_enabled = enabled;
        emit dictionaryChanged();
    }
}

bool SpellChecker::isCorrect(const QString &word) const
{
    const QString w = word.trimmed();
    if (!m_enabled || w.size() <= 1) {
        return true;
    }
    bool hasLetter = false;
    for (const QChar ch : w) {
        if (ch.isDigit()) {
            return true; // "4G", "x86": not words
        }
        hasLetter = hasLetter || ch.isLetter();
    }
    if (!hasLetter || m_ignored.contains(w.toLower()) || m_userWords.contains(w)) {
        return true;
    }
#ifdef ZMAIL_HAVE_HUNSPELL
    if (m_hunspell) {
        return m_hunspell->spell(w.toStdString());
    }
#endif
    return true;
}

QStringList SpellChecker::suggestions(const QString &word, int maxSuggestions) const
{
    QStringList out;
#ifdef ZMAIL_HAVE_HUNSPELL
    if (m_hunspell && m_enabled) {
        for (const std::string &s : m_hunspell->suggest(word.trimmed().toStdString())) {
            out << QString::fromStdString(s);
            if (out.size() >= maxSuggestions) {
                break;
            }
        }
    }
#else
    Q_UNUSED(word)
    Q_UNUSED(maxSuggestions)
#endif
    return out;
}

void SpellChecker::ignoreWord(const QString &word)
{
    const QString w = word.trimmed();
    if (!w.isEmpty()) {
        m_ignored.insert(w.toLower());
        emit dictionaryChanged();
    }
}

void SpellChecker::addRuntime(const QString &w)
{
    m_userWords.insert(w);
#ifdef ZMAIL_HAVE_HUNSPELL
    if (m_hunspell) {
        m_hunspell->add(w.toStdString());
    }
#endif
}

void SpellChecker::addToUserDictionary(const QString &word)
{
    const QString w = word.trimmed();
    if (w.isEmpty() || w.contains(QLatin1Char('\n'))) {
        return;
    }
    addRuntime(w);
    m_ignored.remove(w.toLower());
    const QString path = userDictionaryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << w << QLatin1Char('\n');
    }
    emit dictionaryChanged();
}

void SpellChecker::loadUserDictionary()
{
    QFile f(userDictionaryPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString w = in.readLine().trimmed();
        if (!w.isEmpty()) {
            addRuntime(w);
        }
    }
}

} // namespace zmail
