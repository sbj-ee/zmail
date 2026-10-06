#pragma once

#include "core/Signatures.h"

#include <QDialog>

class QAction;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QTextCharFormat;
class QTextEdit;
class QToolButton;

namespace zmail::ui {
class SafeHtmlView;
}

// Settings → Signatures…: named signatures, edited as styled text (bold,
// italic, underline, font, size, colour, links) with a live preview of what
// recipients see and the generated plain-text version, and which one new
// messages start with. Stored as sanitized HTML (sanitizeSignatureHtml);
// a plain-only signature from before styled signatures stays exactly as it
// was until it's edited here.
class SignaturesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignaturesDialog(zmail::SignatureStore *store, QWidget *parent = nullptr);

    void accept() override;

    // Link on the selection (or on the link under the cursor). An empty
    // url removes the link; text replaces the selected text if different.
    // Only http(s): and mailto: are accepted; returns false otherwise.
    bool insertLink(const QString &text, const QString &url);
    // The Link button: a small dialog prefilled from the selection.
    void editLink();
    void setTextColor(const QColor &c);

    QTextEdit *editor() const { return m_rich; }
    zmail::ui::SafeHtmlView *preview() const { return m_preview; }
    QString plainPreview() const;
    QList<zmail::Signature> signatures() const { return m_sigs; }

private:
    void load(int row);
    void storeCurrent();
    void refreshDefaults();
    void refreshPreview();
    void syncToolbar(const QTextCharFormat &f);
    void mergeFormat(const QTextCharFormat &f);

    zmail::SignatureStore *m_store;
    QList<zmail::Signature> m_sigs;
    int m_row = -1;
    bool m_loading = false;
    bool m_edited = false; // the rich text was changed since load()
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QTextEdit *m_rich = nullptr;
    zmail::ui::SafeHtmlView *m_preview = nullptr;
    QPlainTextEdit *m_plain = nullptr;
    QLabel *m_plainLabel = nullptr;
    QComboBox *m_default = nullptr;
    QAction *m_bold = nullptr;
    QAction *m_italic = nullptr;
    QAction *m_underline = nullptr;
    QAction *m_color = nullptr;
    QAction *m_link = nullptr;
    QFontComboBox *m_font = nullptr;
    QComboBox *m_size = nullptr;
};
