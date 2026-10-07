#include "MessageView.h"

#include "HtmlFit.h"
#include "RemoteImages.h"
#include "SafeHtmlView.h"
#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace zmail::ui {

namespace {

const char *kZoomKey = "viewer/zoom";
const char *kDarkKey = "viewer/darkMail";
constexpr int kPlainTextColumns = 78; // comfortable measure for plain-text mail
constexpr qreal kMargin = 16;

QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

QString linkify(const QString &escaped)
{
    // Stops before an escaped <, >, " or ' too: "Docs <https://x/y>" (the
    // usual plain-text link form, also zmail's plain signatures) links
    // https://x/y, not https://x/y&gt.
    static const QRegularExpression url(
        QStringLiteral("(https?://(?:(?!&(?:gt|lt|quot|#0?39|#x27);)[^\\s<>\"'])*[^\\s<>\"'.,;:!?)\\]&])"));
    QString out = escaped;
    out.replace(url, QStringLiteral("<a href=\"\\1\">\\1</a>"));
    return out;
}

QFont zoomed(QFont f, qreal z)
{
    if (f.pointSizeF() > 0) {
        f.setPointSizeF(f.pointSizeF() * z);
    } else if (f.pixelSize() > 0) {
        f.setPixelSize(std::max(1, qRound(f.pixelSize() * z)));
    }
    return f;
}

QLabel *valueLabel(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return l;
}

} // namespace

