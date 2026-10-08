#include "ComposeWindow.h"

#include "Icons.h"
#include "SafeTextEdit.h"
#include "SpellHighlighter.h"
#include "Theme.h"
#include "core/GmailClient.h"
#include "core/Log.h"
#include "core/MailSession.h"
#include "core/ContactStore.h"
#include "core/MailCache.h"
#include "core/Markdown.h"
#include "core/MessageParser.h"
#include "core/RichText.h"
#include "core/Sender.h"
#include "core/Signatures.h"
#include "core/SizeFormat.h"
#include "core/SpellChecker.h"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCompleter>
#include <QStringListModel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextCharFormat>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTextList>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

using zmail::ui::icon;
namespace limits = zmail::limits;
using namespace zmail;

namespace {
// Block properties that tag the signature and the quoted original, so the
// signature can be swapped and the format switched without touching them.
constexpr int kSigProp = QTextFormat::UserProperty + 1;
constexpr int kQuoteProp = QTextFormat::UserProperty + 2;

SpellChecker *sharedSpellChecker()
{
    static QPointer<SpellChecker> s;
    if (!s) {
        s = new SpellChecker(qApp);
    }
    return s;
}

QString formatBytes(qint64 n)
{
    return zmail::formatSize(n); // same units as the message list and status bar
}

QString plainToHtml(const QString &text)
{
    QString out;
    for (const QString &para : text.split(QLatin1Char('\n'))) {
        out += QStringLiteral("<p>") + (para.isEmpty() ? QStringLiteral("<br>") : para.toHtmlEscaped()) +
               QStringLiteral("</p>");
    }
    return out;
}
} // namespace

ComposeWindow::ComposeWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("composeWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAcceptDrops(true);

    buildToolbar();
    addToolBarBreak();
    buildFormatBar();

    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    buildHeaderBlock(central);
    v->addWidget(m_headerBlock);
    buildBanner(central);
    v->addWidget(m_banner);

    m_stack = new QStackedWidget(central);
    m_body = new zmail::ui::SafeTextEdit(m_stack); // never reads files named in quoted/pasted HTML
    m_body->setObjectName(QStringLiteral("composeBody"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setAcceptRichText(true);
    m_body->document()->setDocumentMargin(14);
    m_body->setContextMenuPolicy(Qt::CustomContextMenu);
    m_body->viewport()->installEventFilter(this);
    m_body->installEventFilter(this); // lets Ctrl+K reach Insert link (see eventFilter)
    // Keep B/I/U (format bar, built above) showing the format under the
    // cursor, so Ctrl+B/I/U toggle from the right state.
    connect(m_body, &QTextEdit::currentCharFormatChanged, this, [this](const QTextCharFormat &f) {
        QAction *bold = findChild<QAction *>(QStringLiteral("actionBold"));
        QAction *italic = findChild<QAction *>(QStringLiteral("actionItalic"));
        QAction *underline = findChild<QAction *>(QStringLiteral("actionUnderline"));
        const QSignalBlocker b1(bold), b2(italic), b3(underline);
        bold->setChecked(f.fontWeight() >= QFont::Bold);
        italic->setChecked(f.fontItalic());
        underline->setChecked(f.fontUnderline());
    });
    m_preview = new QTextBrowser(m_stack);
    m_preview->setObjectName(QStringLiteral("markdownPreview"));
    m_preview->setFrameShape(QFrame::NoFrame);
    m_preview->setOpenLinks(false);
    m_preview->document()->setDocumentMargin(14);
    m_stack->addWidget(m_body);
    m_stack->addWidget(m_preview);
    v->addWidget(m_stack, 1);
    setCentralWidget(central);

    m_sizeMeter = new QProgressBar(this);
    m_sizeMeter->setObjectName(QStringLiteral("sizeMeter"));
    m_sizeMeter->setRange(0, 1000);
    m_sizeMeter->setTextVisible(true);
    m_sizeMeter->setFixedWidth(300);
    m_sizeMeter->setMaximumHeight(16);
    m_modeLabel = new QLabel(this);
    m_modeLabel->setObjectName(QStringLiteral("composeModeLabel"));
    statusBar()->addWidget(m_modeLabel, 1);
    statusBar()->addPermanentWidget(new QLabel(tr("Size:"), this));
    statusBar()->addPermanentWidget(m_sizeMeter);

    m_spell = sharedSpellChecker();
    m_highlighter = new SpellHighlighter(m_spell, m_body->document());
    m_spellAction->setEnabled(m_spell->isAvailable());
    m_spellAction->setChecked(m_spell->isAvailable() && QSettings().value(QStringLiteral("compose/spellCheck"), true).toBool());
    if (!m_spell->isAvailable()) {
        m_spellAction->setToolTip(tr("Spell check needs Hunspell and an en_US dictionary (hunspell-en-us)"));
    }
    m_highlighter->setDocument(m_spellAction->isChecked() ? m_body->document() : nullptr);
    connect(m_spellAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("compose/spellCheck"), on);
        m_highlighter->setDocument(on ? m_body->document() : nullptr);
        updateModeLabel();
    });
    connect(m_body, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = m_body->createStandardContextMenu(pos);
        if (m_spellAction->isChecked()) {
            SpellHighlighter::addSuggestions(menu, m_body, m_spell, pos);
        }
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_body->viewport()->mapToGlobal(pos));
    });

    connect(m_subject, &QLineEdit::textChanged, this, &ComposeWindow::updateTitle);
    connect(m_body, &QTextEdit::textChanged, this, &ComposeWindow::updateSizeMeter);
    connect(m_format, qOverload<int>(&QComboBox::activated), this, [this](int i) { setFormat(Format(i)); });
    connect(m_signature, qOverload<int>(&QComboBox::activated), this, [this](int) {
        applySignature();
        updateModeLabel();
    });

    reloadSignatures();
    // Put the default signature in the body now. Before, only setDraft()
    // (Reply/Forward), setSignatureStore() or a format change inserted it,
    // so a New message in the default format went out unsigned even though
    // the bar said "Signature: <name>".
    applySignature();
    refreshChips();
    updateTitle();
    updateSizeMeter();
    updateModeLabel();
    resize(860, 640);
}

ComposeWindow::~ComposeWindow()
{
    delete m_ownSigStore;
}

// ---- construction -----------------------------------------------------------

