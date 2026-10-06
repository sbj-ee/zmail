#pragma once

#include <QTextEdit>

namespace zmail::ui {

// The compose editor. A quoted reply/forward or pasted HTML can name local
// files (<img src="/etc/passwd">, file:, /dev/zero); QTextEdit would read
// them from disk. Like SafeHtmlView, this only renders data:image/* URIs and
// images already added to the document; everything else gets a 1x1
// transparent image, so QTextDocument never falls back to the disk.
class SafeTextEdit : public QTextEdit
{
    Q_OBJECT
public:
    using QTextEdit::QTextEdit;
    QVariant loadResource(int type, const QUrl &name) override;
    int blockedLoads() const { return m_blocked; }

private:
    int m_blocked = 0;
};

} // namespace zmail::ui