MessageView::MessageView(QWidget *parent)
    : QWidget(parent)
{
    QSettings settings;
    m_zoom = std::clamp(settings.value(QLatin1String(kZoomKey), 1.0).toDouble(), kMinZoom, kMaxZoom);
    m_dark = settings.value(QLatin1String(kDarkKey), false).toBool();

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_warning = new QLabel(this);
    m_warning->setObjectName(QStringLiteral("messageWarning"));
    m_warning->setTextFormat(Qt::RichText);
    m_warning->setWordWrap(true);
    m_warning->setMargin(8);
    m_warning->hide();
    lay->addWidget(m_warning);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("messageHeader"));
    m_header->setAutoFillBackground(true);
    m_header->setBackgroundRole(QPalette::AlternateBase);
    auto *hv = new QVBoxLayout(m_header);
    hv->setContentsMargins(12, 8, 12, 8);
    hv->setSpacing(4);
    m_subject = valueLabel(m_header);
    m_subject->setObjectName(QStringLiteral("messageSubject"));
    QFont sf = m_subject->font();
    sf.setBold(true);
    sf.setPointSizeF(sf.pointSizeF() * 1.25);
    m_subject->setFont(sf);
    hv->addWidget(m_subject);
    m_headerGrid = new QGridLayout;
    m_headerGrid->setHorizontalSpacing(10);
    m_headerGrid->setVerticalSpacing(2);
    m_headerGrid->setColumnStretch(1, 1);
    hv->addLayout(m_headerGrid);
    lay->addWidget(m_header);

    m_attachments = new QLabel(this);
    m_attachments->setObjectName(QStringLiteral("messageAttachments"));
    m_attachments->setTextFormat(Qt::PlainText);
    m_attachments->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_attachments->setWordWrap(true);
    m_attachments->setContentsMargins(12, 4, 12, 6);
    m_attachments->setAutoFillBackground(true);
    m_attachments->setBackgroundRole(QPalette::AlternateBase);
    m_attachments->hide();
    lay->addWidget(m_attachments);

    m_imagesBar = new QFrame(this);
    m_imagesBar->setObjectName(QStringLiteral("remoteImagesBar"));
    m_imagesBar->setFrameShape(QFrame::StyledPanel);
    m_imagesBar->setAutoFillBackground(true);
    m_imagesBar->setBackgroundRole(QPalette::ToolTipBase);
    m_imagesBar->setForegroundRole(QPalette::ToolTipText);
    auto *ib = new QHBoxLayout(m_imagesBar);
    ib->setContentsMargins(12, 4, 8, 4);
    m_imagesText = new QLabel(m_imagesBar);
    m_imagesText->setForegroundRole(QPalette::ToolTipText);
    m_imagesText->setWordWrap(true); // Ask is the default: don't hold a narrow pane wide
    ib->addWidget(m_imagesText, 1);
    m_loadImages = new QPushButton(tr("Load images"), m_imagesBar);
    m_loadImages->setObjectName(QStringLiteral("loadImagesButton"));
    m_loadImages->setToolTip(tr("Download this message's remote images. The sender can see that you opened it."));
    ib->addWidget(m_loadImages);
    m_alwaysForSender = new QPushButton(tr("Always for this sender"), m_imagesBar);
    m_alwaysForSender->setObjectName(QStringLiteral("alwaysForSenderButton"));
    m_alwaysForSender->setToolTip(
        tr("Load remote images now and in future mail from this address. Edit the list in Settings \u203a Privacy."));
    ib->addWidget(m_alwaysForSender);
    connect(m_alwaysForSender, &QPushButton::clicked, this, &MessageView::alwaysLoadForSender);
    m_imagesBar->hide();
    lay->addWidget(m_imagesBar);
    connect(m_loadImages, &QPushButton::clicked, this, &MessageView::loadImages);

    m_layoutBar = new QFrame(this);
    m_layoutBar->setObjectName(QStringLiteral("simplifiedLayoutBar"));
    m_layoutBar->setFrameShape(QFrame::StyledPanel);
    m_layoutBar->setAutoFillBackground(true);
    m_layoutBar->setBackgroundRole(QPalette::ToolTipBase);
    m_layoutBar->setForegroundRole(QPalette::ToolTipText);
    auto *lb = new QHBoxLayout(m_layoutBar);
    lb->setContentsMargins(12, 4, 8, 4);
    auto *layoutText = new QLabel(tr("This message's layout is very complex. It is shown simplified so zmail stays responsive."),
                                  m_layoutBar);
    layoutText->setForegroundRole(QPalette::ToolTipText);
    layoutText->setWordWrap(true);
    lb->addWidget(layoutText, 1);
    auto *full = new QPushButton(tr("Show full layout"), m_layoutBar);
    full->setObjectName(QStringLiteral("fullLayoutButton"));
    full->setToolTip(tr("Lay the message out as it was sent. This can take a while, and zmail won't respond until it is done."));
    lb->addWidget(full);
    connect(full, &QPushButton::clicked, this, &MessageView::showFullLayout);
    m_layoutBar->hide();
    lay->addWidget(m_layoutBar);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    lay->addWidget(line);

    m_body = new SafeHtmlView(this);
    connect(m_body, &SafeHtmlView::mailtoActivated, this, &MessageView::mailtoRequested);
    m_body->setObjectName(QStringLiteral("messageBody"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setOpenLinks(false);
    // QTextEdit's default (WrapAtWordBoundaryOrAnywhere) lets a table cell's
    // minimum width fall to one character, so a squeezed column broke text
    // one letter per line. Break only between words; zoom-to-fit handles
    // anything that still can't fit.
    m_body->setWordWrapMode(QTextOption::WordWrap);
    m_body->document()->setDocumentMargin(kMargin);
    m_body->viewport()->installEventFilter(this); // Ctrl+wheel zoom
    m_body->installEventFilter(this);             // pane resizes -> relayout
    lay->addWidget(m_body, 1);
    applyBodyPalette();

    m_relayout = new QTimer(this);
    m_relayout->setSingleShot(true);
    m_relayout->setInterval(90);
    connect(m_relayout, &QTimer::timeout, this, &MessageView::render);
    connect(m_body, &SafeHtmlView::remoteImageArrived, this, [this]() { m_relayout->start(150); });

    clear();
}

void MessageView::applyBodyPalette()
{
    // Mail is designed for white paper: dark text on a light page, whatever
    // the app theme. Dark mode (optional) inverts the mail's own colours.
    QPalette p = m_body->palette();
    const QColor base = m_dark ? QColor(0x20, 0x21, 0x24) : QColor(Qt::white);
    const QColor text = m_dark ? QColor(0xe8, 0xea, 0xed) : QColor(0x20, 0x21, 0x24);
    const QColor link = m_dark ? QColor(0x8a, 0xb4, 0xf8) : QColor(0x1a, 0x56, 0xc4);
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Base, base);
        p.setColor(group, QPalette::Window, base);
        p.setColor(group, QPalette::Text, text);
        p.setColor(group, QPalette::WindowText, text);
        p.setColor(group, QPalette::Link, link);
        p.setColor(group, QPalette::LinkVisited, link);
    }
    m_body->setPalette(p);
    m_body->viewport()->setPalette(p);
    // QTextDocument bakes the *application* palette's link colour into <a>
    // when the HTML is parsed, so a brand theme's gold link (or the dark
    // theme's light blue) would land on the white page. Mail's own link
    // styles still win over this default.
    m_body->document()->setDefaultStyleSheet(QStringLiteral("a { color: %1; }").arg(link.name()));
    m_body->viewport()->setAutoFillBackground(true);
}