void ComposeWindow::buildToolbar()
{
    auto *tb = addToolBar(tr("Message"));
    tb->setObjectName(QStringLiteral("composeToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_send = tb->addAction(icon(QStringLiteral("send")), tr("Send"));
    m_send->setObjectName(QStringLiteral("actionSend"));
    m_send->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_E)}); // Ctrl+E as in Eudora
    m_send->setToolTip(tr("Send now (Ctrl+Enter or Ctrl+E)"));
    connect(m_send, &QAction::triggered, this, &ComposeWindow::send);
    QAction *later = tb->addAction(icon(QStringLiteral("clock")), tr("Send Later"));
    later->setObjectName(QStringLiteral("actionSendLater"));
    later->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
    later->setToolTip(tr("Queue it in Out, to go with File \u203a Send Queued Messages (Ctrl+Shift+Enter)"));
    connect(later, &QAction::triggered, this, &ComposeWindow::queue);
    m_saveDraft = tb->addAction(icon(QStringLiteral("save")), tr("Save Draft"));
    m_saveDraft->setObjectName(QStringLiteral("actionSaveDraft"));
    m_saveDraft->setShortcut(QKeySequence::Save);
    m_saveDraft->setToolTip(tr("Save to Gmail Drafts (Ctrl+S)"));
    connect(m_saveDraft, &QAction::triggered, this, &ComposeWindow::saveDraft);
    tb->addSeparator();
    QAction *attach = tb->addAction(icon(QStringLiteral("paperclip")), tr("Attach"));
    attach->setObjectName(QStringLiteral("actionComposeAttach"));
    attach->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H)); // as in Eudora
    connect(attach, &QAction::triggered, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Attach files"));
        if (!files.isEmpty()) {
            addFiles(files);
        }
    });
    // Stationery: use one in this message, or keep this message as one.
    auto *stationery = new QToolButton(tb);
    stationery->setObjectName(QStringLiteral("stationeryButton"));
    stationery->setText(tr("Stationery"));
    stationery->setToolButtonStyle(Qt::ToolButtonTextOnly);
    stationery->setPopupMode(QToolButton::InstantPopup);
    auto *stationeryMenu = new QMenu(stationery);
    stationeryMenu->setObjectName(QStringLiteral("composeStationeryMenu"));
    connect(stationeryMenu, &QMenu::aboutToShow, this, [this, stationeryMenu]() {
        stationeryMenu->clear();
        for (const Stationery &s : StationeryStore().all()) {
            stationeryMenu->addAction(s.name, this, [this, s]() { setStationery(s); });
        }
        if (!stationeryMenu->isEmpty()) {
            stationeryMenu->addSeparator();
        }
        QAction *save = stationeryMenu->addAction(tr("Save This Message as Stationery\u2026"), this, [this]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Save as Stationery"), tr("Name:"), QLineEdit::Normal,
                                                       m_subject->text().trimmed(), &ok);
            if (ok && !name.trimmed().isEmpty()) {
                saveAsStationery(name);
            }
        });
        save->setObjectName(QStringLiteral("actionSaveAsStationery"));
    });
    stationery->setMenu(stationeryMenu);
    tb->addWidget(stationery);
    m_spellAction = tb->addAction(icon(QStringLiteral("spell-check")), tr("Spelling"));
    m_spellAction->setObjectName(QStringLiteral("actionSpelling"));
    m_spellAction->setCheckable(true);
    m_previewAction = tb->addAction(icon(QStringLiteral("eye")), tr("Preview"));
    m_previewAction->setObjectName(QStringLiteral("actionMarkdownPreview"));
    m_previewAction->setCheckable(true);
    m_previewAction->setToolTip(tr("Show the rendered Markdown"));
    m_previewAction->setVisible(false);
    connect(m_previewAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            m_preview->setHtml(markdown::toEmailHtml(m_body->toPlainText()));
        }
        m_stack->setCurrentWidget(on ? static_cast<QWidget *>(m_preview) : m_body);
    });
    tb->addSeparator();

    tb->addWidget(new QLabel(tr(" Signature "), tb));
    m_signature = new QComboBox(tb);
    m_signature->setObjectName(QStringLiteral("signatureCombo"));
    m_signature->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    tb->addWidget(m_signature);
    tb->addWidget(new QLabel(tr("  Format "), tb));
    m_format = new QComboBox(tb);
    m_format->setObjectName(QStringLiteral("formatCombo"));
    m_format->addItems({tr("HTML"), tr("Plain text"), tr("Markdown")});
    tb->addWidget(m_format);
    tb->addWidget(new QLabel(tr("  Priority "), tb));
    m_priority = new QComboBox(tb);
    m_priority->setObjectName(QStringLiteral("priorityCombo"));
    m_priority->addItems({tr("Normal"), tr("High"), tr("Low")});
    tb->addWidget(m_priority);
}

void ComposeWindow::buildFormatBar()
{
    auto *fb = addToolBar(tr("Format"));
    fb->setObjectName(QStringLiteral("formatToolBar"));
    fb->setMovable(false);
    fb->setIconSize(QSize(16, 16));
    m_formatBar = fb;

    auto *font = new QFontComboBox(fb);
    font->setObjectName(QStringLiteral("fontCombo"));
    font->setCurrentFont(QApplication::font());
    font->setMaximumWidth(180);
    fb->addWidget(font);
    auto *size = new QComboBox(fb);
    size->setObjectName(QStringLiteral("fontSizeCombo"));
    size->addItems({"9", "10", "11", "12", "14", "16", "18", "24"});
    size->setCurrentText(QStringLiteral("11"));
    fb->addWidget(size);
    fb->addSeparator();

    auto add = [&](const char *obj, const char *ic, const QString &tip, bool checkable = false) {
        QAction *a = fb->addAction(icon(QString::fromLatin1(ic)), tip);
        a->setObjectName(QString::fromLatin1(obj));
        a->setToolTip(tip);
        a->setCheckable(checkable);
        return a;
    };
    QAction *bold = add("actionBold", "bold", tr("Bold"), true);
    QAction *italic = add("actionItalic", "italic", tr("Italic"), true);
    QAction *underline = add("actionUnderline", "underline", tr("Underline"), true);
    QAction *color = add("actionTextColor", "baseline", tr("Text colour"));
    fb->addSeparator();
    QAction *bullets = add("actionBullets", "list", tr("Bulleted list"));
    QAction *numbers = add("actionNumbers", "list-ordered", tr("Numbered list"));
    fb->addSeparator();
    QAction *left = add("actionAlignLeft", "text-align-start", tr("Align left"));
    QAction *center = add("actionAlignCenter", "text-align-center", tr("Centre"));
    QAction *right = add("actionAlignRight", "text-align-end", tr("Align right"));
    fb->addSeparator();
    QAction *link = add("actionLink", "link", tr("Insert link"));
    QAction *quote = add("actionQuote", "quote", tr("Quote"));

    // The usual editor keys; the tooltip names the key ("Bold (Ctrl+B)").
    const auto key = [](QAction *a, const QKeySequence &k) {
        a->setShortcut(k);
        a->setShortcutContext(Qt::WindowShortcut);
        a->setToolTip(QStringLiteral("%1 (%2)").arg(a->toolTip(), k.toString(QKeySequence::NativeText)));
    };
    key(bold, QKeySequence::Bold);
    key(italic, QKeySequence::Italic);
    key(underline, QKeySequence::Underline);
    key(link, QKeySequence(Qt::CTRL | Qt::Key_K));

    connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) { m_body->setCurrentFont(f); });
    connect(size, &QComboBox::textActivated, this, [this](const QString &s) { m_body->setFontPointSize(s.toDouble()); });
    connect(bold, &QAction::toggled, this, [this](bool on) { m_body->setFontWeight(on ? QFont::Bold : QFont::Normal); });
    connect(italic, &QAction::toggled, this, [this](bool on) { m_body->setFontItalic(on); });
    connect(underline, &QAction::toggled, this, [this](bool on) { m_body->setFontUnderline(on); });
    connect(color, &QAction::triggered, this, [this] {
        const QColor c = QColorDialog::getColor(m_body->textColor(), this, tr("Text colour"));
        if (c.isValid()) {
            m_body->setTextColor(c);
        }
    });
    connect(bullets, &QAction::triggered, this, [this]() { m_body->textCursor().createList(QTextListFormat::ListDisc); });
    connect(numbers, &QAction::triggered, this, [this]() { m_body->textCursor().createList(QTextListFormat::ListDecimal); });
    connect(left, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignLeft); });
    connect(center, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignHCenter); });
    connect(right, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignRight); });
    connect(link, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString url = QInputDialog::getText(this, tr("Insert link"), tr("Address:"), QLineEdit::Normal,
                                                  QStringLiteral("https://"), &ok).trimmed();
        const QUrl u(url, QUrl::StrictMode);
        if (!ok || !u.isValid() || !(u.scheme() == QLatin1String("https") || u.scheme() == QLatin1String("http") ||
                                     u.scheme() == QLatin1String("mailto"))) {
            return;
        }
        QTextCursor c = m_body->textCursor();
        QTextCharFormat f;
        f.setAnchor(true);
        f.setAnchorHref(u.toString());
        f.setForeground(QApplication::palette().color(QPalette::Link));
        f.setFontUnderline(true);
        if (c.hasSelection()) {
            c.mergeCharFormat(f);
        } else {
            c.insertText(u.toString(), f);
        }
    });
    connect(quote, &QAction::triggered, this, [this] {
        QTextCursor c = m_body->textCursor();
        QTextBlockFormat f = c.blockFormat();
        const int level = f.property(QTextFormat::BlockQuoteLevel).toInt() + 1;
        f.setProperty(QTextFormat::BlockQuoteLevel, level);
        f.setLeftMargin(24.0 * level);
        c.mergeBlockFormat(f);
    });
}

