#pragma once

#include <QString>
#include <QStringList>

namespace zmail {

// Turns the toolbar search box into an FTS5 MATCH string or a LIKE needle.
// Strips known operators (from:, has:, …) so they can be applied as post-filters;
// bare words and "quoted phrases" become the full-text query.
class SearchQuery
{
public:
    // FTS5: token* AND token* (prefix), phrases as "…". Empty if nothing to MATCH.
    static QString toFts5(const QString &userText);
    // LIKE fallback needle (no wildcards inside; caller wraps with %).
    static QString toLikeNeedle(const QString &userText);
    // Words/phrases left after removing from:/to:/subject:/label:/has:/is: operators.
    static QStringList freeTextTokens(const QString &userText);
};

} // namespace zmail
