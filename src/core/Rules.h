#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace zmail {

// Filters, as in Eudora: an ordered list of rules, each a set of conditions
// on a message's headers and the things to do when they hold. The first
// enabled rule that matches is the one that applies; the rest are skipped.
//
// What a rule can do:
//  - colour the message's row in the list (applies to every listed message),
//  - when the message arrives, or on Message > Filter Messages: flag it in a
//    colour, move it to a folder, mark it read,
//  - when it arrives: play a sound of its own, or none.

struct RuleCondition
{
    QString field = QStringLiteral("from"); // from | to | subject | any (any of the three)
    QString op = QStringLiteral("contains"); // contains | notContains | is | startsWith | endsWith | regex
    QString value;
    bool operator==(const RuleCondition &) const = default;
};

struct Rule
{
    QString name;
    bool enabled = true;
    bool matchAny = false;            // false: every condition; true: any one
    QList<RuleCondition> conditions;  // none: matches every message (a catch-all last rule)

    QString color;   // "#rrggbb" row colour; empty: none
    QString flag;    // flag colour id (ui/Flags.h); empty: none
    QString sound;   // empty: the usual new-mail sound; "none": silence; else a .wav path
    QString moveTo;  // Gmail label id of a folder; empty: stays put
    bool markRead = false;

    bool operator==(const Rule &) const = default;
    bool hasArrivalActions() const { return !flag.isEmpty() || !moveTo.isEmpty() || markRead; }
};

// The headers rules look at.
struct RuleMessage
{
    QString from; // "Name <address>"
    QString to;
    QString subject;
};

inline constexpr const char *kRuleSoundNone = "none";

class Rules
{
public:
    static QString defaultPath(); // <config>/rules.json

    // A missing file is an empty list (true); unreadable or not JSON is false
    // and leaves the list as it was.
    bool load(const QString &path = defaultPath());
    bool save(const QString &path = defaultPath()) const;

    QList<Rule> rules;

    // The first enabled rule that matches, or nullptr.
    const Rule *match(const RuleMessage &m) const;
    static bool matches(const Rule &rule, const RuleMessage &m);
    static bool matches(const RuleCondition &c, const RuleMessage &m);

    static QJsonObject toJson(const QList<Rule> &rules);
    static QList<Rule> fromJson(const QJsonObject &doc);
};

} // namespace zmail
