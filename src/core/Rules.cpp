#include "Rules.h"

#include "Log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace zmail {

QString Rules::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/rules.json");
}

bool Rules::matches(const RuleCondition &c, const RuleMessage &m)
{
    const auto test = [&c](const QString &text) {
        const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
        if (c.op == QLatin1String("is")) {
            return text.trimmed().compare(c.value.trimmed(), cs) == 0;
        }
        if (c.op == QLatin1String("startsWith")) {
            return text.trimmed().startsWith(c.value.trimmed(), cs);
        }
        if (c.op == QLatin1String("endsWith")) {
            return text.trimmed().endsWith(c.value.trimmed(), cs);
        }
        if (c.op == QLatin1String("regex")) {
            const QRegularExpression re(c.value, QRegularExpression::CaseInsensitiveOption);
            return re.isValid() && re.match(text).hasMatch(); // a broken pattern matches nothing
        }
        return text.contains(c.value, cs); // contains, and notContains (negated below)
    };
    const bool negate = c.op == QLatin1String("notContains");
    bool hit = false;
    if (c.field == QLatin1String("to")) {
        hit = test(m.to);
    } else if (c.field == QLatin1String("subject")) {
        hit = test(m.subject);
    } else if (c.field == QLatin1String("any")) {
        // "Does not contain" over several headers: in none of them.
        hit = test(m.from) || test(m.to) || test(m.subject);
    } else {
        hit = test(m.from);
    }
    return negate ? !hit : hit;
}

bool Rules::matches(const Rule &rule, const RuleMessage &m)
{
    if (rule.conditions.isEmpty()) {
        return true;
    }
    for (const RuleCondition &c : rule.conditions) {
        const bool hit = matches(c, m);
        if (rule.matchAny && hit) {
            return true;
        }
        if (!rule.matchAny && !hit) {
            return false;
        }
    }
    return !rule.matchAny;
}

const Rule *Rules::match(const RuleMessage &m) const
{
    for (const Rule &r : rules) {
        if (r.enabled && matches(r, m)) {
            return &r;
        }
    }
    return nullptr;
}

QJsonObject Rules::toJson(const QList<Rule> &rules)
{
    QJsonArray arr;
    for (const Rule &r : rules) {
        QJsonArray conds;
        for (const RuleCondition &c : r.conditions) {
            conds.append(QJsonObject{{QStringLiteral("field"), c.field},
                                     {QStringLiteral("op"), c.op},
                                     {QStringLiteral("value"), c.value}});
        }
        QJsonObject o{{QStringLiteral("name"), r.name},
                      {QStringLiteral("enabled"), r.enabled},
                      {QStringLiteral("match"), r.matchAny ? QStringLiteral("any") : QStringLiteral("all")},
                      {QStringLiteral("conditions"), conds}};
        // Actions are written only when set, so the file reads as what a rule does.
        if (!r.color.isEmpty()) o.insert(QStringLiteral("color"), r.color);
        if (!r.flag.isEmpty()) o.insert(QStringLiteral("flag"), r.flag);
        if (!r.sound.isEmpty()) o.insert(QStringLiteral("sound"), r.sound);
        if (!r.moveTo.isEmpty()) o.insert(QStringLiteral("moveTo"), r.moveTo);
        if (r.markRead) o.insert(QStringLiteral("markRead"), true);
        arr.append(o);
    }
    return {{QStringLiteral("version"), 1}, {QStringLiteral("rules"), arr}};
}

QList<Rule> Rules::fromJson(const QJsonObject &doc)
{
    static const QStringList fields{QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("subject"),
                                    QStringLiteral("any")};
    static const QStringList ops{QStringLiteral("contains"), QStringLiteral("notContains"), QStringLiteral("is"),
                                 QStringLiteral("startsWith"), QStringLiteral("endsWith"), QStringLiteral("regex")};
    QList<Rule> out;
    for (const auto &v : doc.value(QStringLiteral("rules")).toArray()) {
        const QJsonObject o = v.toObject();
        Rule r;
        r.name = o.value(QStringLiteral("name")).toString();
        r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        r.matchAny = o.value(QStringLiteral("match")).toString() == QLatin1String("any");
        for (const auto &cv : o.value(QStringLiteral("conditions")).toArray()) {
            const QJsonObject co = cv.toObject();
            RuleCondition c;
            c.field = co.value(QStringLiteral("field")).toString();
            c.op = co.value(QStringLiteral("op")).toString();
            c.value = co.value(QStringLiteral("value")).toString();
            // A hand-edited file: unknown words fall back to the plainest reading.
            if (!fields.contains(c.field)) c.field = fields.first();
            if (!ops.contains(c.op)) c.op = ops.first();
            r.conditions.append(c);
        }
        r.color = o.value(QStringLiteral("color")).toString();
        r.flag = o.value(QStringLiteral("flag")).toString();
        r.sound = o.value(QStringLiteral("sound")).toString();
        r.moveTo = o.value(QStringLiteral("moveTo")).toString();
        r.markRead = o.value(QStringLiteral("markRead")).toBool(false);
        out.append(r);
    }
    return out;
}

bool Rules::load(const QString &path)
{
    QFile f(path);
    if (!f.exists()) {
        rules.clear();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(lcSync) << "Rules: can't read" << path;
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcSync) << "Rules:" << path << "is not valid JSON:" << err.errorString();
        return false;
    }
    rules = fromJson(doc.object());
    return true;
}

bool Rules::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(toJson(rules)).toJson(QJsonDocument::Indented)) >= 0 &&
           f.commit();
}

} // namespace zmail
