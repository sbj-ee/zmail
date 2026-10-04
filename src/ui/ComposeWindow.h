#pragma once

#include "core/Limits.h"

#include <QList>
#include <QMainWindow>

class QAction;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QTextEdit;
class QWidget;

// Eudora-style compose window: header block (To/From/Subject/Cc/Bcc/Attached),
// formatting toolbar, body, and an encoded-size meter against Gmail's limit.
class ComposeWindow : public QMainWindow
{
    Q_OBJECT

public:
    struct Attachment
    {
        QString name;
        qint64 bytes = 0;
    };

    explicit ComposeWindow(QWidget *parent = nullptr);

    // Fill with a realistic sample reply (fictional people) for previews.
    void loadSampleReply();

    void setAttachments(const QList<Attachment> &list);
    QList<Attachment> attachments() const { return m_attachments; }

    // Encoded MIME size estimate: headers + body + base64(attachments).
    qint64 encodedSize() const;
    zmail::limits::SizeLevel sizeLevel() const;

    QStringList headerFieldOrder() const;

private:
    void buildToolbar();
    void buildHeaderBlock(QWidget *host);
    void buildFormatBar();
    void updateSizeMeter();
    void updateTitle();

    QWidget *m_headerBlock = nullptr;
    QLineEdit *m_to = nullptr;
    QLineEdit *m_from = nullptr;
    QLineEdit *m_subject = nullptr;
    QLineEdit *m_cc = nullptr;
    QLineEdit *m_bcc = nullptr;
    QWidget *m_attached = nullptr;
    QTextEdit *m_body = nullptr;
    QProgressBar *m_sizeMeter = nullptr;
    QLabel *m_modeLabel = nullptr;
    QAction *m_send = nullptr;
    QComboBox *m_signature = nullptr;
    QComboBox *m_format = nullptr;
    QList<Attachment> m_attachments;
};
