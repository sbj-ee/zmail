#include "SafeTextEdit.h"

#include "core/HtmlSanitizer.h"

#include <QPixmap>
#include <QTextDocument>

namespace zmail::ui {

QVariant SafeTextEdit::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && name.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) == 0) {
        return QPixmap::fromImage(html::dataImage(name)); // a pixmap: see SafeHtmlView::loadResource
    }
    ++m_blocked;
    return QPixmap::fromImage(html::blockedResource());
}

} // namespace zmail::ui