void ComposeWindow::buildHeaderBlock(QWidget *host)
{
    m_headerBlock = new QFrame(host);
    m_headerBlock->setObjectName(QStringLiteral("headerBlock"));
    auto *frame = static_cast<QFrame *>(m_headerBlock);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setAutoFillBackground(true);
    QPalette p = frame->palette();
    p.setColor(QPalette::Window, p.color(QPalette::AlternateBase));
    frame->setPalette(p);

    auto *g = new QGridLayout(frame);
    g->setContentsMargins(12, 8, 12, 8);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(4);

    int row = 0;
    auto field = [&](const QString &label, const char *obj) {
        auto *l = new QLabel(label, frame);
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *e = new QLineEdit(frame);
        e->setObjectName(QString::fromLatin1(obj));
        e->setFrame(false);
        l->setBuddy(e);
        g->addWidget(l, row, 0);
        g->addWidget(e, row, 1);
        ++row;
        return e;
    };
    m_to = field(tr("To:"), "fieldTo");
    m_from = field(tr("From:"), "fieldFrom");
    m_from->setReadOnly(true);
    m_subject = field(tr("Subject:"), "fieldSubject");
    m_cc = field(tr("Cc:"), "fieldCc");
    m_bcc = field(tr("Bcc:"), "fieldBcc");

    auto *al = new QLabel(tr("Attached:"), frame);
    QFont f = al->font();
    f.setBold(true);
    al->setFont(f);
    al->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_attached = new QWidget(frame);
    m_attached->setObjectName(QStringLiteral("fieldAttached"));
    auto *ah = new QHBoxLayout(m_attached);
    ah->setContentsMargins(2, 0, 0, 0);
    ah->setSpacing(6);
    g->addWidget(al, row, 0);
    g->addWidget(m_attached, row, 1);
    g->setColumnStretch(1, 1);
}

void ComposeWindow::buildBanner(QWidget *host)
{
    m_banner = new QFrame(host);
    m_banner->setObjectName(QStringLiteral("composeBanner"));
    m_banner->setFrameShape(QFrame::StyledPanel);
    m_banner->setAutoFillBackground(true);
    auto *h = new QHBoxLayout(m_banner);
    h->setContentsMargins(12, 6, 12, 6);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setObjectName(QStringLiteral("composeBannerText"));
    m_bannerText->setWordWrap(true);
    h->addWidget(m_bannerText, 1);
    m_bannerZip = new QPushButton(icon(QStringLiteral("file-archive")), tr("Zip attachments\u2026"), m_banner);
    m_bannerZip->setObjectName(QStringLiteral("bannerZip"));
    m_bannerUndo = new QPushButton(tr("Undo zip"), m_banner);
    m_bannerUndo->setObjectName(QStringLiteral("bannerUndoZip"));
    h->addWidget(m_bannerZip);
    h->addWidget(m_bannerUndo);
    connect(m_bannerZip, &QPushButton::clicked, this, [this] { offerZip(); });
    connect(m_bannerUndo, &QPushButton::clicked, this, &ComposeWindow::revertZip);
    m_banner->hide();
}

QStringList ComposeWindow::headerFieldOrder() const
{
    QStringList out;
    auto *g = qobject_cast<QGridLayout *>(m_headerBlock->layout());
    for (int r = 0; r < g->rowCount(); ++r) {
        if (auto *l = qobject_cast<QLabel *>(g->itemAtPosition(r, 0)->widget())) {
            out << l->text().remove(QLatin1Char(':'));
        }
    }
    return out;
}

// ---- attachments ------------------------------------------------------------

void ComposeWindow::setAttachments(const QList<Attachment> &list)
{
    m_attachments = list;
    m_unzipped.clear();
    refreshChips();
    updateSizeMeter();
}

void ComposeWindow::addAttachment(const Attachment &a)
{
    m_attachments << a;
    refreshChips();
    updateSizeMeter();
}

void ComposeWindow::addFiles(const QStringList &paths)
{
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (!fi.isFile() || !fi.isReadable()) {
            fail(tr("Can't read %1").arg(p));
            continue;
        }
        Attachment a;
        a.name = fi.fileName();
        a.path = fi.absoluteFilePath();
        a.bytes = fi.size();
        m_attachments << a;
    }
    refreshChips();
    updateSizeMeter();
    if (sizeLevel() == limits::SizeLevel::Blocked && !isZipped()) {
        // Ask after the event that added them has finished.
        QMetaObject::invokeMethod(this, [this] { offerZip(); }, Qt::QueuedConnection);
    }
}

void ComposeWindow::removeAttachment(int index)
{
    if (index < 0 || index >= m_attachments.size()) {
        return;
    }
    m_attachments.removeAt(index);
    if (m_attachments.isEmpty()) {
        m_unzipped.clear();
    }
    refreshChips();
    updateSizeMeter();
}

void ComposeWindow::refreshChips()
{
    auto *h = static_cast<QHBoxLayout *>(m_attached->layout());
    while (QLayoutItem *it = h->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QString dim = QApplication::palette().color(QPalette::PlaceholderText).name();
    if (m_attachments.isEmpty()) {
        h->addWidget(new QLabel(tr("<span style='color:%1'>(none)</span>").arg(dim), m_attached));
    }
    for (int i = 0; i < m_attachments.size(); ++i) {
        const Attachment &a = m_attachments[i];
        auto *chip = new QFrame(m_attached);
        chip->setObjectName(QStringLiteral("attachmentChip"));
        chip->setFrameShape(QFrame::StyledPanel);
        auto *ch = new QHBoxLayout(chip);
        ch->setContentsMargins(6, 1, 2, 1);
        ch->setSpacing(4);
        auto *ic = new QLabel(chip);
        ic->setPixmap(icon(QStringLiteral("paperclip")).pixmap(14, 14));
        ch->addWidget(ic);
        ch->addWidget(new QLabel(QStringLiteral("%1 <span style='color:%2'>(%3)</span>")
                                     .arg(a.name.toHtmlEscaped(), dim, formatBytes(a.bytes)),
                                 chip));
        auto *x = new QToolButton(chip);
        x->setObjectName(QStringLiteral("removeAttachment"));
        x->setIcon(icon(QStringLiteral("x")));
        x->setIconSize(QSize(12, 12));
        x->setAutoRaise(true);
        x->setToolTip(tr("Remove %1").arg(a.name));
        connect(x, &QToolButton::clicked, this, [this, i] { removeAttachment(i); });
        ch->addWidget(x);
        h->addWidget(chip);
    }
    h->addStretch(1);
}

void ComposeWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

void ComposeWindow::dropEvent(QDropEvent *e)
{
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls()) {
        if (u.isLocalFile()) {
            files << u.toLocalFile();
        }
    }
    if (!files.isEmpty()) {
        addFiles(files);
        e->acceptProposedAction();
    }
}

