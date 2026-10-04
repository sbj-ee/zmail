#include "AboutDialog.hpp"
#include "version.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About zmail"));

    auto *label = new QLabel(aboutText(), this);
    label->setObjectName(QStringLiteral("aboutLabel"));
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(label);
    layout->addWidget(buttons);
    setMinimumWidth(380);
}

QString AboutDialog::aboutText()
{
    return QStringLiteral(
               "<h2>zmail %1</h2>"
               "<p>A local Gmail client for Linux that plays a sound for new mail, "
               "per sender or per rule, the way Eudora did.</p>"
               "<p>Copyright &copy; 2026 Stephen B. Johnson<br>"
               "Released under the MIT License.</p>"
               "<p><a href=\"https://github.com/%2\">github.com/%2</a></p>"
               "<p>Built with Qt %3.</p>")
        .arg(QString::fromLatin1(zmail::kVersionString),
             QString::fromLatin1(zmail::kRepo),
             QString::fromLatin1(qVersion()));
}
