#pragma once

#include "core/Signatures.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QTextEdit;

// Settings → Signatures…: named signatures with a rich (HTML) and a plain
// version, and which one new messages start with.
class SignaturesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignaturesDialog(zmail::SignatureStore *store, QWidget *parent = nullptr);

    void accept() override;

private:
    void load(int row);
    void storeCurrent();
    void refreshDefaults();

    zmail::SignatureStore *m_store;
    QList<zmail::Signature> m_sigs;
    int m_row = -1;
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QTextEdit *m_rich = nullptr;
    QPlainTextEdit *m_plain = nullptr;
    QComboBox *m_default = nullptr;
};