bool ComposeWindow::eventFilter(QObject *obj, QEvent *ev)
{
    // QTextEdit claims Ctrl+K (delete to end of line) before shortcuts see
    // it; here it means Insert link, as in most mail and word processors.
    if (obj == m_body && ev->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(ev);
        if (ke->key() == Qt::Key_K && ke->modifiers() == Qt::ControlModifier) {
            ev->ignore();
            return true;
        }
    }
    // Files dropped on the body attach rather than insert a file:// link.
    if (obj == m_body->viewport() && (ev->type() == QEvent::DragEnter || ev->type() == QEvent::Drop)) {
        auto *de = static_cast<QDropEvent *>(ev);
        if (de->mimeData()->hasUrls() && de->mimeData()->urls().value(0).isLocalFile()) {
            if (ev->type() == QEvent::DragEnter) {
                static_cast<QDragEnterEvent *>(ev)->acceptProposedAction();
            } else {
                dropEvent(de);
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// ---- zipping (PLAN §4.5.1) ---------------------------------------------------

ComposeWindow::ZipPolicy ComposeWindow::zipPolicy()
{
    const QString v = QSettings().value(QStringLiteral("compose/zipPolicy"), QStringLiteral("ask")).toString();
    return v == QLatin1String("always") ? ZipPolicy::Always : v == QLatin1String("never") ? ZipPolicy::Never : ZipPolicy::Ask;
}

void ComposeWindow::setZipPolicy(ZipPolicy p)
{
    QSettings().setValue(QStringLiteral("compose/zipPolicy"), p == ZipPolicy::Always ? QStringLiteral("always")
                                                              : p == ZipPolicy::Never ? QStringLiteral("never")
                                                                                      : QStringLiteral("ask"));
}

namespace {
bool loadData(ComposeWindow::Attachment &a, QString *err)
{
    if (!a.data.isEmpty() || a.bytes == 0) {
        return true;
    }
    QFile f(a.path);
    if (a.path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        *err = QObject::tr("Can't read %1").arg(a.name);
        return false;
    }
    a.data = f.readAll();
    a.bytes = a.data.size();
    return true;
}

QList<OutgoingAttachment> toOutgoing(const QList<ComposeWindow::Attachment> &list, QString *err)
{
    QList<OutgoingAttachment> out;
    for (ComposeWindow::Attachment a : list) {
        if (!loadData(a, err)) {
            return {};
        }
        out << OutgoingAttachment{a.name, a.mimeType, a.data};
    }
    return out;
}
} // namespace

bool ComposeWindow::offerZip()
{
    if (m_attachments.isEmpty() || isZipped()) {
        return false;
    }
    const ZipPolicy policy = zipPolicy();
    if (policy == ZipPolicy::Never) {
        updateBanner();
        return false;
    }
    QString err;
    const QList<OutgoingAttachment> out = toOutgoing(m_attachments, &err);
    if (!err.isEmpty()) {
        fail(err);
        return false;
    }
    const zip::Probe probe = zip::probe(out);
    qint64 attachEncoded = 0;
    for (const Attachment &a : m_attachments) {
        attachEncoded += 300 + limits::base64MimeSize(a.bytes);
    }
    const qint64 estimate = encodedSize() - attachEncoded + 300 + limits::base64MimeSize(probe.estimatedZipBytes);
    if (!probe.worthwhile || limits::classifySendSize(estimate) == limits::SizeLevel::Blocked) {
        m_lastError = tr("Zipping won't help: the attachments would still be about %1 once encoded, over "
                         "Gmail's %2 MB limit. Remove some attachments or share them via Drive.")
                          .arg(formatBytes(estimate))
                          .arg(limits::kSendLimitBytes / 1'000'000);
        updateBanner();
        return false;
    }
    if (policy == ZipPolicy::Ask) {
        bool remember = false;
        ZipAnswer ans = ZipAnswer::Cancel;
        if (m_zipPrompt) {
            ans = m_zipPrompt(probe, &remember);
        } else {
            QMessageBox box(QMessageBox::Question, tr("Zip attachments?"),
                            tr("This message is over Gmail's %1 MB limit once encoded.\n\nZipping the %2 attachments "
                               "into attachments.zip should bring them from %3 to about %4, which fits. Zip them?")
                                .arg(limits::kSendLimitBytes / 1'000'000)
                                .arg(m_attachments.size())
                                .arg(formatBytes(probe.rawBytes), formatBytes(probe.estimatedZipBytes)),
                            QMessageBox::NoButton, this);
            QPushButton *zipBtn = box.addButton(tr("Zip"), QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            auto *rememberBox = new QCheckBox(tr("Remember my choice"), &box);
            box.setCheckBox(rememberBox);
            box.exec();
            ans = box.clickedButton() == zipBtn ? ZipAnswer::Zip : ZipAnswer::Cancel;
            remember = rememberBox->isChecked();
        }
        if (remember) {
            setZipPolicy(ans == ZipAnswer::Zip ? ZipPolicy::Always : ZipPolicy::Never);
        }
        if (ans != ZipAnswer::Zip) {
            updateBanner();
            return false;
        }
    }
    return zipAttachments();
}

bool ComposeWindow::zipAttachments()
{
    QString err;
    const QList<OutgoingAttachment> out = toOutgoing(m_attachments, &err);
    if (!err.isEmpty() || out.isEmpty()) {
        fail(err.isEmpty() ? tr("Nothing to zip") : err);
        return false;
    }
    const QByteArray zipData = zip::makeArchive(out, &err);
    if (zipData.isEmpty()) {
        fail(tr("Couldn't create attachments.zip: %1").arg(err));
        return false;
    }
    m_unzipped = m_attachments;
    Attachment z;
    z.name = QStringLiteral("attachments.zip");
    z.mimeType = QStringLiteral("application/zip");
    z.data = zipData;
    z.bytes = zipData.size();
    m_attachments = {z};
    m_lastError.clear();
    qCInfo(lcGmail) << "zipped" << m_unzipped.size() << "attachments into" << z.bytes << "bytes";
    refreshChips();
    updateSizeMeter();
    return true;
}

void ComposeWindow::revertZip()
{
    if (m_unzipped.isEmpty()) {
        return;
    }
    m_attachments = m_unzipped;
    m_unzipped.clear();
    m_lastError.clear();
    refreshChips();
    updateSizeMeter();
}

// ---- size ---------------------------------------------------------------------

qint64 ComposeWindow::encodedSize() const
{
    // Headers + HTML part + generated text/plain part, then base64 attachments.
    qint64 total = 2048;
    if (m_body) {
        const qint64 text = m_body->toPlainText().toUtf8().size();
        const qint64 html = m_currentFormat == Format::Plain ? 0
                          : m_currentFormat == Format::Html  ? m_body->toHtml().toUtf8().size()
                                                             : text * 2;
        total += limits::base64MimeSize(html) + limits::base64MimeSize(text);
    }
    for (const Attachment &a : m_attachments) {
        total += 300 + limits::base64MimeSize(a.bytes); // part headers + body
    }
    return total;
}

limits::SizeLevel ComposeWindow::sizeLevel() const
{
    return limits::classifySendSize(encodedSize());
}

void ComposeWindow::updateSizeMeter()
{
    if (!m_sizeMeter) {
        return;
    }
    const qint64 size = encodedSize();
    m_sizeMeter->setValue(int(std::min<qint64>(1000, size * 1000 / limits::kSendLimitBytes)));
    m_sizeMeter->setFormat(tr("%1 of %2 (encoded)").arg(formatBytes(size), formatBytes(limits::kSendLimitBytes)));

    const auto level = limits::classifySendSize(size);
    const QString chunk = level == limits::SizeLevel::Blocked ? QStringLiteral("#d32f2f")
                        : level == limits::SizeLevel::Warn ? QStringLiteral("#f0a020")
                                                           : QStringLiteral("#43a047");
    m_sizeMeter->setStyleSheet(QStringLiteral("QProgressBar{border:1px solid palette(mid);border-radius:3px;"
                                              "text-align:center;}"
                                              "QProgressBar::chunk{background:%1;border-radius:2px;}")
                                   .arg(chunk));
    m_send->setEnabled(level != limits::SizeLevel::Blocked && !m_busy);
    m_sizeMeter->setToolTip(level == limits::SizeLevel::Blocked
                                ? tr("Over Gmail's 25 MB limit after encoding. Remove attachments to send.")
                                : tr("Encoded size, counting base64 overhead (about 37%)."));
    updateBanner();
}

void ComposeWindow::updateBanner()
{
    if (!m_banner) {
        return;
    }
    const bool blocked = limits::classifySendSize(encodedSize()) == limits::SizeLevel::Blocked;
    QString text;
    QColor bg;
    const QPalette pal = QApplication::palette();
    if (blocked) {
        text = tr("<b>Too big to send.</b> This message is %1 once encoded; Gmail accepts up to %2 MB.")
                   .arg(formatBytes(encodedSize()))
                   .arg(limits::kSendLimitBytes / 1'000'000);
        if (!m_lastError.isEmpty()) {
            text += QStringLiteral(" ") + m_lastError.toHtmlEscaped();
        }
        bg = zmail::ui::suspiciousBackground(pal);
    } else if (!m_lastError.isEmpty()) {
        text = m_lastError.toHtmlEscaped();
        bg = zmail::ui::suspiciousBackground(pal);
    } else if (isZipped()) {
        text = tr("Zipped %1 attachments into <b>attachments.zip</b> (%2).")
                   .arg(m_unzipped.size())
                   .arg(formatBytes(m_attachments.value(0).bytes));
        bg = pal.color(QPalette::AlternateBase);
    }
    m_bannerZip->setVisible(blocked && !isZipped() && !m_attachments.isEmpty() && zipPolicy() != ZipPolicy::Never);
    m_bannerUndo->setVisible(isZipped());
    if (!text.isEmpty()) {
        QPalette p = m_banner->palette();
        p.setColor(QPalette::Window, bg);
        m_banner->setPalette(p);
        m_bannerText->setText(text);
    }
    m_banner->setVisible(!text.isEmpty());
}

void ComposeWindow::updateTitle()
{
    const QString subj = m_subject->text().trimmed();
    setWindowTitle(QStringLiteral("%1 \u2014 zmail %2")
                       .arg(subj.isEmpty() ? tr("New Message") : subj,
                            QString::fromLatin1(zmail::kVersionString)));
}

void ComposeWindow::updateModeLabel()
{
    if (!m_modeLabel) {
        return;
    }
    const QString fmt = m_format->currentText();
    const QString spell = !m_spell || !m_spell->isAvailable() ? tr("Spell check unavailable")
                        : m_spellAction->isChecked()          ? tr("Spell check on")
                                                              : tr("Spell check off");
    const QString sig = signatureName().isEmpty() ? tr("none") : signatureName();
    m_modeLabel->setText(QStringLiteral("%1 \u00b7 %2 \u00b7 %3").arg(fmt, spell, tr("Signature: %1").arg(sig)));
}

// ---- format ---------------------------------------------------------------------

ComposeWindow::Format ComposeWindow::format() const
{
    return m_currentFormat;
}

void ComposeWindow::setBodyText(const QString &text)
{
    if (m_currentFormat == Format::Html) {
        m_body->setHtml(text);
    } else {
        m_body->setPlainText(text);
    }
    applySignature();
    insertQuote();
}

void ComposeWindow::setFormat(Format f)
{
    m_format->setCurrentIndex(int(f));
    if (f == m_currentFormat) {
        updateModeLabel();
        return;
    }
    // The part the user wrote: everything before the signature / quote.
    QTextDocument *doc = m_body->document();
    int end = doc->characterCount() - 1;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.blockFormat().hasProperty(kSigProp) || b.blockFormat().hasProperty(kQuoteProp)) {
            end = std::max(0, b.position() - 1);
            break;
        }
    }
    QTextCursor sel(doc);
    sel.setPosition(end, QTextCursor::KeepAnchor);
    QTextDocument mine;
    QTextCursor(&mine).insertFragment(sel.selection());

    const Format from = m_currentFormat;
    m_currentFormat = f;
    m_previewAction->setChecked(false);
    m_previewAction->setVisible(f == Format::Markdown);
    m_formatBar->setEnabled(f == Format::Html);
    m_body->setAcceptRichText(f == Format::Html);
    m_body->clear();
    m_body->setCurrentCharFormat(QTextCharFormat());
    if (f == Format::Html) {
        m_body->document()->setDefaultFont(QApplication::font());
        m_body->setHtml(from == Format::Markdown ? markdown::toHtml(mine.toPlainText()) : plainToHtml(mine.toPlainText()));
    } else {
        m_body->document()->setDefaultFont(f == Format::Markdown ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                                                 : QApplication::font());
        QString text;
        if (from == Format::Html) {
            text = f == Format::Markdown ? mine.toMarkdown(QTextDocument::MarkdownDialectCommonMark).trimmed()
                                         : richtext::toPlainText(&mine);
        } else {
            text = mine.toPlainText();
        }
        m_body->setPlainText(text);
    }
    applySignature();
    insertQuote();
    QSettings().setValue(QStringLiteral("compose/format"), int(f));
    updateModeLabel();
    updateSizeMeter();
}

// ---- signatures -------------------------------------------------------------------

void ComposeWindow::setSignatureStore(SignatureStore *store)
{
    m_sigStore = store;
    m_signature->clear();
    reloadSignatures();
    applySignature();
    updateModeLabel();
}

void ComposeWindow::reloadSignatures()
{
    if (!m_sigStore) {
        m_ownSigStore = m_ownSigStore ? m_ownSigStore : new SignatureStore();
        m_sigStore = m_ownSigStore;
    }
    const QString keep = m_signature->count() ? m_signature->currentData().toString() : m_sigStore->defaultName();
    m_signature->clear();
    for (const Signature &s : m_sigStore->all()) {
        m_signature->addItem(s.name, s.name);
    }
    m_signature->addItem(tr("None"), QString());
    const int idx = m_signature->findData(keep);
    m_signature->setCurrentIndex(idx >= 0 ? idx : m_signature->count() - 1);
}

void ComposeWindow::setSignature(const QString &name)
{
    const int idx = m_signature->findData(name);
    m_signature->setCurrentIndex(idx >= 0 ? idx : m_signature->count() - 1);
    applySignature();
    updateModeLabel();
}

QString ComposeWindow::signatureName() const
{
    return m_signature ? m_signature->currentData().toString() : QString();
}

void ComposeWindow::removeTaggedBlocks(int property)
{
    QTextDocument *doc = m_body->document();
    QList<int> numbers;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.blockFormat().hasProperty(property)) {
            numbers << b.blockNumber();
        }
    }
    QTextCursor c(doc);
    c.beginEditBlock();
    for (auto it = numbers.crbegin(); it != numbers.crend(); ++it) {
        QTextBlock b = doc->findBlockByNumber(*it);
        c.setPosition(b.position());
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        if (b.blockNumber() > 0) {
            c.deletePreviousChar(); // the paragraph break before it
        } else {
            QTextBlockFormat f = c.blockFormat();
            f.clearProperty(property);
            c.setBlockFormat(f);
        }
    }
    c.endEditBlock();
}

namespace {
// Tags every block from `from` up to the cursor's block with `property`.
void tagBlocks(QTextDocument *doc, int from, int to, int property, int clear)
{
    for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() <= to; b = b.next()) {
        QTextCursor c(b);
        QTextBlockFormat f = b.blockFormat();
        f.setProperty(property, true);
        f.clearProperty(clear);
        c.setBlockFormat(f);
    }
}
} // namespace

