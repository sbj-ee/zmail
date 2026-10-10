#include "HtmlFit.h"

#include "Theme.h"

#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QVariant>

#include <algorithm>
#include <cmath>
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
    int fixedCount = 0;
    for (const QTextLength &c : std::as_const(cols)) {
        if (c.type() == QTextLength::FixedLength) {
            fixedSum += c.rawValue();
            ++fixedCount;
        }
    }
    // Columns without a constraint (or past the end of the list) size to
    // their content.
    const int flexible = t->columns() - fixedCount;
    if (fixedSum > 0 && flexible <= 0) {
        // Every column has a pixel width: a designed grid. Too wide for the
        // pane, or stretched to a percentage: keep the proportions.
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
    } else if (fixedSum > 0) {
        // Spacer, icon and label columns (width="10", width="30") beside a
        // content column with no width. Turning them into percentages of
        // their own sum (the old rule) handed them 100% of the table and
        // squeezed the content column to one character per line. Keep them
        // in pixels and leave the content columns at least half the room.
        const QTextLength tw = f.width();
        const qreal tableWidth = tw.type() == QTextLength::FixedLength      ? tw.rawValue()
                                 : tw.type() == QTextLength::PercentageLength ? opt.availableWidth * tw.rawValue() / 100.0
                                                                              : opt.availableWidth;
        const qreal room = std::max<qreal>(0, tableWidth * 0.5);
        const qreal want = fixedSum * opt.zoom;
        const qreal shrink = want > room && want > 0 ? room / want : 1.0;
        for (QTextLength &c : cols) {
            if (c.type() == QTextLength::FixedLength) {
                c = QTextLength(QTextLength::FixedLength, c.rawValue() * opt.zoom * shrink);
            }
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
    // Compiled once per attribute name: this runs for every tag of a message.
    static QHash<QString, QRegularExpression> compiled;
    auto found = compiled.constFind(name);
    if (found == compiled.constEnd()) {
        found = compiled.insert(name, QRegularExpression(
                                          QStringLiteral("\\b%1\\s*=\\s*(\"([^\"]*)\"|'([^']*)'|([^\\s>]+))").arg(name),
                                          QRegularExpression::CaseInsensitiveOption));
    }
    const auto m = found->match(tag);
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

bool isHiddenStyle(const QString &style)
{
    if (style.isEmpty()) {
        return false;
    }
    if (lastStyleValue(style, QStringLiteral("display")).compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    // Preheaders and dark-mode twins: collapsed to nothing with overflow hidden.
    static const QRegularExpression zero(QStringLiteral("^0(px|em|%)?$"), QRegularExpression::CaseInsensitiveOption);
    const bool clipped =
        lastStyleValue(style, QStringLiteral("overflow")).compare(QLatin1String("hidden"), Qt::CaseInsensitive) == 0;
    return clipped && (zero.match(lastStyleValue(style, QStringLiteral("max-height"))).hasMatch() ||
                       zero.match(lastStyleValue(style, QStringLiteral("max-width"))).hasMatch());
}

int countOf(const QString &s, const QRegularExpression &re)
{
    int n = 0;
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

// QTextDocument ignores display:none, so hidden preheaders, mobile-only
// blocks and dark-mode duplicates (a second logo, a black status band) were
// drawn. Drop hidden elements, as mail clients do.
QString stripHidden(const QString &html)
{
    static const QRegularExpression cand(
        QStringLiteral("<([a-zA-Z][a-zA-Z0-9]*)\\b([^>]*\\b(?:display|max-height|max-width)\\s*:[^>]*)>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tableOpen(QStringLiteral("<table\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tableClose(QStringLiteral("</table\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression divOpen(QStringLiteral("<div\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression divClose(QStringLiteral("</div\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QStringList voidTags = {QStringLiteral("img"), QStringLiteral("br"), QStringLiteral("hr"),
                                         QStringLiteral("input"), QStringLiteral("wbr"), QStringLiteral("col")};
    QString out;
    out.reserve(html.size());
    qsizetype pos = 0;
    while (true) {
        const auto m = cand.match(html, pos);
        if (!m.hasMatch()) {
            break;
        }
        out += QStringView(html).mid(pos, m.capturedStart() - pos);
        pos = m.capturedEnd();
        const QString tag = m.captured(1).toLower();
        if (!isHiddenStyle(attr(m.captured(2), QStringLiteral("style")))) {
            out += m.captured(0);
            continue;
        }
        if (voidTags.contains(tag) || m.captured(0).endsWith(QLatin1String("/>"))) {
            continue; // drop the tag
        }
        // Find the matching close tag.
        const QRegularExpression same(QStringLiteral("<(/?)%1\\b[^>]*>").arg(tag), QRegularExpression::CaseInsensitiveOption);
        int depth = 1;
        qsizetype end = -1;
        auto it = same.globalMatch(html, pos);
        while (it.hasNext()) {
            const auto t = it.next();
            if (t.capturedLength(1) > 0) {
                if (--depth == 0) {
                    end = t.capturedEnd();
                    break;
                }
            } else if (!t.captured(0).endsWith(QLatin1String("/>"))) {
                ++depth;
            }
        }
        if (end < 0) {
            out += m.captured(0); // unclosed: leave it alone
            continue;
        }
        // Only drop a range that closes what it opens (an unclosed <td> must
        // not swallow the rest of its table).
        const QString inner = html.mid(m.capturedStart(), end - m.capturedStart());
        if (countOf(inner, tableOpen) != countOf(inner, tableClose) || countOf(inner, divOpen) != countOf(inner, divClose)) {
            out += m.captured(0);
            continue;
        }
        pos = end;
    }
    out += QStringView(html).mid(pos);
    return out;
}

// A table built as width:100%; max-width:600px is a 600 px card centred in
// a wide window (Gmail draws it so). QTextDocument has no max-width and
// stretched it to the pane. Pin it to its max width; fit() still shrinks it
// to a percentage when the pane is narrower.
QString pinMaxWidthTables(const QString &html)
{
    static const QRegularExpression table(QStringLiteral("<table\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression px(QStringLiteral("^(\\d+(?:\\.\\d+)?)px$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression widthAttr(QStringLiteral("\\swidth\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
                                              QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    auto it = table.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        QString tag = m.captured(0);
        const QString style = attr(tag, QStringLiteral("style"));
        const auto mw = px.match(lastStyleValue(style, QStringLiteral("max-width")));
        const QString w = lastStyleValue(style, QStringLiteral("width"));
        if (!mw.hasMatch() || mw.captured(1).toDouble() < 200 || (!w.isEmpty() && !w.endsWith(QLatin1Char('%')))) {
            out += tag;
            continue;
        }
        const QString n = QString::number(qRound(mw.captured(1).toDouble()));
        tag.remove(widthAttr);
        // Last declaration wins; the attribute covers parsers that prefer it.
        QString newStyle = style.trimmed();
        while (newStyle.endsWith(QLatin1Char(';'))) {
            newStyle.chop(1); // ";;" makes Qt's CSS parser drop the whole style
        }
        newStyle += QStringLiteral("; width:%1px").arg(n);
        tag.replace(style, newStyle);
        tag.insert(6, QStringLiteral(" width=\"%1\"").arg(n));
        out += tag;
    }
    out += QStringView(html).mid(last);
    return out;
}

// "margin: 0 auto" centres a block in a browser; QTextDocument reads "auto"
// as a length and pushed the content right by ~40 px per level (off the
// edge of its card). Use 0 instead, and centre tables with align="center".
QString neutraliseAutoMargins(const QString &html)
{
    static const QRegularExpression tag(QStringLiteral("<([a-zA-Z][a-zA-Z0-9]*)\\b[^>]*\\bmargin[^>]*>"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression decl(QStringLiteral("(margin(?:-left|-right)?\\s*:)([^;]*)"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression autoWord(QStringLiteral("\\bauto\\b"), QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    auto it = tag.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        QString t = m.captured(0);
        const QString style = attr(t, QStringLiteral("style"));
        if (style.isEmpty() || !style.contains(autoWord)) {
            out += t;
            continue;
        }
        QString fixedStyle;
        bool centred = false;
        qsizetype pos = 0;
        auto di = decl.globalMatch(style);
        while (di.hasNext()) {
            const auto d = di.next();
            fixedStyle += QStringView(style).mid(pos, d.capturedStart() - pos);
            QString v = d.captured(2);
            if (v.contains(autoWord)) {
                centred = true;
                v.replace(autoWord, QStringLiteral("0"));
            }
            fixedStyle += d.captured(1) + v;
            pos = d.capturedEnd();
        }
        fixedStyle += QStringView(style).mid(pos);
        if (centred) {
            t.replace(style, fixedStyle);
            if (m.captured(1).compare(QLatin1String("table"), Qt::CaseInsensitive) == 0 &&
                attr(t, QStringLiteral("align")).isEmpty()) {
                t.insert(6, QStringLiteral(" align=\"center\""));
            }
        }
        out += t;
    }
    out += QStringView(html).mid(last);
    return out;
}

} // namespace

QString prepare(const QString &input, double layoutBudget, int *depthUsed)
{
    static const QRegularExpression comments(QStringLiteral("<!--.*?-->"),
                                             QRegularExpression::DotMatchesEverythingOption);
    QString html = input;
    html.remove(comments);
    html = stripHidden(html);
    html = pinMaxWidthTables(html);
    html = neutraliseAutoMargins(html);

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
    int depth = kMaxTableDepth;
    QString limited = limitTableDepth(linked, depth);
    // Nesting is not the only way to be slow: a wide grid costs its cells
    // times its columns at any depth. Over budget, give up a level of tables
    // at a time, down to none (plain blocks lay out in linear time).
    if (layoutBudget > 0 && layoutCost(limited) > layoutBudget) {
        // Start at the nesting the message really has: limits above that change nothing.
        static const QRegularExpression table(QStringLiteral("<(/?)table\\b"), QRegularExpression::CaseInsensitiveOption);
        int nesting = 0, deepest = 0;
        for (auto t = table.globalMatch(limited); t.hasNext();) {
            nesting = std::max(0, nesting + (t.next().capturedLength(1) > 0 ? -1 : 1));
            deepest = std::max(deepest, nesting);
        }
        // No tables at all: just a long message, laid out in linear time.
        if (deepest > 0) {
            depth = std::min(depth, deepest);
            do {
                limited = limitTableDepth(linked, --depth);
            } while (depth > 0 && layoutCost(limited) > layoutBudget);
        }
    }
    if (depthUsed) {
        *depthUsed = depth;
    }
    return limited;
}

double layoutCost(const QString &html)
{
    static const QRegularExpression tag(QStringLiteral("<(/?)(table|tr|td|th)\\b[^>]*>"),
                                        QRegularExpression::CaseInsensitiveOption);
    struct Open { int cells = 0; int rowCells = 0; int columns = 0; };
    QList<Open> open;
    double cost = 0;
    qsizetype last = 0;
    // A table d levels down is laid out 2^(d-1) times; what is in its cells, 2^d.
    const auto times = [](qsizetype depth) { return std::ldexp(1.0, int(std::min<qsizetype>(depth, 40))); };
    const auto close = [&]() {
        const Open t = open.takeLast();
        cost += double(t.cells) * std::max(1, std::max(t.columns, t.rowCells)) * times(open.size());
    };
    auto it = tag.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        cost += double(m.capturedStart() - last) / 256.0 * times(open.size());
        last = m.capturedEnd();
        const bool closing = m.capturedLength(1) > 0;
        const qsizetype name = m.capturedLength(2);
        if (name == 5) { // table
            if (!closing) {
                open.append(Open());
            } else if (!open.isEmpty()) {
                close();
            }
        } else if (!open.isEmpty() && !closing) {
            Open &t = open.last();
            if (m.capturedView(2).compare(QLatin1String("tr"), Qt::CaseInsensitive) == 0) {
                t.columns = std::max(t.columns, t.rowCells);
                t.rowCells = 0;
            } else {
                ++t.cells;
                ++t.rowCells;
            }
        }
    }
    while (!open.isEmpty()) {
        close(); // never closed: the parser closes them at the end
    }
    cost += double(html.size() - last) / 256.0;
    return cost;
}

QString limitTableDepth(const QString &html, int maxDepth)
{
    static const QRegularExpression tag(QStringLiteral("<(/?)(table|tbody|thead|tfoot|tr|td|th)\\b([^>]*)>"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression pixels(QStringLiteral("^\\d+(?:\\.\\d+)?(?:px)?$"),
                                           QRegularExpression::CaseInsensitiveOption);
    enum Part { TableOpen, TableClose, Row, CellOpen, CellClose };
    enum Kind { Plain, FixedWidth, Coloured, Grid }; // the order tables are given up in
    struct Piece { qsizetype start; qsizetype end; int table; Part part; QString align; };
    struct Table {
        int parent = -1;
        int cells = 0;
        bool fixedWidth = false;
        bool coloured = false;
        bool flat = false;
        bool cellOpen = false; // while writing
        int depth = 0;         // tables kept above this one
        int height = 0;        // tables kept on the deepest path from here down, this one included
        Kind kind() const { return cells > 1 ? Grid : coloured ? Coloured : fixedWidth ? FixedWidth : Plain; }
    };
    const auto hasColour = [](const QString &attrs) {
        const QString style = attr(attrs, QStringLiteral("style"));
        return isRealColour(lastStyleValue(style, QStringLiteral("background-color"))) ||
               isRealColour(lastStyleValue(style, QStringLiteral("background"))) ||
               isRealColour(attr(attrs, QStringLiteral("bgcolor")));
    };

    QList<Piece> pieces;
    QList<Table> tables;
    QList<int> open;
    int deepest = 0;
    auto it = tag.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const bool closing = m.capturedLength(1) > 0;
        const QString name = m.captured(2).toLower();
        const QString attrs = m.captured(3);
        Piece p{m.capturedStart(), m.capturedEnd(), -1, Row, {}};
        if (name == QLatin1String("table")) {
            if (closing) {
                if (open.isEmpty()) {
                    continue; // stray </table>: leave it to the parser
                }
                p.table = open.takeLast();
                p.part = TableClose;
            } else {
                Table t;
                t.parent = open.isEmpty() ? -1 : open.last();
                const QString style = attr(attrs, QStringLiteral("style"));
                t.fixedWidth = pixels.match(attr(attrs, QStringLiteral("width"))).hasMatch() ||
                               pixels.match(lastStyleValue(style, QStringLiteral("width"))).hasMatch();
                t.coloured = hasColour(attrs);
                p.table = int(tables.size());
                p.part = TableOpen;
                tables.append(t);
                open.append(p.table);
                deepest = std::max(deepest, int(open.size()));
            }
        } else if (open.isEmpty()) {
            continue;
        } else {
            p.table = open.last();
            if (name == QLatin1String("td") || name == QLatin1String("th")) {
                p.part = closing ? CellClose : CellOpen;
                if (!closing) {
                    Table &t = tables[p.table];
                    ++t.cells;
                    t.coloured = t.coloured || hasColour(attrs);
                    p.align = attr(attrs, QStringLiteral("align")).toLower();
                    if (p.align.isEmpty()) {
                        p.align = lastStyleValue(attr(attrs, QStringLiteral("style")), QStringLiteral("text-align")).toLower();
                    }
                }
            }
        }
        pieces.append(p);
    }
    if (deepest <= maxDepth) {
        return html;
    }

    // Tables are in document order, so a parent always comes before its
    // children: depths go down the list, heights come back up it.
    const auto measure = [&tables]() {
        for (Table &t : tables) {
            t.height = t.flat ? 0 : 1;
        }
        int tallest = 0;
        for (int i = int(tables.size()) - 1; i >= 0; --i) {
            const Table &t = tables.at(i);
            if (t.parent >= 0) {
                Table &parent = tables[t.parent];
                parent.height = std::max(parent.height, (parent.flat ? 0 : 1) + t.height);
            } else {
                tallest = std::max(tallest, t.height);
            }
        }
        return tallest;
    };
    for (Kind kind : {Plain, FixedWidth, Coloured, Grid}) {
        if (measure() <= maxDepth) {
            break;
        }
        for (Table &t : tables) {
            if (t.parent >= 0) {
                const Table &parent = tables.at(t.parent);
                t.depth = parent.depth + (parent.flat ? 0 : 1);
            }
            if (t.flat) {
                continue;
            }
            // One-cell tables: the outermost on a path that is too deep.
            // Grids: only the ones past the limit, so the innermost.
            if (kind == Grid ? t.depth >= maxDepth : t.kind() == kind && t.depth + t.height > maxDepth) {
                t.flat = true;
            }
        }
    }

    static const QStringList alignments = {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")};
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    for (const Piece &p : std::as_const(pieces)) {
        Table &t = tables[p.table];
        if (!t.flat) {
            continue;
        }
        out += QStringView(html).mid(last, p.start - last);
        last = p.end;
        if (t.cellOpen && p.part != CellClose) {
            out += QStringLiteral("</div>"); // a cell with no </td>
            t.cellOpen = false;
        }
        switch (p.part) {
        case TableOpen: out += QStringLiteral("<div>"); break;
        case TableClose: out += QStringLiteral("</div>"); break;
        case Row: break;
        case CellOpen:
            out += alignments.contains(p.align) ? QStringLiteral("<div align=\"%1\">").arg(p.align) : QStringLiteral("<div>");
            t.cellOpen = true;
            break;
        case CellClose:
            if (t.cellOpen) {
                out += QStringLiteral("</div>");
                t.cellOpen = false;
            }
            break;
        }
    }
    out += QStringView(html).mid(last);
    return out;
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
    QList<std::pair<int, int>> emojiRuns;
    const QString emoji = emojiFamily();
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
            if (!emoji.isEmpty()) {
                // Mail fonts (Arial, 'Work Sans' ...) have no emoji and Qt 6.4
                // doesn't fall back to the colour font: point emoji at it.
                const QString text = frag.text();
                const auto ucs = text.toUcs4();
                int offset = 0, runStart = -1;
                for (int i = 0; i < ucs.size(); ++i) {
                    const char32_t c = ucs[i];
                    const bool e = isEmojiCodePoint(c, i + 1 < ucs.size() ? ucs[i + 1] : 0);
                    if (e && runStart < 0) {
                        runStart = offset;
                    } else if (!e && runStart >= 0) {
                        emojiRuns.append({frag.position() + runStart, offset - runStart});
                        runStart = -1;
                    }
                    offset += c > 0xFFFF ? 2 : 1;
                }
                if (runStart >= 0) {
                    emojiRuns.append({frag.position() + runStart, offset - runStart});
                }
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
                        // Only a height given (height="58"): the width that
                        // keeps its shape, not its full natural width.
                        w = h > 0 && natural.height() > 0 ? h * natural.width() / natural.height() : natural.width();
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
    if (!emojiRuns.isEmpty()) {
        QTextCharFormat ef;
        ef.setFontFamilies(QStringList{emoji});
        for (const auto &run : std::as_const(emojiRuns)) {
            QTextCursor cur(doc);
            cur.setPosition(run.first);
            cur.setPosition(run.first + run.second, QTextCursor::KeepAnchor);
            cur.mergeCharFormat(ef);
        }
    }
    for (const auto &bc : std::as_const(blockChanges)) {
        QTextCursor cur(bc.first);
        cur.setBlockFormat(bc.second);
    }
    edit.endEditBlock();
}

} // namespace zmail::ui::HtmlFit
