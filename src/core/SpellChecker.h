#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class Hunspell;

namespace zmail {

// Hunspell wrapper ported from zwriter's SpellChecker (MIT, same author).
// Shares zwriter's personal dictionary, so a word added in either app is
// known to both: ~/.local/share/sbj-ee/zwriter/user-dictionary.txt, one word
// per line (ZMAIL_USER_DICTIONARY overrides the path, for tests).
// A no-op (everything is "correct") when zmail is built without Hunspell or
// no en_US dictionary is installed.
class SpellChecker : public QObject
{
    Q_OBJECT

public:
    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    bool isAvailable() const;
    QString dictionaryId() const { return m_dictId; }
    static QString userDictionaryPath();

    bool isCorrect(const QString &word) const;
    QStringList suggestions(const QString &word, int maxSuggestions = 8) const;

    void ignoreWord(const QString &word);          // this session only
    void addToUserDictionary(const QString &word); // persists (shared with zwriter)

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

signals:
    void dictionaryChanged();

private:
    void loadUserDictionary();
    void addRuntime(const QString &word);

    Hunspell *m_hunspell = nullptr;
    QString m_dictId = QStringLiteral("en_US");
    QSet<QString> m_ignored; // lower-cased
    QSet<QString> m_userWords;
    bool m_enabled = true;
};

} // namespace zmail
