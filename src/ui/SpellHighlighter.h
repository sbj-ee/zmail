#pragma once

#include <QSyntaxHighlighter>

class QMenu;
class QTextEdit;

namespace zmail {

class SpellChecker;

// Red squiggles under misspelt words, plus a context-menu helper with
// suggestions, "Add to Dictionary" and "Ignore".
class SpellHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    SpellHighlighter(SpellChecker *checker, QTextDocument *doc);

    // Prepends spelling entries for the word under `pos` (viewport coords).
    static void addSuggestions(QMenu *menu, QTextEdit *edit, SpellChecker *checker, const QPoint &pos);

protected:
    void highlightBlock(const QString &text) override;

private:
    SpellChecker *m_checker;
};

} // namespace zmail
