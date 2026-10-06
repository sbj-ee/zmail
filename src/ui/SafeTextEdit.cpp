#include "SafeTextEdit.h"

#include "core/HtmlSanitizer.h"

#include <QTextDocument>

namespace zmail::ui {

QVariant SafeTextEdit::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && name.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) == 0) {
        return html::dataImage(name);
    }
    ++m_blocked;
    return html::blockedResource();
}

} // namespace zmail::ui