void ComposeWindow::applySignature()
{
    if (!m_body) {
        return;
    }
    removeTaggedBlocks(kSigProp);
    const QString name = signatureName();
    if (name.isEmpty() || !m_sigStore) {
        return;
    }
    const Signature sig = m_sigStore->find(name);
    QTextDocument *doc = m_body->document();
    // Goes above the quoted original (top-posting, as Gmail does).
    QTextBlock anchor = doc->lastBlock();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        if (b.blockFormat().hasProperty(kQuoteProp)) {
            anchor = b.previous().isValid() ? b.previous() : b;
            break;
        }
    }
    // Where the user is typing. Inserting at that very position would push
    // their cursor below the signature, so put it back afterwards.
    QTextCursor user = m_body->textCursor();
    const int keep = user.position();
    const int at = anchor.position() + anchor.length() - 1;
    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(at);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat()); // blank line before "-- "
    const int start = c.position();
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    QTextCharFormat dim;
    if (m_currentFormat == Format::Html) {
        dim.setForeground(QApplication::palette().color(QPalette::PlaceholderText));
        c.insertText(QStringLiteral("-- "), dim);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        c.insertHtml(sig.richHtml());
    } else {
        // Markdown: two trailing spaces keep the lines apart when rendered.
        const QString nl = m_currentFormat == Format::Markdown ? QStringLiteral("  ") : QString();
        c.insertText(QStringLiteral("-- ") + nl);
        for (const QString &line : sig.plain().split(QLatin1Char('\n'))) {
            c.insertBlock(QTextBlockFormat(), QTextCharFormat());
            c.insertText(line + nl);
        }
    }
    tagBlocks(doc, start, c.position(), kSigProp, kQuoteProp);
    c.endEditBlock();
    if (keep <= at && user.position() != keep) {
        user.setPosition(keep);
        m_body->setTextCursor(user);
    }
    doc->setModified(false);
}