void MessageView::clear()
{
    m_msg = {};
    m_empty = true;
    m_showImages = false;
    m_blocked = 0;
    m_body->setRemoteImagesAllowed(false);
    m_body->clear();
    m_warning->hide();
    m_header->hide();
    m_attachments->hide();
    m_imagesBar->hide();
    m_layoutBar->hide();
    m_simplified = false;
    m_fullLayout = false;
}

void MessageView::setMessage(const ViewMessage &m)
{
    const bool same = !m_empty && !m.id.isEmpty() && m.id == m_msg.id;
    if (!same) {
        // Settings > Privacy > Remote images: Always (default), Ask (the bar,
        // or a sender on the allow list), Never.
        m_showImages = RemoteImages::shouldLoadFor(m.from);
        m_body->clearRemoteImages();
        m_body->setRemoteImagesAllowed(m_showImages);
        m_fullLayout = false;
    }
    m_msg = m;
    m_empty = false;
    renderHeader();
    render();
    if (!same) {
        m_body->verticalScrollBar()->setValue(0);
        m_body->horizontalScrollBar()->setValue(0);
    }
}

QString MessageView::headerText() const
{
    QStringList out;
    if (!m_subject->text().isEmpty()) {
        out << m_subject->text();
    }
    for (int r = 0; r < m_headerGrid->rowCount(); ++r) {
        auto *k = m_headerGrid->itemAtPosition(r, 0);
        auto *v = m_headerGrid->itemAtPosition(r, 1);
        auto *kl = k ? qobject_cast<QLabel *>(k->widget()) : nullptr;
        auto *vl = v ? qobject_cast<QLabel *>(v->widget()) : nullptr;
        if (kl && vl) {
            out << kl->text() + QLatin1Char(' ') + vl->text();
        }
    }
    if (m_attachments->isVisibleTo(const_cast<MessageView *>(this))) {
        out << m_attachments->text();
    }
    return out.join(QLatin1Char('\n'));
}

void MessageView::renderHeader()
{
    while (QLayoutItem *it = m_headerGrid->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    if (!m_msg.warning.isEmpty()) {
        QPalette wp = m_warning->palette();
        wp.setColor(QPalette::Window, suspiciousBackground(QApplication::palette()));
        wp.setColor(QPalette::WindowText, suspiciousForeground(QApplication::palette()));
        m_warning->setPalette(wp);
        m_warning->setAutoFillBackground(true);
        m_warning->setText(m_msg.warning);
        m_warning->show();
    } else {
        m_warning->hide();
    }
    m_subject->setText(m_msg.subject.isEmpty() ? tr("(no subject)") : m_msg.subject);
    int row = 0;
    auto add = [&](const QString &key, const QString &value, const QString &obj) {
        if (value.trimmed().isEmpty()) {
            return;
        }
        auto *k = new QLabel(key, m_header);
        k->setForegroundRole(QPalette::PlaceholderText);
        k->setAlignment(Qt::AlignRight | Qt::AlignTop);
        QFont kf = k->font();
        kf.setBold(true);
        k->setFont(kf);
        auto *v = valueLabel(m_header);
        v->setObjectName(obj);
        v->setText(value);
        m_headerGrid->addWidget(k, row, 0);
        m_headerGrid->addWidget(v, row, 1);
        ++row;
    };
    add(tr("From:"), m_msg.from, QStringLiteral("headerFrom"));
    add(tr("To:"), m_msg.to, QStringLiteral("headerTo"));
    add(tr("Cc:"), m_msg.cc, QStringLiteral("headerCc"));
    if (m_msg.date.isValid()) {
        const QDateTime dt = m_msg.date.toLocalTime();
        add(tr("Date:"),
            QLocale(QLocale::English, QLocale::UnitedStates).toString(dt, QStringLiteral("dddd, MMMM d, yyyy 'at' h:mm AP")) +
                QLatin1Char(' ') + dt.timeZoneAbbreviation(),
            QStringLiteral("headerDate"));
    }
    if (!m_msg.label.isEmpty()) {
        add(tr("Label:"), QStringLiteral("\u25a0 ") + m_msg.label, QStringLiteral("headerLabel"));
        if (auto *item = m_headerGrid->itemAtPosition(row - 1, 1)) {
            QPalette lp = item->widget()->palette();
            if (m_msg.labelColor.isValid()) {
                lp.setColor(QPalette::WindowText, m_msg.labelColor);
            }
            item->widget()->setPalette(lp);
        }
    }
    m_header->show();
    if (m_msg.attachments.isEmpty()) {
        m_attachments->hide();
    } else {
        const int n = int(m_msg.attachments.size());
        m_attachments->setText((n == 1 ? tr("\U0001F4CE 1 attachment:  ") : tr("\U0001F4CE %1 attachments:  ").arg(n)) +
                               m_msg.attachments.join(QStringLiteral("  \u00b7  ")));
        m_attachments->show();
    }
}

int MessageView::bodyWidth() const
{
    // The pane's width with room for the vertical scroll bar always set
    // aside, whether or not it's showing. Using the viewport width fed back:
    // each render's setHtml() briefly emptied the document, the scroll bar
    // hid, the viewport widened, that resize queued another render, the
    // scroll bar came back... so a long mail re-rendered ~10x a second,
    // forever, pinning a core while idle (0.3.1). Lines are wrapped at this
    // fixed width too, so the scroll bar coming and going never reflows.
    const int extent = m_body->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_body->verticalScrollBar());
    return std::max(1, m_body->contentsRect().width() - extent);
}

