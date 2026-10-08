#include "Stationery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace zmail {

QString StationeryStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/stationery.json");
}

StationeryStore::StationeryStore(const QString &path)
    : m_path(path)
{
}

QList<Stationery> StationeryStore::all() const
{
    QList<Stationery> out;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) {
        return out;
    }
    for (const auto &v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("stationery")).toArray()) {
        const QJsonObject o = v.toObject();
        Stationery s;
        s.name = o.value(QStringLiteral("name")).toString().trimmed();
        s.to = o.value(QStringLiteral("to")).toString();
        s.cc = o.value(QStringLiteral("cc")).toString();
        s.subject = o.value(QStringLiteral("subject")).toString();
        s.body = o.value(QStringLiteral("body")).toString();
        if (!s.name.isEmpty()) {
            out.append(s);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Stationery &a, const Stationery &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return out;
}

bool StationeryStore::setAll(const QList<Stationery> &items)
{
    QJsonArray arr;
    for (const Stationery &s : items) {
        if (s.name.trimmed().isEmpty()) {
            continue;
        }
        arr.append(QJsonObject{{QStringLiteral("name"), s.name.trimmed()},
                               {QStringLiteral("to"), s.to},
                               {QStringLiteral("cc"), s.cc},
                               {QStringLiteral("subject"), s.subject},
                               {QStringLiteral("body"), s.body}});
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    return f.open(QIODevice::WriteOnly) &&
           f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("stationery"), arr}})
                       .toJson(QJsonDocument::Indented)) >= 0 &&
           f.commit();
}

Stationery StationeryStore::find(const QString &name) const
{
    for (const Stationery &s : all()) {
        if (s.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0) {
            return s;
        }
    }
    return {};
}

bool StationeryStore::save(const Stationery &item)
{
    if (item.name.trimmed().isEmpty()) {
        return false;
    }
    QList<Stationery> items = all();
    bool replaced = false;
    for (Stationery &s : items) {
        if (s.name.compare(item.name.trimmed(), Qt::CaseInsensitive) == 0) {
            s = item;
            s.name = item.name.trimmed();
            replaced = true;
        }
    }
    if (!replaced) {
        items.append(item);
    }
    return setAll(items);
}

bool StationeryStore::remove(const QString &name)
{
    QList<Stationery> items = all();
    const qsizetype before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&name](const Stationery &s) { return s.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0; }),
                items.end());
    return items.size() != before && setAll(items);
}

} // namespace zmail
