#pragma once

#include <QDialog>

// Help > About zmail. Version comes from the generated version.hpp.
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    // The rich text shown in the dialog (exposed for tests).
    static QString aboutText();
};