void ComposeWindow::insertQuote()
{
    removeTaggedBlocks(kQuoteProp);
    if (m_quotedText.isEmpty() && m_quotedHtml.isEmpty()) {
        return;
    }
    QTextDocument *doc = m_body->document();
    QTextCursor c(doc);
    c.beginEditBlock();
    c.movePosition(QTextCursor::End);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat()); // spacer, removed with the quote
    const int start = c.position();
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    if (m_currentFormat == Format::Html && !m_quotedHtml.isEmpty()) {
        c.insertHtml(m_quotedHtml);
    } else {
        c.insertText(m_quotedText);
    }
    tagBlocks(doc, start, c.position(), kQuoteProp, kSigProp);
    c.endEditBlock();
    QTextCursor top(doc);
    m_body->setTextCursor(top);
    doc->setModified(false);
}

// ---- live mode ---------------------------------------------------------------------

void ComposeWindow::setSession(MailSession *session)
{
    m_session = session;
    if (session) {
        m_from->setText(session->fromHeader());
        connect(session, &MailSession::identityChanged, this,
                [this] { if (m_session) m_from->setText(m_session->fromHeader()); });
        const int fmt = QSettings().value(QStringLiteral("compose/format"), 0).toInt();
        if (fmt != int(m_currentFormat) && fmt >= 0 && fmt <= 2) {
            setFormat(Format(fmt));
        }
        // To/Cc/Bcc autocomplete from the local contacts cache + sent-address frecency.
        if (ContactStore *store = session->contacts()) {
            auto install = [store](QLineEdit *edit) {
                auto *model = new QStringListModel(edit);
                auto *comp = new QCompleter(model, edit);
                comp->setCaseSensitivity(Qt::CaseInsensitive);
                comp->setFilterMode(Qt::MatchStartsWith);
                edit->setCompleter(comp);
                QObject::connect(edit, &QLineEdit::textEdited, edit, [store, model, edit](const QString &text) {
                    // Complete the last address fragment after a comma.
                    QString frag = text.section(QLatin1Char(','), -1).trimmed();
                    if (frag.contains(QLatin1Char('<'))) {
                        frag = frag.section(QLatin1Char('<'), -1);
                    }
                    QStringList rows;
                    for (const AutocompleteHit &h : store->autocomplete(frag, 12)) {
                        rows << (h.email.isEmpty() ? h.displayName // a nickname or category, written out below
                                 : h.displayName.isEmpty()
                                     ? h.email
                                     : QStringLiteral("%1 <%2>").arg(h.displayName, h.email));
                    }
                    model->setStringList(rows);
                });
                // Nicknames (a contact's, or a category's name for everyone in
                // it) are written out when the field is left.
                QObject::connect(edit, &QLineEdit::editingFinished, edit, [store, edit]() {
                    const QString expanded = store->expandRecipients(edit->text());
                    if (expanded != edit->text().trimmed() && !expanded.isEmpty()) {
                        edit->setText(expanded);
                        edit->setModified(true);
                    }
                });
            };
            install(m_to);
            install(m_cc);
            install(m_bcc);
        }
    }
}

void ComposeWindow::setDraft(const ComposeDraft &d)
{
    m_to->setText(d.to);
    m_cc->setText(d.cc);
    m_bcc->setText(d.bcc);
    m_subject->setText(d.subject);
    m_inReplyTo = d.inReplyTo;
    m_references = d.references;
    m_threadId = d.threadId;
    m_quotedText = d.quotedText;
    m_quotedHtml = d.quotedHtml;
    m_body->clear();
    applySignature();
    insertQuote();
    applyStationery(); // chosen before the draft arrived (Reply With)
    m_body->setFocus();
    if (m_to->text().isEmpty()) {
        m_to->setFocus();
    }
}

