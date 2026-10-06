#include "SignaturesDialog.h"

#include "Icons.h"
#include "SafeHtmlView.h"
#include "SafeTextEdit.h"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

using namespace zmail;

namespace {
// The signature editor: pasted HTML goes through the signature sanitizer
// (no images, remote CSS or odd links even before saving); images and
// other pasted data are ignored.
class SignatureEdit : public zmail::ui::SafeTextEdit
{
public:
    using SafeTextEdit::SafeTextEdit;

protected:
    void insertFromMimeData(const QMimeData *src) override
    {
        if (src->hasHtml()) {
            textCursor().insertHtml(sanitizeSignatureHtml(src->html()));
        } else if (src->hasText()) {
            textCursor().insertText(src->text());
        }
    }
};

bool linkAllowed(const QUrl &u)
{
    if (!u.isValid()) {
        return false;
    }
    const QString s = u.scheme().toLower();
    if (s == QLatin1String("mailto")) {
        return !u.path().isEmpty();
    }
    return (s == QLatin1String("https") || s == QLatin1String("http")) && !u.host().isEmpty();
}

QIcon swatch(const QColor &c)
{
    QPixmap p(16, 16);
    p.fill(Qt::transparent);
    QPainter g(&p);
    g.setPen(QPen(QApplication::palette().color(QPalette::Mid)));
    g.setBrush(c);
    g.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 2, 2);
    return QIcon(p);
}

// The anchor around `pos`: [start, end) of the fragments with that href.
QPair<int, int> anchorRange(QTextDocument *doc, int pos, const QString &href)
{
    int start = pos, end = pos;
    const QTextBlock b = doc->findBlock(pos);
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.charFormat().anchorHref() != href) {
            continue;
        }
        if (f.position() <= pos && pos <= f.position() + f.length()) {
            start = std::min(start, f.position());
            end = std::max(end, f.position() + f.length());
        }
    }
    return {start, end};
}
} // namespace

