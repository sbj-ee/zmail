#include "SpellHighlighter.h"

#include "core/SpellChecker.h"

#include <QAction>
#include <QMenu>
#include <QRegularExpression>
#include <QTextEdit>

namespace zmail {

namespace {
const QRegularExpression &wordRe()
{
    static const QRegularExpression re(QStringLiteral("[\\p{L}][\\p{L}\\p{M}'’]*[\\p{L}]|[\\p{L}]"));
    return re;
}
} // namespace

SpellHighlighter::SpellHighlighter(SpellChecker *checker, QTextDocument *doc)
    : QSyntaxHighlighter(doc)
    , m_checker(checker)
{
    connect(checker, &SpellChecker::dictionaryChanged, this, &QSyntaxHighlighter::rehighlight);
}

void SpellHighlighter::highlightBlock(const QString &text)
{
    if (!m_checker->isEnabled() || !m_checker->isAvailable()) {
        return;
    }
    // Leave quoted lines, URLs and addresses alone.
    if (text.startsWith(QLatin1Char('>'))) {
        return;
    }
    QTextCharFormat bad;
    bad.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    bad.setUnderlineColor(QColor(0xd3, 0x2f, 0x2f));
    static const QRegularExpression skip(QStringLiteral("(https?://|www\\.)\\S+|\\S+@\\S+"));
    QList<QPair<int, int>> skipped;
    for (auto it = skip.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        skipped << qMakePair(m.capturedStart(), m.capturedEnd());
    }
    for (auto it = wordRe().globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        bool inSkip = false;
        for (const auto &s : skipped) {
            inSkip = inSkip || (m.capturedStart() >= s.first && m.capturedStart() < s.second);
        }
        if (!inSkip && !m_checker->isCorrect(m.captured())) {
            QTextCharFormat f = format(m.capturedStart());
            f.merge(bad);
            setFormat(m.capturedStart(), int(m.capturedLength()), f);
        }
    }
}

void SpellHighlighter::addSuggestions(QMenu *menu, QTextEdit *edit, SpellChecker *checker, const QPoint &pos)
{
    if (!checker || !checker->isAvailable() || !checker->isEnabled()) {
        return;
    }
    QTextCursor c = edit->cursorForPosition(pos);
    const QString block = c.block().text();
    const int at = c.positionInBlock();
    for (auto it = wordRe().globalMatch(block); it.hasNext();) {
        const auto m = it.next();
        if (at < m.capturedStart() || at > m.capturedEnd()) {
            continue;
        }
        const QString word = m.captured();
        if (checker->isCorrect(word)) {
            return;
        }
        QAction *first = menu->actions().value(0);
        const QStringList sugg = checker->suggestions(word);
        QList<QAction *> added;
        for (const QString &s : sugg) {
            auto *a = new QAction(s, menu);
            QFont f = a->font();
            f.setBold(true);
            a->setFont(f);
            const int start = int(c.block().position() + m.capturedStart());
            const int len = int(m.capturedLength());
            QObject::connect(a, &QAction::triggered, edit, [edit, start, len, s] {
                QTextCursor r(edit->document());
                r.setPosition(start);
                r.setPosition(start + len, QTextCursor::KeepAnchor);
                r.insertText(s);
            });
            added << a;
        }
        if (sugg.isEmpty()) {
            auto *none = new QAction(QObject::tr("(no suggestions)"), menu);
            none->setEnabled(false);
            added << none;
        }
        auto *add = new QAction(QObject::tr("Add “%1” to Dictionary").arg(word), menu);
        QObject::connect(add, &QAction::triggered, checker, [checker, word] { checker->addToUserDictionary(word); });
        auto *ignore = new QAction(QObject::tr("Ignore “%1”").arg(word), menu);
        QObject::connect(ignore, &QAction::triggered, checker, [checker, word] { checker->ignoreWord(word); });
        added << add << ignore;
        menu->insertActions(first, added);
        menu->insertSeparator(first);
        return;
    }
}

} // namespace zmail