QByteArray ComposeWindow::saveState() const
{
    QJsonArray files;
    QString err;
    for (const OutgoingAttachment &a : toOutgoing(m_attachments, &err)) { // files read now: they may move before it is sent
        files.append(QJsonObject{{QStringLiteral("name"), a.fileName},
                                 {QStringLiteral("mime"), a.mimeType},
                                 {QStringLiteral("data"), QString::fromLatin1(a.data.toBase64())}});
    }
    const QJsonObject o{{QStringLiteral("to"), m_to->text()},
                        {QStringLiteral("cc"), m_cc->text()},
                        {QStringLiteral("bcc"), m_bcc->text()},
                        {QStringLiteral("subject"), m_subject->text()},
                        {QStringLiteral("format"), int(m_currentFormat)},
                        {QStringLiteral("body"), m_currentFormat == Format::Html ? m_body->toHtml() : m_body->toPlainText()},
                        {QStringLiteral("priority"), m_priority->currentIndex()},
                        {QStringLiteral("inReplyTo"), m_inReplyTo},
                        {QStringLiteral("references"), QJsonArray::fromStringList(m_references)},
                        {QStringLiteral("threadId"), m_threadId},
                        {QStringLiteral("draftId"), m_draftId},
                        {QStringLiteral("attachments"), files}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool ComposeWindow::restoreState(const QByteArray &state)
{
    const QJsonDocument doc = QJsonDocument::fromJson(state);
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    m_stationery = {};
    m_quotedText.clear();
    m_quotedHtml.clear();
    m_to->setText(o.value(QStringLiteral("to")).toString());
    m_cc->setText(o.value(QStringLiteral("cc")).toString());
    m_bcc->setText(o.value(QStringLiteral("bcc")).toString());
    m_subject->setText(o.value(QStringLiteral("subject")).toString());
    m_inReplyTo = o.value(QStringLiteral("inReplyTo")).toString();
    m_references.clear();
    for (const auto &v : o.value(QStringLiteral("references")).toArray()) {
        m_references << v.toString();
    }
    m_threadId = o.value(QStringLiteral("threadId")).toString();
    m_draftId = o.value(QStringLiteral("draftId")).toString();
    m_priority->setCurrentIndex(std::clamp(o.value(QStringLiteral("priority")).toInt(), 0, m_priority->count() - 1));
    const Format format = Format(std::clamp(o.value(QStringLiteral("format")).toInt(), 0, 2));
    // The body as it was, signature and quote included: nothing is added again.
    if (m_signature) {
        const QSignalBlocker block(m_signature);
        m_signature->setCurrentIndex(0); // "None": the saved text already has whatever was signed
    }
    m_body->clear();
    setFormat(format);
    m_body->clear();
    const QString body = o.value(QStringLiteral("body")).toString();
    if (format == Format::Html) {
        m_body->setHtml(body);
    } else {
        m_body->setPlainText(body);
    }
    m_attachments.clear();
    for (const auto &v : o.value(QStringLiteral("attachments")).toArray()) {
        const QJsonObject f = v.toObject();
        const QByteArray data = QByteArray::fromBase64(f.value(QStringLiteral("data")).toString().toLatin1());
        addAttachment({f.value(QStringLiteral("name")).toString(), data.size(), {}, data, f.value(QStringLiteral("mime")).toString()});
    }
    m_body->document()->setModified(false);
    m_to->setModified(false);
    m_subject->setModified(false);
    return true;
}

bool ComposeWindow::saveAsStationery(const QString &name)
{
    const Stationery s = asStationery(name);
    if (s.name.isEmpty() || !StationeryStore().save(s)) {
        fail(tr("Couldn't save the stationery."));
        return false;
    }
    statusBar()->showMessage(tr("Saved as stationery \u201c%1\u201d.").arg(s.name), 5000);
    emit stationerySaved(s.name);
    return true;
}

void ComposeWindow::setStationery(const Stationery &s)
{
    m_stationery = s;
    applyStationery();
}

void ComposeWindow::applyStationery()
{
    if (m_stationery.name.isEmpty()) {
        return;
    }
    // A reply already knows who it is to and what it is about.
    if (m_to->text().trimmed().isEmpty()) {
        m_to->setText(m_stationery.to);
    }
    if (m_cc->text().trimmed().isEmpty()) {
        m_cc->setText(m_stationery.cc);
    }
    if (m_subject->text().trimmed().isEmpty()) {
        m_subject->setText(m_stationery.subject);
    }
    if (!m_stationery.body.isEmpty()) {
        QTextCursor c(m_body->document());
        c.movePosition(QTextCursor::Start);
        // Its own untagged block(s), above the signature and the quote. The
        // block that was first keeps its format (it may be the signature's).
        c.insertBlock(c.blockFormat(), c.blockCharFormat());
        c.movePosition(QTextCursor::Start);
        c.setBlockFormat(QTextBlockFormat());
        c.setBlockCharFormat(QTextCharFormat());
        c.insertText(m_stationery.body, QTextCharFormat()); // plain text, never parsed as HTML
        m_body->setTextCursor(c);
    }
    m_body->document()->setModified(true);
}

Stationery ComposeWindow::asStationery(const QString &name) const
{
    Stationery s;
    s.name = name.trimmed();
    s.to = m_to->text().trimmed();
    s.cc = m_cc->text().trimmed();
    s.subject = m_subject->text();
    QStringList lines;
    for (QTextBlock b = m_body->document()->begin(); b.isValid(); b = b.next()) {
        if (b.blockFormat().hasProperty(kSigProp) || b.blockFormat().hasProperty(kQuoteProp)) {
            continue;
        }
        lines << b.text();
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    s.body = lines.join(QLatin1Char('\n'));
    return s;
}

void ComposeWindow::setMailto(const MailtoFields &f)
{
    ComposeDraft d;
    d.to = f.to;
    d.cc = f.cc;
    d.bcc = f.bcc;
    d.subject = f.subject;
    setDraft(d);
    if (!f.body.isEmpty()) {
        QTextCursor c(m_body->document());
        c.movePosition(QTextCursor::Start);
        c.insertText(f.body); // plain text, never parsed as HTML
        c.movePosition(QTextCursor::Start);
        m_body->setTextCursor(c);
    }
}

void ComposeWindow::attachFromMessage(const QString &gmailMessageId)
{
    if (!m_session || !m_session->api()) {
        return;
    }
    QPointer<ComposeWindow> self(this);
    GmailClient *api = m_session->api();
    statusBar()->showMessage(tr("Fetching attachments\u2026"));
    api->getMessageFull(gmailMessageId, [self, api, gmailMessageId](const QJsonObject &json, const ApiError &err) {
        if (!self) {
            return;
        }
        if (err.isError) {
            self->fail(tr("Couldn't fetch the original's attachments: %1").arg(err.message));
            return;
        }
        const auto body = MessageParser::bodyFromFull(json);
        for (const auto &ref : body.attachmentRefs) {
            if (ref.attachmentId.isEmpty()) {
                self->addAttachment({ref.fileName, ref.inlineData.size(), {}, ref.inlineData, ref.mimeType});
                continue;
            }
            api->getAttachment(gmailMessageId, ref.attachmentId, [self, ref](const QJsonObject &a, const ApiError &e) {
                if (!self) {
                    return;
                }
                if (e.isError) {
                    self->fail(tr("Couldn't fetch %1: %2").arg(ref.fileName, e.message));
                    return;
                }
                const QByteArray data = MessageParser::decodeBase64Url(a.value(QStringLiteral("data")).toString());
                self->addAttachment({ref.fileName, data.size(), {}, data, ref.mimeType});
                self->statusBar()->showMessage(tr("Attached %1").arg(ref.fileName), 3000);
            });
        }
        if (body.attachmentRefs.isEmpty()) {
            self->statusBar()->clearMessage();
        }
    });
}

// An address field as it will be sent: nicknames written out, even if the
// field was never left (Ctrl+Enter straight from To).
QString ComposeWindow::recipients(const QLineEdit *field) const
{
    const QString text = field->text().trimmed();
    ContactStore *store = m_session ? m_session->contacts() : nullptr;
    return store && store->isOpen() ? store->expandRecipients(text) : text;
}

OutgoingMessage ComposeWindow::message() const
{
    OutgoingMessage m;
    m.from = m_from->text().trimmed();
    m.to = recipients(m_to);
    m.cc = recipients(m_cc);
    m.bcc = recipients(m_bcc);
    m.subject = m_subject->text();
    m.inReplyTo = m_inReplyTo;
    m.references = m_references;
    m.threadId = m_threadId;
    m.messageId = m_messageId;
    m.priority = m_priority->currentIndex() == 1 ? OutgoingMessage::Priority::High
               : m_priority->currentIndex() == 2 ? OutgoingMessage::Priority::Low
                                                 : OutgoingMessage::Priority::Normal;
    switch (m_currentFormat) {
    case Format::Html:
        m.text = richtext::toPlainText(m_body->document());
        m.html = m_body->toHtml();
        break;
    case Format::Plain:
        m.text = m_body->toPlainText();
        break;
    case Format::Markdown: {
        const QString src = m_body->toPlainText();
        m.html = markdown::toEmailHtml(src);
        QStringList lines = markdown::toPlainText(src).split(QLatin1Char('\n'));
        for (QString &l : lines) {
            while (l.endsWith(QLatin1Char(' '))) {
                l.chop(1);
            }
            if (l == QLatin1String("--")) {
                l = QStringLiteral("-- "); // RFC 3676 signature delimiter
            }
        }
        m.text = lines.join(QLatin1Char('\n'));
        break;
    }
    }
    QString err;
    m.attachments = toOutgoing(m_attachments, &err);
    return m;
}

QByteArray ComposeWindow::buildMime() const
{
    return MimeBuilder::build(message());
}

bool ComposeWindow::validate(QString *why) const
{
    const QStringList all = MimeBuilder::splitAddresses(recipients(m_to)) + MimeBuilder::splitAddresses(recipients(m_cc)) +
                            MimeBuilder::splitAddresses(recipients(m_bcc));
    if (all.isEmpty()) {
        *why = tr("Add at least one recipient.");
        return false;
    }
    for (const QString &a : all) {
        const QString addr = MessageParser::splitAddress(a).second;
        const int at = addr.indexOf(QLatin1Char('@'));
        if (at <= 0 || at == addr.size() - 1 || addr.contains(QLatin1Char(' '))) {
            *why = tr("\u201c%1\u201d doesn't look like an email address.").arg(a.trimmed());
            return false;
        }
    }
    return true;
}

void ComposeWindow::setBusy(bool busy, const QString &status)
{
    m_busy = busy;
    m_saveDraft->setEnabled(!busy);
    updateSizeMeter();
    if (!status.isEmpty()) {
        statusBar()->showMessage(status);
    } else {
        statusBar()->clearMessage();
    }
}

void ComposeWindow::fail(const QString &message)
{
    m_lastError = message;
    qCWarning(lcGmail) << "compose:" << message;
    updateBanner();
    emit sendFailed(message);
}

void ComposeWindow::send()
{
    if (m_busy) {
        return;
    }
    m_lastError.clear();
    QString why;
    if (!validate(&why)) {
        fail(why);
        return;
    }
    if (sizeLevel() == limits::SizeLevel::Blocked) {
        fail(tr("Remove attachments (or zip them) to send."));
        return;
    }
    QString err;
    toOutgoing(m_attachments, &err);
    if (!err.isEmpty()) {
        fail(err);
        return;
    }
    const QByteArray mime = buildMime();
    if (limits::classifySendSize(mime.size()) == limits::SizeLevel::Blocked) {
        fail(tr("The message is %1 once encoded, over Gmail's %2 MB limit.")
                 .arg(formatBytes(mime.size()))
                 .arg(limits::kSendLimitBytes / 1'000'000));
        return;
    }
    if (!m_session || !m_session->sender()) {
        fail(tr("Sign in to Gmail to send."));
        return;
    }
    setBusy(true, tr("Sending\u2026"));
    Sender *sender = m_session->sender();
    m_progressConn = connect(sender, &Sender::progress, this, [this](qint64 s, qint64 t) {
        if (t > 0) {
            statusBar()->showMessage(tr("Sending\u2026 %1%").arg(s * 100 / t));
        }
    });
    QPointer<ComposeWindow> self(this);
    sender->send(mime, m_threadId, [self, sender](const Sender::Result &r) {
        if (!self) {
            return;
        }
        disconnect(self->m_progressConn);
        self->setBusy(false);
        if (!r.ok) {
            self->fail(r.blocked ? r.err.message
                                 : tr("Gmail didn't accept the message: %1")
                                       .arg(r.err.message.isEmpty() ? tr("HTTP %1").arg(r.err.httpStatus) : r.err.message));
            return;
        }
        self->m_sent = true;
        if (self->m_queuedId > 0 && self->m_session && self->m_session->cache()) {
            self->m_session->cache()->removeQueued(self->m_queuedId); // sent from here instead of from the queue
            self->m_queuedId = 0;
        }
        if (!self->m_draftId.isEmpty()) {
            sender->deleteDraft(self->m_draftId, [](const Sender::Result &) {});
        }
        if (self->m_session) {
            self->m_session->syncSoon();
        }
        emit self->sent(r.messageId, r.threadId);
        self->close();
    });
}

// Send Later: the finished message goes into the queue in Out (as in Eudora)
// and leaves with File > Send Queued Messages. Checked exactly as Send
// checks it, so nothing waits in the queue that can't be sent.
void ComposeWindow::queue()
{
    if (m_busy) {
        return;
    }
    m_lastError.clear();
    QString why;
    if (!validate(&why)) {
        fail(why);
        return;
    }
    if (sizeLevel() == limits::SizeLevel::Blocked) {
        fail(tr("Remove attachments (or zip them) to send."));
        return;
    }
    QString err;
    toOutgoing(m_attachments, &err);
    if (!err.isEmpty()) {
        fail(err);
        return;
    }
    const QByteArray mime = buildMime();
    if (limits::classifySendSize(mime.size()) == limits::SizeLevel::Blocked) {
        fail(tr("The message is %1 once encoded, over Gmail's %2 MB limit.")
                 .arg(formatBytes(mime.size()))
                 .arg(limits::kSendLimitBytes / 1'000'000));
        return;
    }
    MailCache *cache = m_session ? m_session->cache() : nullptr;
    if (!cache) {
        fail(tr("Sign in to Gmail to queue mail."));
        return;
    }
    const OutgoingMessage m = message();
    MailCache::QueuedMessage q;
    q.mime = mime;
    q.threadId = m_threadId;
    q.draftId = m_draftId;
    q.to = m.to;
    q.cc = m.cc;
    q.subject = m.subject;
    q.text = m.text;
    q.state = saveState();
    if (cache->addQueued(q) <= 0) {
        fail(tr("Couldn't put the message in the queue."));
        return;
    }
    if (m_queuedId > 0) {
        cache->removeQueued(m_queuedId); // the version this window was opened from
        m_queuedId = 0;
    }
    m_sent = true; // nothing left to save or ask about on close
    emit queued();
    close();
}

void ComposeWindow::saveDraft()
{
    if (m_busy) {
        return;
    }
    if (!m_session || !m_session->sender()) {
        fail(tr("Sign in to Gmail to save drafts."));
        return;
    }
    m_lastError.clear();
    const QByteArray mime = buildMime();
    setBusy(true, tr("Saving draft\u2026"));
    QPointer<ComposeWindow> self(this);
    m_session->sender()->saveDraft(mime, m_threadId, m_draftId, [self](const Sender::Result &r) {
        if (!self) {
            return;
        }
        self->setBusy(false);
        if (!r.ok) {
            self->m_closeAfterSave = false;
            self->fail(tr("Couldn't save the draft: %1").arg(r.err.message));
            return;
        }
        self->m_draftId = r.draftId;
        if (self->m_threadId.isEmpty()) {
            self->m_threadId = r.threadId;
        }
        self->m_body->document()->setModified(false);
        self->statusBar()->showMessage(tr("Draft saved"), 4000);
        emit self->draftSaved(r.draftId);
        if (self->m_closeAfterSave) {
            self->m_closeAfterSave = false;
            self->m_confirmClose = false;
            self->close();
        }
    });
}

void ComposeWindow::closeEvent(QCloseEvent *e)
{
    const bool dirty = m_body->document()->isModified() || m_to->isModified() || m_subject->isModified() ||
                       (!m_attachments.isEmpty() && m_draftId.isEmpty());
    if (m_confirmClose && !m_sent && m_session && dirty) {
        const auto ans = QMessageBox::question(this, tr("Save draft?"),
                                               tr("Save this message to Drafts before closing?"),
                                               QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                               QMessageBox::Save);
        if (ans == QMessageBox::Cancel) {
            e->ignore();
            return;
        }
        if (ans == QMessageBox::Save) {
            m_closeAfterSave = true;
            saveDraft();
            e->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(e);
}

// ---- sample ----------------------------------------------------------------------

void ComposeWindow::loadSampleReply()
{
    m_to->setText(QStringLiteral("Priya Raman <priya.raman@example.com>"));
    m_from->setText(QStringLiteral("Alex Morgan <alex.morgan@example.com>"));
    m_subject->setText(QStringLiteral("Re: Q4 budget review \u2014 numbers attached"));
    m_cc->setText(QStringLiteral("Hannah Lindqvist <h.lindqvist@example.com>"));
    m_bcc->clear();
    const QPalette pal = QApplication::palette();
    const QString quote = pal.color(QPalette::PlaceholderText).name();
    m_signature->setCurrentIndex(m_signature->count() - 1); // the sample has its own
    m_body->setHtml(QStringLiteral(
        "<p>Hi Priya,</p>"
        "<p>Thanks, this looks good. A few notes on the draft:</p>"
        "<ul><li><b>Tab 2:</b> the travel line still shows the old per-diem.</li>"
        "<li><b>Tab 3:</b> can we split contractor hours by quarter?</li>"
        "<li>I've attached the <i>site estimate</i> Hannah sent over.</li></ul>"
        "<p>Happy to go through it before Monday's review.</p>"
        "<p>-- <br>Alex Morgan<br><span style='color:%1'>Field Operations \u00b7 (555) 014-2290</span></p>"
        "<p style='color:%1'>On 10/3/26 8:12 PM, Priya Raman wrote:</p>"
        "<blockquote style='color:%1; margin-left:8px; border-left:3px solid %1; padding-left:8px'>"
        "Attached is the Q4 budget draft with the revised travel line. Can you look over tabs 2 and 3 "
        "before Monday's review?</blockquote>")
                         .arg(quote));
    Attachment a, b;
    a.name = QStringLiteral("site-estimate.pdf");
    a.bytes = 1'184'512;
    b.name = QStringLiteral("Q4-budget-draft.xlsx");
    b.bytes = 421'880;
    setAttachments({a, b});
    updateModeLabel();
}