void MessageView::render()
{
    if (m_empty) {
        return;
    }
    // Anything render() itself causes (scroll bars, resizes) must not queue
    // another render.
    const QScopedValueRollback<bool> busy(m_rendering, true);
    ++m_renders;
    QScrollBar *vs = m_body->verticalScrollBar();
    const double ratio = vs->maximum() > 0 ? double(vs->value()) / vs->maximum() : 0.0;
    const int vw = bodyWidth();
    m_renderedWidth = vw;
    QTextDocument *doc = m_body->document();
    const qreal margin = kMargin;
    doc->setDocumentMargin(margin);
    const QFont base = QApplication::font();

    QString prefix;
    if (!m_msg.error.isEmpty()) {
        prefix = QStringLiteral("<p style='color:%1'>%2</p>")
                     .arg(m_dark ? QStringLiteral("#f28b82") : QStringLiteral("#b3261e"),
                          esc(tr("Couldn't load the message: %1").arg(m_msg.error)));
    }
    m_blocked = 0;
    m_trackers = 0;
    m_simplified = false;
    m_body->resetBlocked();
    m_effectiveZoom = m_zoom;

    if (!m_msg.loading && !m_msg.bodyHtml.isEmpty()) {
        m_body->setLineWrapMode(QTextEdit::FixedPixelWidth);
        m_body->setLineWrapColumnOrWidth(vw);
        QString html = SafeHtmlView::sanitize(m_msg.bodyHtml, &m_blocked, m_showImages);
        int tableDepth = HtmlFit::kMaxTableDepth;
        html = prefix + HtmlFit::prepare(html, m_fullLayout ? 0 : HtmlFit::kLayoutBudget, &tableDepth);
        m_simplified = tableDepth < HtmlFit::kMaxTableDepth;
        if (m_showImages && RemoteImages::blockTrackers()) {
            html = SafeHtmlView::dropTrackers(html, &m_trackers); // even in "Always load"
        }
        qreal z = m_zoom;
        for (int pass = 0; pass < 2; ++pass) {
            // setDefaultFont() lays the whole document out again: empty it
            // first, or every render paid for a second layout of the mail
            // it was about to replace (seconds, for deeply nested tables).
            doc->clear();
            doc->setDefaultFont(zoomed(base, z));
            m_body->setHtml(html);
            HtmlFit::Options o;
            o.availableWidth = vw - 2 * margin;
            o.zoom = z;
            o.dark = m_dark;
            HtmlFit::fit(doc, o);
            const qreal w = doc->size().width();
            if (pass == 1 || w <= vw + 2) {
                break;
            }
            // Still wider than the pane (fixed-width content that can't
            // reflow): zoom to fit instead of scrolling sideways.
            z = std::max<qreal>(0.4, z * (vw - 2) / w);
        }
        m_effectiveZoom = z;
    } else {
        const QFont f = zoomed(base, m_zoom);
        doc->clear(); // as above
        doc->setDefaultFont(f);
        QString text = m_msg.bodyText.isEmpty() ? m_msg.snippet : m_msg.bodyText;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")); // CRLF bodies: one line break, not two
        QString html = prefix + QStringLiteral("<div style='white-space:pre-wrap'>%1</div>").arg(linkify(esc(text)));
        if (m_msg.loading) {
            html += QStringLiteral("<p><i>%1</i></p>").arg(tr("Loading message\u2026"));
        }
        const int measure = QFontMetrics(f).averageCharWidth() * kPlainTextColumns + int(2 * margin);
        if (measure < vw) {
            m_body->setLineWrapMode(QTextEdit::FixedPixelWidth);
            m_body->setLineWrapColumnOrWidth(measure);
        } else {
            m_body->setLineWrapMode(QTextEdit::FixedPixelWidth);
            m_body->setLineWrapColumnOrWidth(vw);
        }
        m_body->setHtml(html);
    }

    const RemoteImageMode mode = RemoteImages::mode();
    if (m_blocked > 0 && !m_showImages && !m_msg.loading && mode != RemoteImageMode::Never) {
        m_alwaysForSender->setVisible(mode == RemoteImageMode::Ask && !RemoteImages::senderAddress(m_msg.from).isEmpty());
        m_imagesText->setText(m_blocked == 1 ? tr("1 remote image blocked to protect your privacy.")
                                             : tr("%1 remote images blocked to protect your privacy.").arg(m_blocked));
        m_imagesBar->show();
    } else {
        m_imagesBar->hide();
    }
    m_layoutBar->setVisible(m_simplified && !m_msg.loading);
    if (ratio > 0) {
        vs->setValue(int(std::round(ratio * vs->maximum())));
    }
}

