#pragma once

#include <QtGlobal>

class QString;
class QTextDocument;

namespace zmail::ui::HtmlFit {

struct Options
{
    qreal availableWidth = 0; // viewport width in px the mail must fit
    qreal zoom = 1.0;         // 1.0 = 100 %
    bool dark = false;        // invert the lightness of the mail's own colours
};

// Text pass on sanitized mail HTML before setHtml(). QTextDocument draws
// <div> backgrounds only behind the div's own lines and ignores CSS
// display:table / table-cell, which email builders rely on. So:
//  - comments (incl. Outlook-only <!--[if mso]> scaffolding) are dropped,
//  - <div style="display:table"> + child "display:table-cell" divs become a
//    real one-row table, so side-by-side columns stay side by side,
//  - other <div>s with a background colour become a 100 %-wide one-cell
//    table carrying that colour, so the card/band shows behind its content.
//  - tables nested deeper than kMaxTableDepth are thinned (limitTableDepth).
// With a layout budget (in layoutCost units), tables are thinned further,
// a level at a time down to none, until the estimated cost fits; depthUsed
// reports the limit that was applied. A budget of 0 applies kMaxTableDepth
// only.
QString prepare(const QString &html, double layoutBudget = 0, int *depthUsed = nullptr);

// What MessageView allows a message before it simplifies the layout: about
// half a second of layout at the typical 20 microseconds a unit. Over a real
// mailbox of 558 HTML messages the slowest took 1.05 s and four were above
// this; a 60 x 60 table with no nesting at all took 4.8 s.
inline constexpr double kLayoutBudget = 30000;

// QTextDocument lays a nested table's cells out twice per level, so layout
// time doubles with every level of nesting, on the GUI thread, on every
// render. Builders nest 15-20 deep (wrapper inside wrapper inside wrapper)
// and the window stopped responding (0.5.5). Measured over a real mailbox,
// the slowest mail took 5.4 s to lay out with no limit, 2.9 s at 12 levels,
// 0.8 s at 10 and 0.3 s at 8, where one mail in thirteen changed, by a
// wrapper's padding.
inline constexpr int kMaxTableDepth = 8;

// Keep at most maxDepth levels of table nesting on any path, by turning
// tables into plain blocks (<div>s: cells stack). Only paths that are too
// deep are touched, and what matters least goes first: one-cell wrapper
// tables with no width and no background, then one-cell tables with a fixed
// width, then one-cell tables with a background, and only then the innermost
// tables with several cells.
QString limitTableDepth(const QString &html, int maxDepth = kMaxTableDepth);

// A rough count of the work QTextDocument's layout will do for this HTML.
// Each table costs its cells times its columns (wide grids are slow whatever
// their depth), doubled for every table it is nested in; each 256 characters
// of content cost one, doubled per level likewise. Measured at 7-40
// microseconds a unit, about 20 typically.
double layoutCost(const QString &html);

// Make an email laid out for a fixed-width web client usable in a resizable
// QTextDocument. Run after setHtml().
//  - Tables with no width (e.g. Unlayer/Mailchimp outer tables, whose real
//    widths sit inside Outlook-only <!--[if mso]> comments) become 100 % wide,
//    so nested width=100 % tables no longer collapse to their narrowest word.
//  - Fixed-width tables and columns wider than the pane are turned into
//    proportional percentages; narrower ones keep their size (scaled by zoom).
//  - Images are scaled by zoom and capped to the pane width, keeping aspect.
//  - Explicit font sizes are scaled by zoom.
void fit(QTextDocument *doc, const Options &opt);

} // namespace zmail::ui::HtmlFit
