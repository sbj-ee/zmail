#include "ComposeWindow.h"

#include "Icons.h"
#include "Theme.h"
#include "version.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFontComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QStatusBar>
#include <QTextEdit>
#include <QTextList>
#include <QToolBar>
#include <QVBoxLayout>

using zmail::ui::icon;
namespace limits = zmail::limits;

ComposeWindow::ComposeWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("composeWindow"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    buildToolbar();
    addToolBarBreak();
    buildFormatBar();

    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    buildHeaderBlock(central);
    v->addWidget(m_headerBlock);

    m_body = new QTextEdit(central);
    m_body->setObjectName(QStringLiteral("composeBody"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setAcceptRichText(true);
    m_body->document()->setDocumentMargin(14);
    v->addWidget(m_body, 1);
    setCentralWidget(central);

    m_sizeMeter = new QProgressBar(this);
    m_sizeMeter->setObjectName(QStringLiteral("sizeMeter"));
    m_sizeMeter->setRange(0, 1000);
    m_sizeMeter->setTextVisible(true);
    m_sizeMeter->setFixedWidth(300);
    m_sizeMeter->setMaximumHeight(16);
    m_modeLabel = new QLabel(tr("HTML \u00b7 Spell check on \u00b7 Signature: Work"), this);
    statusBar()->addWidget(m_modeLabel, 1);
    statusBar()->addPermanentWidget(new QLabel(tr("Size:"), this));
    statusBar()->addPermanentWidget(m_sizeMeter);

    connect(m_subject, &QLineEdit::textChanged, this, &ComposeWindow::updateTitle);
    connect(m_body, &QTextEdit::textChanged, this, &ComposeWindow::updateSizeMeter);
    updateTitle();
    updateSizeMeter();
    resize(860, 640);
}

void ComposeWindow::buildToolbar()
{
    auto *tb = addToolBar(tr("Message"));
    tb->setObjectName(QStringLiteral("composeToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_send = tb->addAction(icon(QStringLiteral("send")), tr("Send"));
    m_send->setObjectName(QStringLiteral("actionSend"));
    QAction *later = tb->addAction(icon(QStringLiteral("clock")), tr("Send Later\u2026"));
    later->setObjectName(QStringLiteral("actionSendLater"));
    tb->addSeparator();
    QAction *attach = tb->addAction(icon(QStringLiteral("paperclip")), tr("Attach"));
    attach->setObjectName(QStringLiteral("actionComposeAttach"));
    QAction *spell = tb->addAction(icon(QStringLiteral("spell-check")), tr("Spelling"));
    spell->setCheckable(true);
    spell->setChecked(true);
    tb->addSeparator();

    tb->addWidget(new QLabel(tr(" Signature "), tb));
    m_signature = new QComboBox(tb);
    m_signature->setObjectName(QStringLiteral("signatureCombo"));
    m_signature->addItems({tr("Work"), tr("Personal"), tr("None")});
    tb->addWidget(m_signature);
    tb->addWidget(new QLabel(tr("  Format "), tb));
    m_format = new QComboBox(tb);
    m_format->setObjectName(QStringLiteral("formatCombo"));
    m_format->addItems({tr("HTML"), tr("Plain text"), tr("Markdown")});
    tb->addWidget(m_format);
    tb->addWidget(new QLabel(tr("  Priority "), tb));
    auto *prio = new QComboBox(tb);
    prio->setObjectName(QStringLiteral("priorityCombo"));
    prio->addItems({tr("Normal"), tr("High"), tr("Low")});
    tb->addWidget(prio);
}

void ComposeWindow::buildFormatBar()
{
    auto *fb = addToolBar(tr("Format"));
    fb->setObjectName(QStringLiteral("formatToolBar"));
    fb->setMovable(false);
    fb->setIconSize(QSize(16, 16));

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
    add("actionTextColor", "baseline", tr("Text colour"));
    fb->addSeparator();
    QAction *bullets = add("actionBullets", "list", tr("Bulleted list"));
    QAction *numbers = add("actionNumbers", "list-ordered", tr("Numbered list"));
    fb->addSeparator();
    QAction *left = add("actionAlignLeft", "text-align-start", tr("Align left"));
    QAction *center = add("actionAlignCenter", "text-align-center", tr("Centre"));
    QAction *right = add("actionAlignRight", "text-align-end", tr("Align right"));
    fb->addSeparator();
    add("actionLink", "link", tr("Insert link"));
    add("actionQuote", "quote", tr("Quote"));

    connect(bold, &QAction::toggled, this, [this](bool on) { m_body->setFontWeight(on ? QFont::Bold : QFont::Normal); });
    connect(italic, &QAction::toggled, this, [this](bool on) { m_body->setFontItalic(on); });
    connect(underline, &QAction::toggled, this, [this](bool on) { m_body->setFontUnderline(on); });
    connect(bullets, &QAction::triggered, this, [this]() { m_body->textCursor().createList(QTextListFormat::ListDisc); });
    connect(numbers, &QAction::triggered, this, [this]() { m_body->textCursor().createList(QTextListFormat::ListDecimal); });
    connect(left, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignLeft); });
    connect(center, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignHCenter); });
    connect(right, &QAction::triggered, this, [this]() { m_body->setAlignment(Qt::AlignRight); });
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

void ComposeWindow::setAttachments(const QList<Attachment> &list)
{
    m_attachments = list;
    auto *h = static_cast<QHBoxLayout *>(m_attached->layout());
    while (QLayoutItem *it = h->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QString dim = QApplication::palette().color(QPalette::PlaceholderText).name();
    if (list.isEmpty()) {
        h->addWidget(new QLabel(tr("<span style='color:%1'>(none)</span>").arg(dim), m_attached));
    }
    for (const Attachment &a : list) {
        auto *chip = new QFrame(m_attached);
        chip->setObjectName(QStringLiteral("attachmentChip"));
        chip->setFrameShape(QFrame::StyledPanel);
        auto *ch = new QHBoxLayout(chip);
        ch->setContentsMargins(6, 1, 8, 1);
        ch->setSpacing(4);
        auto *ic = new QLabel(chip);
        ic->setPixmap(icon(QStringLiteral("paperclip")).pixmap(14, 14));
        ch->addWidget(ic);
        ch->addWidget(new QLabel(QStringLiteral("%1 <span style='color:%2'>(%3 K)</span>")
                                     .arg(a.name.toHtmlEscaped(), dim,
                                          QLocale(QLocale::English).toString((a.bytes + 1023) / 1024)),
                                 chip));
        h->addWidget(chip);
    }
    h->addStretch(1);
    updateSizeMeter();
}

qint64 ComposeWindow::encodedSize() const
{
    // Headers + HTML part + generated text/plain part, then base64 attachments.
    qint64 total = 2048;
    if (m_body) {
        const qint64 html = m_body->toHtml().toUtf8().size();
        const qint64 text = m_body->toPlainText().toUtf8().size();
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
    const double mb = double(size) / 1e6;
    const double limitMb = double(limits::kSendLimitBytes) / 1e6;
    m_sizeMeter->setValue(int(std::min<qint64>(1000, size * 1000 / limits::kSendLimitBytes)));
    m_sizeMeter->setFormat(tr("%1 MB of %2 MB (encoded)").arg(mb, 0, 'f', 1).arg(limitMb, 0, 'f', 0));

    const auto level = limits::classifySendSize(size);
    const QString chunk = level == limits::SizeLevel::Blocked ? QStringLiteral("#d32f2f")
                        : level == limits::SizeLevel::Warn ? QStringLiteral("#f0a020")
                                                           : QStringLiteral("#43a047");
    m_sizeMeter->setStyleSheet(QStringLiteral("QProgressBar{border:1px solid palette(mid);border-radius:3px;"
                                              "text-align:center;}"
                                              "QProgressBar::chunk{background:%1;border-radius:2px;}")
                                   .arg(chunk));
    m_send->setEnabled(level != limits::SizeLevel::Blocked);
    m_sizeMeter->setToolTip(level == limits::SizeLevel::Blocked
                                ? tr("Over Gmail's 25 MB limit after encoding. Remove attachments to send.")
                                : tr("Encoded size, counting base64 overhead (about 37%)."));
}

void ComposeWindow::updateTitle()
{
    const QString subj = m_subject->text().trimmed();
    setWindowTitle(QStringLiteral("%1 \u2014 zmail %2")
                       .arg(subj.isEmpty() ? tr("New Message") : subj,
                            QString::fromLatin1(zmail::kVersionString)));
}

void ComposeWindow::loadSampleReply()
{
    m_to->setText(QStringLiteral("Priya Raman <priya.raman@example.com>"));
    m_from->setText(QStringLiteral("Alex Morgan <alex.morgan@example.com>"));
    m_subject->setText(QStringLiteral("Re: Q4 budget review \u2014 numbers attached"));
    m_cc->setText(QStringLiteral("Hannah Lindqvist <h.lindqvist@example.com>"));
    m_bcc->clear();
    const QPalette pal = QApplication::palette();
    const QString quote = pal.color(QPalette::PlaceholderText).name();
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
    setAttachments({{QStringLiteral("site-estimate.pdf"), 1'184'512},
                    {QStringLiteral("Q4-budget-draft.xlsx"), 421'880}});
}
