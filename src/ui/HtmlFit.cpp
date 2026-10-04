#include "HtmlFit.h"

#include <QImage>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QVariant>

#include <utility>

namespace zmail::ui::HtmlFit {

namespace {

QColor invertLightness(const QColor &c)
{
    if (!c.isValid() || c.alpha() == 0) {
        return c;
    }
    QColor hsl = c.toHsl();
    // Map lightness l -> 1 - l, but keep a little distance from pure black
    // so dark-mode cards read as surfaces rather than holes.
    const qreal l = hsl.lightnessF();
    const qreal nl = 0.12 + (1.0 - l) * 0.80;
    return QColor::fromHslF(hsl.hslHueF() < 0 ? 0 : hsl.hslHueF(), hsl.hslSaturationF(), nl, hsl.alphaF());
}

void darkenBrushProperty(QTextFormat &f, int property)
{
    if (!f.hasProperty(property)) {
        return;
    }
    QBrush b = f.brushProperty(property);
    if (b.style() == Qt::NoBrush) {
        return;
    }
    b.setColor(invertLightness(b.color()));
    f.setProperty(property, b);
}

QTextLength scaled(const QTextLength &l, qreal zoom)
{
    if (l.type() == QTextLength::FixedLength) {
        return QTextLength(QTextLength::FixedLength, l.rawValue() * zoom);
    }
    return l;
}

void fitTable(QTextTable *t, const Options &opt)
{
    QTextTableFormat f = t->format();
    const QTextLength w = f.width();
    if (w.type() == QTextLength::VariableLength) {
        f.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    } else if (w.type() == QTextLength::FixedLength) {
        if (w.rawValue() * opt.zoom > opt.availableWidth) {
            f.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        } else {
            f.setWidth(scaled(w, opt.zoom));
        }
    }
    QList<QTextLength> cols = f.columnWidthConstraints();
    qreal fixedSum = 0;
    for (const QTextLength &c : std::as_const(cols)) {
        if (c.type() == QTextLength::FixedLength) {
            fixedSum += c.rawValue();
        }
    }
    if (fixedSum > 0) {
        const bool tooWide = fixedSum * opt.zoom > opt.availableWidth ||
                             f.width().type() == QTextLength::PercentageLength;
        for (QTextLength &c : cols) {
            if (c.type() != QTextLength::FixedLength) {
                continue;
            }
            c = tooWide ? QTextLength(QTextLength::PercentageLength, c.rawValue() * 100.0 / fixedSum)
                        : scaled(c, opt.zoom);
        }
        f.setColumnWidthConstraints(cols);
    }
    if (opt.zoom != 1.0) {
        f.setCellPadding(f.cellPadding() * opt.zoom);
        f.setCellSpacing(f.cellSpacing() * opt.zoom);
    }
    if (opt.dark) {
        darkenBrushProperty(f, QTextFormat::BackgroundBrush);
        darkenBrushProperty(f, QTextFormat::FrameBorderBrush);
    }
    t->setFormat(f);
    if (opt.dark) {
        for (int r = 0; r < t->rows(); ++r) {
            for (int c = 0; c < t->columns(); ++c) {
                QTextTableCell cell = t->cellAt(r, c);
                if (!cell.isValid() || cell.row() != r || cell.column() != c) {
                    continue;
                }
                QTextCharFormat cf = cell.format();
                if (cf.hasProperty(QTextFormat::BackgroundBrush)) {
                    darkenBrushProperty(cf, QTextFormat::BackgroundBrush);
                    cell.setFormat(cf);
                }
            }
        }
    }
}

void walkFrames(QTextFrame *frame, const Options &opt)
{
    const auto children = frame->childFrames();
    for (QTextFrame *child : children) {
        if (auto *t = qobject_cast<QTextTable *>(child)) {
            fitTable(t, opt);
        } else if (opt.dark) {
            QTextFrameFormat ff = child->frameFormat();
            darkenBrushProperty(ff, QTextFormat::BackgroundBrush);
            child->setFrameFormat(ff);
        }
        walkFrames(child, opt);
    }
}

QString styleValue(const QString &style, const QString &prop)
{
    const QRegularExpression re(QStringLiteral("(?:^|;)\\s*%1\\s*:\\s*([^;]+)").arg(prop),
                                QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(style);
    if (!m.hasMatch()) {
        return {};
    }
    QString v = m.captured(1).trimmed();
    v.remove(QStringLiteral("!important"), Qt::CaseInsensitive);
    return v.trimmed();
}

QString lastStyleValue(const QString &style, const QString &prop)
{
    // Builders sometimes repeat a property ("background-color:#00a0bd;;background-color:#11365c"):
    // the last one wins, as in CSS.
    QString v;
    const QStringList decls = style.split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0) {
            v = d.mid(colon + 1).trimmed();
        }
    }
    v.remove(QStringLiteral("!important"), Qt::CaseInsensitive);
    return v.trimmed();
}

bool isRealColour(const QString &v)
{
    if (v.isEmpty()) {
        return false;
    }
    const QString l = v.toLower();
    if (l == QLatin1String("transparent") || l == QLatin1String("none") || l == QLatin1String("inherit") ||
        l == QLatin1String("initial")) {
        return false;
    }
    return QColor::isValidColorName(v) || l.startsWith(QLatin1String("rgb"));
}

QString attr(const QString &tag, const QString &name)
{
    const QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s>]+))").arg(name),
                                QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(tag);
    if (!m.hasMatch()) {
        return {};
    }
    for (int i = 2; i <= 4; ++i) {
        if (m.capturedLength(i) > 0) {
            return m.captured(i);
        }
    }
    return {};
}

} // namespace