SignaturesDialog::SignaturesDialog(SignatureStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_sigs(store->all())
{
    setObjectName(QStringLiteral("signaturesDialog"));
    setWindowTitle(tr("Signatures"));
    resize(900, 600);
    auto *root = new QHBoxLayout(this);

    auto *left = new QVBoxLayout;
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("signatureList"));
    left->addWidget(m_list);
    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("New"), this);
    add->setObjectName(QStringLiteral("newSignatureButton"));
    auto *del = new QPushButton(tr("Delete"), this);
    del->setObjectName(QStringLiteral("deleteSignatureButton"));
    btns->addWidget(add);
    btns->addWidget(del);
    left->addLayout(btns);
    root->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("signatureName"));
    form->addRow(tr("Name:"), m_name);
    right->addLayout(form);

    // Toolbar: bold, italic, underline, font, size, colour, link.
    auto *tb = new QToolBar(this);
    tb->setObjectName(QStringLiteral("signatureToolBar"));
    tb->setIconSize(QSize(16, 16));
    auto act = [&](const char *obj, const char *ic, const QString &tip, bool checkable) {
        QAction *a = tb->addAction(zmail::ui::icon(QString::fromLatin1(ic)), tip);
        a->setObjectName(QString::fromLatin1(obj));
        a->setCheckable(checkable);
        return a;
    };
    m_bold = act("signatureBold", "bold", tr("Bold"), true);
    m_italic = act("signatureItalic", "italic", tr("Italic"), true);
    m_underline = act("signatureUnderline", "underline", tr("Underline"), true);
    tb->addSeparator();
    m_font = new QFontComboBox(tb);
    m_font->setObjectName(QStringLiteral("signatureFont"));
    m_font->setMaximumWidth(190);
    tb->addWidget(m_font);
    m_size = new QComboBox(tb);
    m_size->setObjectName(QStringLiteral("signatureSize"));
    m_size->addItems({"8", "9", "10", "11", "12", "14", "16", "18", "20", "24"});
    m_size->setCurrentText(QStringLiteral("11"));
    tb->addWidget(m_size);
    tb->addSeparator();
    m_color = tb->addAction(swatch(Qt::black), tr("Text colour"));
    m_color->setObjectName(QStringLiteral("signatureColor"));
    m_link = act("signatureLink", "link", tr("Insert or edit link"), false);
    right->addWidget(tb);

    auto *split = new QSplitter(Qt::Vertical, this);
    auto *editBox = new QWidget(split);
    auto *el = new QVBoxLayout(editBox);
    el->setContentsMargins(0, 0, 0, 0);
    m_rich = new SignatureEdit(editBox);
    m_rich->setObjectName(QStringLiteral("signatureEditor"));
    m_rich->setAcceptRichText(true);
    el->addWidget(m_rich);

    auto *prevBox = new QWidget(split);
    auto *pl = new QVBoxLayout(prevBox);
    pl->setContentsMargins(0, 4, 0, 0);
    pl->addWidget(new QLabel(tr("Preview (as it appears in a message):"), prevBox));
    m_preview = new zmail::ui::SafeHtmlView(prevBox);
    m_preview->setObjectName(QStringLiteral("signaturePreview"));
    m_preview->setFrameShape(QFrame::StyledPanel);
    {
        // Mail is drawn on white whatever the app theme, as in the viewer.
        QPalette p = m_preview->palette();
        p.setColor(QPalette::Base, Qt::white);
        p.setColor(QPalette::Text, Qt::black);
        m_preview->setPalette(p);
    }
    pl->addWidget(m_preview, 2);
    m_plainLabel = new QLabel(tr("Plain-text version (generated; sent with every HTML message):"), prevBox);
    pl->addWidget(m_plainLabel);
    m_plain = new QPlainTextEdit(prevBox);
    m_plain->setObjectName(QStringLiteral("signaturePlain"));
    m_plain->setReadOnly(true);
    pl->addWidget(m_plain, 1);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 3);
    right->addWidget(split, 1);

    auto *defRow = new QFormLayout;
    m_default = new QComboBox(this);
    m_default->setObjectName(QStringLiteral("signatureDefault"));
    defRow->addRow(tr("New messages start with:"), m_default);
    right->addLayout(defRow);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    right->addWidget(bb);
    root->addLayout(right, 3);

    for (const auto &s : m_sigs) {
        m_list->addItem(s.name);
    }
    refreshDefaults();
    m_default->setCurrentIndex(std::max(0, m_default->findText(m_store->defaultName())));

    connect(m_bold, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat f;
        f.setFontWeight(on ? QFont::Bold : QFont::Normal);
        mergeFormat(f);
    });
    connect(m_italic, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat f;
        f.setFontItalic(on);
        mergeFormat(f);
    });
    connect(m_underline, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat f;
        f.setFontUnderline(on);
        mergeFormat(f);
    });
    connect(m_font, &QFontComboBox::textActivated, this, [this](const QString &family) {
        QTextCharFormat f;
        f.setFontFamilies({family});
        mergeFormat(f);
    });
    connect(m_size, &QComboBox::textActivated, this, [this](const QString &s) {
        const double pt = s.toDouble();
        if (pt > 0) {
            QTextCharFormat f;
            f.setFontPointSize(pt);
            mergeFormat(f);
        }
    });
    connect(m_color, &QAction::triggered, this, [this] {
        const QColor c = QColorDialog::getColor(m_rich->textColor(), this, tr("Text colour"));
        if (c.isValid()) {
            setTextColor(c);
        }
    });
    connect(m_link, &QAction::triggered, this, &SignaturesDialog::editLink);
    connect(m_rich, &QTextEdit::currentCharFormatChanged, this, &SignaturesDialog::syncToolbar);
    connect(m_rich, &QTextEdit::textChanged, this, [this] {
        if (!m_loading) {
            m_edited = true;
            refreshPreview();
        }
    });

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        storeCurrent();
        load(row);
    });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_row >= 0) {
            m_list->item(m_row)->setText(t);
        }
    });
    connect(add, &QPushButton::clicked, this, [this] {
        storeCurrent();
        Signature s;
        s.name = tr("Signature %1").arg(m_sigs.size() + 1);
        m_sigs << s;
        m_list->addItem(s.name);
        m_list->setCurrentRow(int(m_sigs.size()) - 1);
        refreshDefaults();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0) {
            return;
        }
        m_row = -1;
        m_sigs.removeAt(row);
        delete m_list->takeItem(row);
        load(m_list->currentRow());
        refreshDefaults();
    });
    connect(bb, &QDialogButtonBox::accepted, this, &SignaturesDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &SignaturesDialog::reject);
    m_list->setCurrentRow(m_sigs.isEmpty() ? -1 : 0);
    load(m_list->currentRow());
}