void MessageView::showFullLayout()
{
    if (m_empty || m_fullLayout) {
        return;
    }
    m_fullLayout = true;
    render();
}

void MessageView::alwaysLoadForSender()
{
    RemoteImages::allowSender(m_msg.from);
    loadImages();
}

void MessageView::reloadImagePolicy()
{
    if (m_empty) {
        return;
    }
    const bool want = RemoteImages::shouldLoadFor(m_msg.from);
    // Turning the setting up shows images now; turning it down applies
    // from the next message, except Never, which blocks straight away.
    if (want && !m_showImages) {
        loadImages();
    } else if (!want && m_showImages && RemoteImages::mode() == RemoteImageMode::Never) {
        m_showImages = false;
        m_body->setRemoteImagesAllowed(false);
        render();
    } else {
        render(); // bar contents (mode, tracker setting)
    }
}

void MessageView::loadImages()
{
    if (m_showImages) {
        return;
    }
    m_showImages = true;
    m_body->setRemoteImagesAllowed(true);
    render();
}

void MessageView::setZoom(qreal z)
{
    z = std::clamp(std::round(z * 10.0) / 10.0, kMinZoom, kMaxZoom);
    if (qFuzzyCompare(z, m_zoom)) {
        return;
    }
    m_zoom = z;
    QSettings().setValue(QLatin1String(kZoomKey), m_zoom);
    render();
    emit zoomChanged(m_zoom);
}

void MessageView::zoomIn()
{
    setZoom(m_zoom + 0.1);
}

void MessageView::zoomOut()
{
    setZoom(m_zoom - 0.1);
}

void MessageView::resetZoom()
{
    setZoom(1.0);
}

void MessageView::setDarkMail(bool on)
{
    if (on == m_dark) {
        return;
    }
    m_dark = on;
    QSettings().setValue(QLatin1String(kDarkKey), on);
    applyBodyPalette();
    render();
}

bool MessageView::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_body) {
        if (ev->type() == QEvent::Resize && !m_empty && !m_rendering && std::abs(bodyWidth() - m_renderedWidth) > 4) {
            m_relayout->start();
        }
    } else if (obj == m_body->viewport()) {
        if (ev->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(ev);
            if (we->modifiers() & Qt::ControlModifier) {
                const int dy = we->angleDelta().y();
                if (dy > 0) {
                    zoomIn();
                } else if (dy < 0) {
                    zoomOut();
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void MessageView::changeEvent(QEvent *ev)
{
    QWidget::changeEvent(ev);
    if (ev->type() == QEvent::FontChange && !m_empty && !m_rendering) {
        m_relayout->start();
    }
}

} // namespace zmail::ui