QString prepare(const QString &input)
{
    static const QRegularExpression comments(QStringLiteral("<!--.*?-->"),
                                             QRegularExpression::DotMatchesEverythingOption);
    QString html = input;
    html.remove(comments);

    static const QRegularExpression divTag(QStringLiteral("<(/?)div\\b([^>]*)>"),
                                           QRegularExpression::CaseInsensitiveOption);
    enum Kind { Plain, Table, Cell, BgWrap };
    QList<Kind> stack;
    QString out;
    out.reserve(html.size() + html.size() / 8);
    qsizetype last = 0;
    auto it = divTag.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        if (m.capturedLength(1) > 0) { // </div>
            const Kind k = stack.isEmpty() ? Plain : stack.takeLast();
            switch (k) {
            case Plain: out += QStringLiteral("</div>"); break;
            case Table: out += QStringLiteral("</tr></table>"); break;
            case Cell: out += QStringLiteral("</td>"); break;
            case BgWrap: out += QStringLiteral("</td></tr></table>"); break;
            }
            continue;
        }
        const QString attrs = m.captured(2);
        const QString style = attr(attrs, QStringLiteral("style"));
        const QString display = styleValue(style, QStringLiteral("display")).toLower();
        QString bg = lastStyleValue(style, QStringLiteral("background-color"));
        if (!isRealColour(bg)) {
            bg = attr(attrs, QStringLiteral("bgcolor"));
            if (!isRealColour(bg)) {
                bg.clear();
            }
        }
        const QString bgStyle = bg.isEmpty() ? QString() : QStringLiteral(" style=\"background-color:%1\"").arg(bg);
        const QString align = attr(attrs, QStringLiteral("align"));
        const QString alignAttr = align.isEmpty() ? QString() : QStringLiteral(" align=\"%1\"").arg(align.toHtmlEscaped());
        if (display == QLatin1String("table")) {
            out += QStringLiteral("<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"%1><tr>").arg(bgStyle);
            stack.append(Table);
        } else if (display == QLatin1String("table-cell") && !stack.isEmpty() && stack.last() == Table) {
            out += QStringLiteral("<td valign=\"top\"%1%2>").arg(bgStyle, alignAttr);
            stack.append(Cell);
        } else if (!bg.isEmpty()) {
            const QString pad = styleValue(style, QStringLiteral("padding"));
            QString cellStyle;
            if (!pad.isEmpty()) {
                cellStyle = QStringLiteral(" style=\"padding:%1\"").arg(pad.toHtmlEscaped());
            }
            out += QStringLiteral("<table width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\"%1><tr><td%2%3>")
                       .arg(bgStyle, alignAttr, cellStyle);
            stack.append(BgWrap);
        } else {
            out += m.captured(0);
            stack.append(Plain);
        }
    }
    out += QStringView(html).mid(last);

    // A link inside <span style="color:X"> keeps the span's colour in mail
    // clients; QTextDocument paints it in the palette's link blue instead
    // (blue on a navy band is unreadable). Copy the colour onto the <a>.
    static const QRegularExpression colouredLink(
        QStringLiteral("(<(?:span|font|p|strong|b)\\b[^>]*?(?:color\\s*:\\s*|\\bcolor\\s*=\\s*[\"']?)(#[0-9a-fA-F]{3,8}|[a-zA-Z]+|rgba?\\([^)]*\\))[^>]*>\\s*(?:<(?:strong|b|span)\\b[^>]*>\\s*)*)<a\\b(?![^>]*\\bstyle\\s*=\\s*[\"'][^\"']*color)"),
        QRegularExpression::CaseInsensitiveOption);
    QString linked;
    linked.reserve(out.size());
    qsizetype pos = 0;
    auto li = colouredLink.globalMatch(out);
    while (li.hasNext()) {
        const auto m = li.next();
        linked += QStringView(out).mid(pos, m.capturedStart() - pos);
        linked += m.captured(1);
        linked += QStringLiteral("<a style=\"color:%1\"").arg(m.captured(2));
        pos = m.capturedEnd();
    }
    linked += QStringView(out).mid(pos);
    return linked;
}