void SignaturesDialog::mergeFormat(const QTextCharFormat &f)
{
    QTextCursor c = m_rich->textCursor();
    if (!c.hasSelection()) {
        c.select(QTextCursor::WordUnderCursor);
    }
    c.mergeCharFormat(f);
    m_rich->mergeCurrentCharFormat(f);
    m_rich->setFocus();
}

void SignaturesDialog::setTextColor(const QColor &c)
{
    QTextCharFormat f;
    f.setForeground(c);
    mergeFormat(f);
    m_color->setIcon(swatch(c));
}

void SignaturesDialog::syncToolbar(const QTextCharFormat &f)
{
    m_bold->setChecked(f.fontWeight() >= QFont::DemiBold);
    m_italic->setChecked(f.fontItalic());
    m_underline->setChecked(f.fontUnderline());
    const QStringList fam = f.fontFamilies().toStringList();
    if (!fam.isEmpty()) {
        m_font->setCurrentFont(QFont(fam.first()));
    }
    if (f.fontPointSize() > 0) {
        m_size->setCurrentText(QString::number(f.fontPointSize()));
    }
    m_color->setIcon(swatch(f.foreground().style() == Qt::NoBrush ? m_rich->palette().color(QPalette::Text)
                                                                   : f.foreground().color()));
}

bool SignaturesDialog::insertLink(const QString &textIn, const QString &urlIn)
{
    const QString url = urlIn.trimmed();
    if (!url.isEmpty() && !linkAllowed(QUrl(url, QUrl::StrictMode))) {
        return false;
    }
    QTextCursor c = m_rich->textCursor();
    // On an existing link with no selection: edit that whole link.
    const QString current = c.charFormat().anchorHref();
    if (!c.hasSelection() && !current.isEmpty()) {
        const auto r = anchorRange(m_rich->document(), c.position(), current);
        c.setPosition(r.first);
        c.setPosition(r.second, QTextCursor::KeepAnchor);
    }
    c.beginEditBlock();
    if (url.isEmpty()) {
        QTextCharFormat plain;
        plain.setAnchor(false);
        plain.setAnchorHref(QString());
        plain.setFontUnderline(false);
        c.mergeCharFormat(plain);
        // Back to the default colour (not the theme's, which may be light).
        for (int i = c.selectionStart(); i < c.selectionEnd();) {
            QTextCursor one(m_rich->document());
            one.setPosition(i);
            one.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            QTextCharFormat f = one.charFormat();
            f.clearForeground();
            one.setCharFormat(f);
            ++i;
        }
        c.endEditBlock();
        return true;
    }
    QTextCharFormat link = c.charFormat();
    link.setAnchor(true);
    link.setAnchorHref(url);
    link.setFontUnderline(true);
    link.setForeground(QColor(0x1a, 0x5f, 0xb4));
    const QString text = textIn.isEmpty() ? (c.hasSelection() ? c.selectedText() : url) : textIn;
    if (!c.hasSelection() || c.selectedText() != text) {
        c.removeSelectedText();
        c.insertText(text, link);
    } else {
        c.mergeCharFormat(link);
    }
    // Typing on after the link isn't part of it.
    QTextCharFormat after = link;
    after.setAnchor(false);
    after.setAnchorHref(QString());
    after.setFontUnderline(false);
    after.clearForeground();
    c.clearSelection();
    c.setCharFormat(after);
    c.endEditBlock();
    m_rich->setTextCursor(c);
    return true;
}

