#pragma once

#include <QList>
#include <QString>

namespace zmail {

// Stationery, as in Eudora: a message kept as a template. Starting a new
// message or a reply "with" one fills in its text (and its recipients and
// subject, where the message has none yet).
struct Stationery
{
    QString name;
    QString to, cc;
    QString subject;
    QString body; // plain text
    bool operator==(const Stationery &) const = default;
};

// Kept in <config>/stationery.json.
class StationeryStore
{
public:
    static QString defaultPath();
    explicit StationeryStore(const QString &path = defaultPath());

    QList<Stationery> all() const; // by name
    bool setAll(const QList<Stationery> &items);
    Stationery find(const QString &name) const; // name empty if there is none
    // Adds it, or replaces the one with the same name (case doesn't matter).
    bool save(const Stationery &item);
    bool remove(const QString &name);

private:
    QString m_path;
};

} // namespace zmail