void fit(QTextDocument *doc, const Options &opt)
{
    if (!doc) {
        return;
    }
    const qreal avail = std::max<qreal>(80, opt.availableWidth);
    Options o = opt;
    o.availableWidth = avail;

    QTextCursor edit(doc);
    edit.beginEditBlock();
    walkFrames(doc->rootFrame(), o);

    // Character-level pass: font sizes, images, colours. Collect first, apply
    // after, since setCharFormat() splits fragments under the iterator.
    struct Change { int pos; int len; QTextCharFormat fmt; };
    QList<Change> changes;
    QList<std::pair<QTextBlock, QTextBlockFormat>> blockChanges;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (o.dark && b.blockFormat().hasProperty(QTextFormat::BackgroundBrush)) {
            QTextBlockFormat bf = b.blockFormat();
            darkenBrushProperty(bf, QTextFormat::BackgroundBrush);
            blockChanges.append({b, bf});
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) {
                continue;
            }
            QTextCharFormat f = frag.charFormat();
            bool changed = false;
            if (f.isImageFormat()) {
                QTextImageFormat img = f.toImageFormat();
                qreal w = img.width();
                qreal h = img.height();
                if (w <= 0) {
                    const QVariant res = doc->resource(QTextDocument::ImageResource, QUrl(img.name()));
                    const QImage natural = res.value<QImage>();
                    if (!natural.isNull()) {
                        w = natural.width();
                        h = h > 0 ? h : natural.height();
                    }
                } else if (h <= 0) {
                    const QImage natural =
                        doc->resource(QTextDocument::ImageResource, QUrl(img.name())).value<QImage>();
                    if (!natural.isNull() && natural.width() > 0) {
                        h = w * natural.height() / natural.width();
                    }
                }
                if (w > 0) {
                    qreal nw = w * o.zoom;
                    qreal nh = h * o.zoom;
                    const qreal cap = avail - 8;
                    if (nw > cap) {
                        nh = nh * cap / nw;
                        nw = cap;
                    }
                    img.setWidth(nw);
                    if (nh > 0) {
                        img.setHeight(nh);
                    }
                    f = img;
                    changed = true;
                }
            }
            if (o.zoom != 1.0) {
                if (f.hasProperty(QTextFormat::FontPixelSize)) {
                    const int px = f.intProperty(QTextFormat::FontPixelSize);
                    if (px > 0) {
                        f.setProperty(QTextFormat::FontPixelSize, std::max(1, qRound(px * o.zoom)));
                        changed = true;
                    }
                }
                if (f.hasProperty(QTextFormat::FontPointSize)) {
                    const qreal pt = f.doubleProperty(QTextFormat::FontPointSize);
                    if (pt > 0) {
                        f.setFontPointSize(pt * o.zoom);
                        changed = true;
                    }
                }
            }
            if (o.dark) {
                if (f.hasProperty(QTextFormat::ForegroundBrush)) {
                    darkenBrushProperty(f, QTextFormat::ForegroundBrush);
                    changed = true;
                }
                if (f.hasProperty(QTextFormat::BackgroundBrush)) {
                    darkenBrushProperty(f, QTextFormat::BackgroundBrush);
                    changed = true;
                }
            }
            if (changed) {
                changes.append({frag.position(), frag.length(), f});
            }
        }
    }
    for (const Change &c : std::as_const(changes)) {
        QTextCursor cur(doc);
        cur.setPosition(c.pos);
        cur.setPosition(c.pos + c.len, QTextCursor::KeepAnchor);
        cur.setCharFormat(c.fmt);
    }
    for (const auto &bc : std::as_const(blockChanges)) {
        QTextCursor cur(bc.first);
        cur.setBlockFormat(bc.second);
    }
    edit.endEditBlock();
}

} // namespace zmail::ui::HtmlFit
