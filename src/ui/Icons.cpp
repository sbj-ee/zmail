#include "Icons.h"

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace zmail::ui {

namespace {
QPixmap render(const QByteArray &svg, int size, qreal dpr)
{
    QSvgRenderer r(svg);
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p, QRectF(0, 0, size, size));
    return pm;
}
} // namespace

QIcon icon(const QString &name, const QColor &color)
{
    QFile f(QStringLiteral(":/icons/lucide/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray src = f.readAll();
    const QPalette pal = QApplication::palette();
    const QColor normal = color.isValid() ? color : pal.color(QPalette::ButtonText);
    const QColor disabled = pal.color(QPalette::Disabled, QPalette::ButtonText);
    // Selected rows (mailbox tree, message list) draw QIcon::Selected: the
    // selection's text colour, so the icon stays visible on any Highlight
    // (e.g. a gold icon on the Boilermakers gold selection).
    const QColor selected = pal.color(QPalette::HighlightedText);
    QByteArray on = src, off = src, sel = src;
    on.replace("currentColor", normal.name().toLatin1());
    off.replace("currentColor", disabled.name().toLatin1());
    sel.replace("currentColor", selected.name().toLatin1());
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QIcon ic;
    for (int s : {16, 20, 24, 32, 48}) {
        ic.addPixmap(render(on, s, dpr), QIcon::Normal);
        ic.addPixmap(render(off, s, dpr), QIcon::Disabled);
        ic.addPixmap(render(sel, s, dpr), QIcon::Selected);
    }
    return ic;
}

QIcon appIcon()
{
    QIcon ic;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) {
        ic.addFile(QStringLiteral(":/icons/zmail-%1.png").arg(s), QSize(s, s));
    }
    return ic;
}

QIcon folderIcon(const QColor &color)
{
    QFile f(QStringLiteral(":/icons/lucide/folder.svg"));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray on = f.readAll();
    on.replace("fill=\"none\"", "fill=\"" + color.name().toLatin1() + "\"");
    QByteArray sel = on;
    on.replace("currentColor", color.name().toLatin1());
    sel.replace("currentColor", QApplication::palette().color(QPalette::HighlightedText).name().toLatin1());
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QIcon ic;
    for (int s : {16, 20, 24, 32, 48}) {
        ic.addPixmap(render(on, s, dpr), QIcon::Normal);
        ic.addPixmap(render(sel, s, dpr), QIcon::Selected);
    }
    return ic;
}

QIcon swatch(const QColor &color, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(color.darker(130));
    p.setBrush(color);
    p.drawRoundedRect(QRectF(1.5, 1.5, size - 3, size - 3), 2.5, 2.5);
    return QIcon(pm);
}

} // namespace zmail::ui
