#include "ListDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace zmail::ui {

ListDialog::ListDialog(int fontSize, int rowSpacing, QWidget *parent)
    : QDialog(parent)
    , m_initialFont(fontSize)
    , m_initialSpacing(rowSpacing)
{
    setObjectName(QStringLiteral("listDialog"));
    setWindowTitle(tr("Message List"));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    // One step below the smallest size is "Default": the application font.
    auto *fontRow = new QHBoxLayout;
    m_font = new QSpinBox(this);
    m_font->setObjectName(QStringLiteral("listFontSize"));
    m_font->setRange(kListFontMin - 1, kListFontMax);
    m_font->setSuffix(tr(" pt"));
    m_font->setSpecialValueText(tr("Default (%1 pt)").arg(QFontInfo(QApplication::font()).pointSize()));
    m_font->setValue(fontSize == kListFontDefault ? m_font->minimum() : std::clamp(fontSize, kListFontMin, kListFontMax));
    fontRow->addWidget(m_font, 1);
    auto *fontDefault = new QPushButton(tr("Default"), this);
    fontDefault->setObjectName(QStringLiteral("listFontDefault"));
    fontDefault->setAutoDefault(false);
    connect(fontDefault, &QPushButton::clicked, this, [this]() { m_font->setValue(m_font->minimum()); });
    fontRow->addWidget(fontDefault);
    form->addRow(tr("Text size:"), fontRow);

    m_spacing = new QSpinBox(this);
    m_spacing->setObjectName(QStringLiteral("listRowSpacing"));
    m_spacing->setRange(0, kListSpacingMax);
    m_spacing->setSuffix(tr(" px"));
    m_spacing->setValue(std::clamp(rowSpacing, 0, kListSpacingMax));
    form->addRow(tr("Space between rows:"), m_spacing);
    lay->addLayout(form);

    auto *presets = new QHBoxLayout;
    struct P { const char *obj; QString text; int spacing; };
    for (const P &p : {P{"listPresetCompact", tr("Compact"), 0},
                       P{"listPresetComfortable", tr("Comfortable"), kListSpacingDefault},
                       P{"listPresetRoomy", tr("Roomy"), 12}}) {
        auto *b = new QPushButton(p.text, this);
        b->setObjectName(QString::fromLatin1(p.obj));
        b->setAutoDefault(false);
        const int v = p.spacing;
        connect(b, &QPushButton::clicked, this, [this, v]() { m_spacing->setValue(v); });
        presets->addWidget(b);
    }
    lay->addLayout(presets);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    const auto preview = [this]() { emit changed(this->fontSize(), this->rowSpacing()); };
    connect(m_font, &QSpinBox::valueChanged, this, preview);
    connect(m_spacing, &QSpinBox::valueChanged, this, preview);
    connect(this, &QDialog::finished, this, [this](int result) {
        if (result == QDialog::Accepted) {
            emit finishedWith(this->fontSize(), this->rowSpacing());
        } else {
            emit finishedWith(m_initialFont, m_initialSpacing);
        }
    });
}

int ListDialog::fontSize() const
{
    return m_font->value() == m_font->minimum() ? kListFontDefault : m_font->value();
}

int ListDialog::rowSpacing() const
{
    return m_spacing->value();
}

} // namespace zmail::ui
