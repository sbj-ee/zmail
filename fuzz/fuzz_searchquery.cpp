// SearchQuery: the search box text turned into an FTS5 MATCH expression and a
// LIKE needle. The FTS5 expression is also run against a real in-memory FTS5
// table, so one SQLite rejects as a syntax error (or crashes on) is caught.
#include "FuzzCommon.h"
#include "core/SearchQuery.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace {
bool g_fts5 = false;
}

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    zmail::fuzz::initQt();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fuzz"));
    db.setDatabaseName(QStringLiteral(":memory:"));
    if (db.open()) {
        QSqlQuery q(db);
        g_fts5 = q.exec(QStringLiteral("CREATE VIRTUAL TABLE t USING fts5(a, b, tokenize='unicode61 remove_diacritics 2')"));
        q.exec(QStringLiteral("INSERT INTO t(a, b) VALUES ('hello world', 'example.com invoice')"));
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    using zmail::SearchQuery;
    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const QString fts = SearchQuery::toFts5(text);
    (void)SearchQuery::toLikeNeedle(text);
    (void)SearchQuery::freeTextTokens(text);
    if (g_fts5 && !fts.isEmpty()) {
        QSqlQuery q(QSqlDatabase::database(QStringLiteral("fuzz")));
        q.prepare(QStringLiteral("SELECT rowid FROM t WHERE t MATCH ?"));
        q.addBindValue(fts);
        if (!q.exec()) {
            // toFts5 promises an expression FTS5 accepts: a syntax error here is a bug.
            if (q.lastError().text().contains(QLatin1String("syntax error"), Qt::CaseInsensitive)) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