void SignaturesDialog::editLink()
{
    QTextCursor c = m_rich->textCursor();
    QString href = c.charFormat().anchorHref();
    QString text = c.selectedText();
    if (!c.hasSelection() && !href.isEmpty()) {
        const auto r = anchorRange(m_rich->document(), c.position(), href);
        QTextCursor s(m_rich->document());
        s.setPosition(r.first);
        s.setPosition(r.second, QTextCursor::KeepAnchor);
        text = s.selectedText();
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("signatureLinkDialog"));
    dlg.setWindowTitle(href.isEmpty() ? tr("Insert link") : tr("Edit link"));
    auto *fl = new QFormLayout(&dlg);
    auto *textEdit = new QLineEdit(text, &dlg);
    textEdit->setObjectName(QStringLiteral("linkText"));
    auto *urlEdit = new QLineEdit(href.isEmpty() ? QStringLiteral("https://") : href, &dlg);
    urlEdit->setObjectName(QStringLiteral("linkUrl"));
    urlEdit->setPlaceholderText(tr("https://… or mailto:…  (empty removes the link)"));
    fl->addRow(tr("Text:"), textEdit);
    fl->addRow(tr("Address:"), urlEdit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    fl->addRow(bb);
    auto validate = [bb, urlEdit] {
        const QString u = urlEdit->text().trimmed();
        bb->button(QDialogButtonBox::Ok)->setEnabled(u.isEmpty() || linkAllowed(QUrl(u, QUrl::StrictMode)));
    };
    connect(urlEdit, &QLineEdit::textChanged, &dlg, validate);
    validate();
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) {
        insertLink(textEdit->text(), urlEdit->text());
    }
}

void SignaturesDialog::refreshPreview()
{
    const QString html = m_edited ? sanitizeSignatureHtml(m_rich->toHtml())
                                  : (m_row >= 0 && m_row < m_sigs.size() ? m_sigs[m_row].richHtml() : QString());
    QString plain;
    if (!m_edited && m_row >= 0 && m_row < m_sigs.size()) {
        plain = m_sigs[m_row].plain();
    } else {
        plain = signaturePlainFromHtml(html);
    }
    m_preview->setHtml(html.isEmpty() ? QString()
                                      : QStringLiteral("<p style='color:#80868b'>-- </p>") + html);
    m_plain->setPlainText(plain.isEmpty() ? QString() : QStringLiteral("-- \n") + plain);
}

QString SignaturesDialog::plainPreview() const
{
    return m_plain->toPlainText();
}

void SignaturesDialog::load(int row)
{
    m_row = row;
    const bool on = row >= 0 && row < m_sigs.size();
    m_name->setEnabled(on);
    m_rich->setEnabled(on);
    m_name->setText(on ? m_sigs[row].name : QString());
    m_loading = true;
    m_rich->setHtml(on ? m_sigs[row].richHtml() : QString());
    m_rich->document()->setModified(false);
    m_loading = false;
    m_edited = false;
    refreshPreview();
}

void SignaturesDialog::storeCurrent()
{
    if (m_row < 0 || m_row >= m_sigs.size()) {
        return;
    }
    Signature &s = m_sigs[m_row];
    s.name = m_name->text().trimmed();
    if (m_edited) {
        // Edited here: the styled version is the signature, the plain one
        // is generated from it. Untouched (e.g. a plain-only signature from
        // before) stays exactly as stored.
        s.html = sanitizeSignatureHtml(m_rich->toHtml());
        s.text.clear();
    }
    refreshDefaults();
}

void SignaturesDialog::refreshDefaults()
{
    const QString cur = m_default->currentText();
    m_default->clear();
    m_default->addItem(tr("(none)"));
    for (const auto &s : m_sigs) {
        m_default->addItem(s.name);
    }
    m_default->setCurrentIndex(std::max(0, m_default->findText(cur)));
}

void SignaturesDialog::accept()
{
    storeCurrent();
    QList<Signature> keep;
    for (const auto &s : m_sigs) {
        if (!s.name.isEmpty()) {
            keep << s;
        }
    }
    m_store->setAll(keep);
    m_store->setDefaultName(m_default->currentIndex() > 0 ? m_default->currentText() : QString());
    QDialog::accept();
}
