#include "Vacation.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QTextDocumentFragment>

namespace zmail {

namespace {
qint64 int64Of(const QJsonValue &v)
{
    // int64 fields arrive as JSON strings.
    return v.isString() ? v.toString().toLongLong() : v.toInteger();
}

QString longDate(const QDate &d)
{
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(d, QStringLiteral("MMMM d"));
}
} // namespace

VacationSettings VacationSettings::fromJson(const QJsonObject &o)
{
    VacationSettings v;
    v.enabled = o.value(QStringLiteral("enableAutoReply")).toBool();
    v.subject = o.value(QStringLiteral("responseSubject")).toString();
    v.text = o.value(QStringLiteral("responseBodyPlainText")).toString();
    if (v.text.trimmed().isEmpty()) {
        const QString html = o.value(QStringLiteral("responseBodyHtml")).toString();
        if (!html.isEmpty()) {
            v.text = QTextDocumentFragment::fromHtml(html).toPlainText();
        }
    }
    v.contactsOnly = o.value(QStringLiteral("restrictToContacts")).toBool();
    v.domainOnly = o.value(QStringLiteral("restrictToDomain")).toBool();
    if (const qint64 start = int64Of(o.value(QStringLiteral("startTime"))); start > 0) {
        v.firstDay = QDateTime::fromMSecsSinceEpoch(start, Qt::UTC).date();
    }
    if (const qint64 end = int64Of(o.value(QStringLiteral("endTime"))); end > 0) {
        v.lastDay = QDateTime::fromMSecsSinceEpoch(end - 1, Qt::UTC).date();
    }
    return v;
}

QJsonObject VacationSettings::toJson() const
{
    QJsonObject o{{QStringLiteral("enableAutoReply"), enabled},
                  {QStringLiteral("responseSubject"), subject},
                  {QStringLiteral("responseBodyPlainText"), text},
                  {QStringLiteral("restrictToContacts"), contactsOnly},
                  {QStringLiteral("restrictToDomain"), domainOnly}};
    if (firstDay.isValid()) {
        o.insert(QStringLiteral("startTime"),
                 QString::number(QDateTime(firstDay, QTime(0, 0), Qt::UTC).toMSecsSinceEpoch()));
    }
    if (lastDay.isValid()) {
        o.insert(QStringLiteral("endTime"),
                 QString::number(QDateTime(lastDay.addDays(1), QTime(0, 0), Qt::UTC).toMSecsSinceEpoch()));
    }
    return o;
}

QString VacationSettings::summary() const
{
    if (!enabled) {
        return QCoreApplication::translate("Vacation", "Off");
    }
    const QDate today = QDateTime::currentDateTimeUtc().date();
    if (lastDay.isValid() && lastDay < today) {
        return QCoreApplication::translate("Vacation", "Ended %1").arg(longDate(lastDay));
    }
    if (firstDay.isValid() && firstDay > today) {
        return lastDay.isValid()
                   ? QCoreApplication::translate("Vacation", "Starts %1, until %2").arg(longDate(firstDay), longDate(lastDay))
                   : QCoreApplication::translate("Vacation", "Starts %1").arg(longDate(firstDay));
    }
    return lastDay.isValid() ? QCoreApplication::translate("Vacation", "On until %1").arg(longDate(lastDay))
                             : QCoreApplication::translate("Vacation", "On");
}

} // namespace zmail
