#pragma once

#include <QColor>
#include <QDateTime>
#include <QStringList>
#include <QWidget>

class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QTextDocument;
class QTimer;

namespace zmail::ui {

class SafeHtmlView;

// What the viewer shows; built from a live CachedMessage or a sample MailItem.
struct ViewMessage
{
    QString id;
    QString from;          // display form, e.g. "Middleton Vet <x@example.com>"
    QString to;
    QString cc;
    QString subject;
    QDateTime date;
    QString label;         // Gmail label swatch, "" = none
    QColor labelColor;
    QStringList attachments;
    QString bodyHtml;
    QString bodyText;
    QString snippet;
    bool loading = false;  // body still being fetched
    QString error;
    QString warning;       // HTML for the spam / phishing banner, "" = none
    // A mailing list that says how to leave it (List-Unsubscribe): the bar
    // with the Unsubscribe button. unsubscribedOn is set once that was done.
    bool canUnsubscribe = false;
    QString unsubscribeTarget; // the list's host or address, for the bar
    QDateTime unsubscribedOn;
    QString calendar;      // an iCalendar invitation in the message, "" = none
};

// Message viewer: header block (From/To/Cc/Date/Subject), attachments row,
// a card for a calendar invitation, the mailing list's Unsubscribe bar,
// "remote images blocked" bar, then the body. HTML mail is drawn on a light
// background like a normal mail client whatever the app theme, optionally
// in a lightness-inverted dark mode; it's laid out to fill the pane
// (HtmlFit), with zoom (Ctrl +/-/0, Ctrl+wheel) remembered in QSettings.
class MessageView : public QWidget
{
    Q_OBJECT

public:
    explicit MessageView(QWidget *parent = nullptr);

    void setMessage(const ViewMessage &m);
    const ViewMessage &message() const { return m_msg; }
    void clear();

    SafeHtmlView *body() const { return m_body; }
    QString headerText() const; // header block as plain text (tests, accessibility)
    // The calendar card as plain text, one fact per line ("" = no card).
    QString calendarText() const;

    qreal zoom() const { return m_zoom; }
    // Zoom actually applied to the last HTML render: zoom() times any
    // zoom-to-fit reduction for mail too wide for the pane.
    qreal effectiveZoom() const { return m_effectiveZoom; }
    // Body renders so far (tests: an idle view must not keep re-rendering).
    int renderCount() const { return m_renders; }
    void setZoom(qreal z);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    static constexpr qreal kMinZoom = 0.5;
    static constexpr qreal kMaxZoom = 3.0;

    bool darkMail() const { return m_dark; }
    void setDarkMail(bool on);

    int blockedImages() const { return m_blocked; }
    bool imagesLoaded() const { return m_showImages; }
    int trackersBlocked() const { return m_trackers; } // tracking pixels dropped from the last render
    void loadImages();
    // Ask mode: allow-list the sender (Settings > Privacy) and load images.
    void alwaysLoadForSender();
    // Re-read Settings > Privacy > Remote images for the open message.
    void reloadImagePolicy();
    // A message whose layout would stall the window (HtmlFit::kLayoutBudget)
    // is shown with its tables simplified, under a bar that offers the full
    // layout for those willing to wait for it.
    // View > View as Plain Text: the message's text part (or the text of
    // its HTML, when it has none), remembered in QSettings.
    bool plainText() const { return m_plain; }
    void setPlainText(bool on);
    // The header and body as one document, for File > Print. The caller owns it.
    QTextDocument *printableDocument() const;
    bool layoutSimplified() const { return m_simplified; }
    void showFullLayout();

signals:
    void zoomChanged(qreal zoom);
    void mailtoRequested(const QUrl &url); // a mailto: link in the body
    void unsubscribeRequested(const QString &messageId); // the Unsubscribe button

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void changeEvent(QEvent *ev) override;

private:
    void render();
    void renderHeader();
    void renderCalendar();
    void renderUnsubscribe();
    void applyBodyPalette();
    int bodyWidth() const;

    ViewMessage m_msg;
    QLabel *m_warning = nullptr;
    QWidget *m_header = nullptr;
    QGridLayout *m_headerGrid = nullptr;
    QLabel *m_subject = nullptr;
    QLabel *m_attachments = nullptr;
    QFrame *m_calendarCard = nullptr;
    QLabel *m_calendarText = nullptr;
    QFrame *m_unsubscribeBar = nullptr;
    QLabel *m_unsubscribeText = nullptr;
    QPushButton *m_unsubscribe = nullptr;
    QFrame *m_imagesBar = nullptr;
    QLabel *m_imagesText = nullptr;
    QPushButton *m_loadImages = nullptr;
    QPushButton *m_alwaysForSender = nullptr;
    QFrame *m_layoutBar = nullptr;
    bool m_plain = false;       // View as Plain Text
    bool m_simplified = false;  // the last render gave up tables to stay within budget
    bool m_fullLayout = false;  // this message: the user asked for all of it
    SafeHtmlView *m_body = nullptr;
    QTimer *m_relayout = nullptr;
    qreal m_zoom = 1.0;
    qreal m_effectiveZoom = 1.0;
    bool m_dark = false;
    bool m_showImages = false;
    int m_blocked = 0;
    int m_trackers = 0;
    int m_renderedWidth = 0;
    int m_renders = 0;
    bool m_rendering = false;
    bool m_empty = true;
};

} // namespace zmail::ui
