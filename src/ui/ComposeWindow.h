#pragma once

#include "core/Stationery.h"
#include "core/Limits.h"
#include "core/Mailto.h"
#include "core/MimeBuilder.h"
#include "core/ReplyBuilder.h"
#include "core/Zip.h"

#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <functional>

class QAction;
class QComboBox;
class QFontComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QTextEdit;
class QWidget;

namespace zmail {
class MailSession;
class SignatureStore;
class SpellChecker;
class SpellHighlighter;
} // namespace zmail

// Eudora-style compose window: header block (To/From/Subject/Cc/Bcc/Attached),
// formatting toolbar, body, and an encoded-size meter against Gmail's limit.
// With a MailSession it sends (users.messages.send) and saves drafts
// (users.drafts); without one it's a preview (sample data, screenshots).
class ComposeWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class Format { Html = 0, Plain = 1, Markdown = 2 };
    enum class ZipPolicy { Ask, Always, Never };
    enum class ZipAnswer { Zip, Cancel };

    struct Attachment
    {
        QString name;
        qint64 bytes = 0;
        QString path;       // file on disk (read at send time if data is empty)
        QByteArray data;    // in-memory content (forwards, zips, tests)
        QString mimeType;
    };

    explicit ComposeWindow(QWidget *parent = nullptr);
    ~ComposeWindow() override;

    // Fill with a realistic sample reply (fictional people) for previews.
    void loadSampleReply();

    // Live mode.
    void setSession(zmail::MailSession *session);
    // Stationery: fill this message from a template. Its text goes at the
    // top; its recipients and subject are used where the message has none.
    // Set before a reply's draft arrives, it is applied when the draft is.
    void setStationery(const zmail::Stationery &s);
    // This message as stationery: its recipients, subject, and the text the
    // user wrote (not the signature, not the quoted original).
    zmail::Stationery asStationery(const QString &name) const;
    bool saveAsStationery(const QString &name); // replaces one of the same name
    // Everything needed to put this message back in a compose window later
    // (a queued message opened again): fields, body, format, attachments.
    QByteArray saveState() const;
    bool restoreState(const QByteArray &state);
    // This window is editing queue entry `id`: sending or queueing it again
    // replaces that entry; closing the window leaves it queued as it was.
    void setQueuedId(qint64 id) { m_queuedId = id; }
    qint64 queuedId() const { return m_queuedId; }
    // Reply / Reply All / Forward: headers, threading and the quoted text.
    void setDraft(const zmail::ComposeDraft &draft);
    // A clicked mailto: link: recipients, subject and a plain-text body.
    void setMailto(const zmail::MailtoFields &fields);
    // Forward: fetch the original's attachments and attach them.
    void attachFromMessage(const QString &gmailMessageId);

    void setAttachments(const QList<Attachment> &list);
    void addFiles(const QStringList &paths);
    void addAttachment(const Attachment &a);
    void removeAttachment(int index);
    QList<Attachment> attachments() const { return m_attachments; }

    // Encoded MIME size estimate: headers + body + base64(attachments).
    qint64 encodedSize() const;
    zmail::limits::SizeLevel sizeLevel() const;

    QStringList headerFieldOrder() const;

    // Body format; switching converts the current text.
    Format format() const;
    void setFormat(Format f);
    void setBodyText(const QString &text); // plain/markdown source, or HTML in Html mode

    // Signatures (Settings → Signatures…); "" = none.
    void setSignatureStore(zmail::SignatureStore *store);
    void setSignature(const QString &name);
    QString signatureName() const;

    // The message as it would be sent (attachments read from disk).
    zmail::OutgoingMessage message() const;
    QByteArray buildMime() const;

    // Attachment zipping (PLAN §4.5.1).
    static ZipPolicy zipPolicy();
    static void setZipPolicy(ZipPolicy p);
    void setZipPrompt(std::function<ZipAnswer(const zmail::zip::Probe &, bool *remember)> prompt) { m_zipPrompt = std::move(prompt); }
    bool offerZip();          // runs the policy/prompt; true if zipped
    bool zipAttachments();    // zip now (no prompt)
    void revertZip();
    bool isZipped() const { return !m_unzipped.isEmpty(); }

    QString draftId() const { return m_draftId; }
    QString threadId() const { return m_threadId; }
    QString lastError() const { return m_lastError; }
    bool isSent() const { return m_sent; }
    void setConfirmOnClose(bool on) { m_confirmClose = on; }

    zmail::SpellChecker *spellChecker() const { return m_spell; }

public slots:
    void send();
    void saveDraft();
    // Into the queue, to go at sendAtMs (if zmail is running then, else when
    // it next is), or with File > Send Queued Messages when sendAtMs is 0.
    void queue(qint64 sendAtMs = 0);
    void sendLater(); // asks when, then queue()

signals:
    void sent(const QString &gmailMessageId, const QString &threadId);
    void queued(qint64 sendAtMs); // Send Later: it is in the queue now
    void stationerySaved(const QString &name);
    void sendFailed(const QString &error);
    void draftSaved(const QString &draftId);

protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void buildToolbar();
    void buildHeaderBlock(QWidget *host);
    void buildFormatBar();
    void buildBanner(QWidget *host);
    void updateSizeMeter();
    void updateTitle();
    void updateModeLabel();
    void updateBanner();
    void refreshChips();
    void reloadSignatures();
    void applySignature();
    void removeTaggedBlocks(int property);
    void insertQuote();
    void setBusy(bool busy, const QString &status = {});
    void fail(const QString &message);
    bool validate(QString *why) const;

    QWidget *m_headerBlock = nullptr;
    QLineEdit *m_to = nullptr;
    QLineEdit *m_from = nullptr;
    QLineEdit *m_subject = nullptr;
    QLineEdit *m_cc = nullptr;
    QLineEdit *m_bcc = nullptr;
    QWidget *m_attached = nullptr;
    QStackedWidget *m_stack = nullptr;
    QTextEdit *m_body = nullptr;
    QTextBrowser *m_preview = nullptr;
    QProgressBar *m_sizeMeter = nullptr;
    QLabel *m_modeLabel = nullptr;
    QAction *m_send = nullptr;
    QAction *m_saveDraft = nullptr;
    QAction *m_spellAction = nullptr;
    QAction *m_previewAction = nullptr;
    QComboBox *m_signature = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_priority = nullptr;
    QWidget *m_formatBar = nullptr;
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QPushButton *m_bannerZip = nullptr;
    QPushButton *m_bannerUndo = nullptr;
    QList<Attachment> m_attachments;
    QList<Attachment> m_unzipped; // originals while zipped (for "Undo zip")
    Format m_currentFormat = Format::Html;

    QString recipients(const QLineEdit *field) const;
    void applyStationery();
    zmail::Stationery m_stationery;
    qint64 m_queuedId = 0;
    QPointer<zmail::MailSession> m_session;
    zmail::SignatureStore *m_sigStore = nullptr;
    zmail::SignatureStore *m_ownSigStore = nullptr;
    zmail::SpellChecker *m_spell = nullptr;
    zmail::SpellHighlighter *m_highlighter = nullptr;
    std::function<ZipAnswer(const zmail::zip::Probe &, bool *)> m_zipPrompt;

    QString m_inReplyTo;
    QStringList m_references;
    QString m_threadId;
    QString m_quotedText, m_quotedHtml;
    QString m_draftId;
    QString m_messageId;
    QString m_lastError;
    bool m_busy = false;
    bool m_sent = false;
    bool m_confirmClose = true;
    bool m_closeAfterSave = false;
    QMetaObject::Connection m_progressConn;
};
