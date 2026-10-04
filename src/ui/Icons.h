#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace zmail::ui {

// Theme-aware icon from the bundled Lucide set (:/icons/lucide/<name>.svg).
// The SVGs use stroke="currentColor"; we substitute the palette's button-text
// colour (or `color` if given) and render crisp pixmaps at common sizes, with
// a dimmed variant for disabled actions.
QIcon icon(const QString &name, const QColor &color = QColor());

// Solid rounded swatch used for label colours in lists and trees.
QIcon swatch(const QColor &color, int size = 12);

} // namespace zmail::ui
