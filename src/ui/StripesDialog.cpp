#include "StripesDialog.h"

#include "Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace zmail::ui {

StripesDialog::StripesDialog(int initial, QWidget *parent)
    : QDialog(parent)
    , m_initial(initial)
{
    setObjectName(QStringLiteral("stripesDialog"));
    setWindowTitle(tr("Row Stripes"));
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("How strongly alternate rows in the message list are shaded:"), this));

    auto *presets = new QHBoxLayout;
    struct P { const char *obj; QString text; StripeStrength s; };
    for (const P &p : {P{"stripePresetOff", tr("Off"), StripeStrength::Off},
                       P{"stripePresetSubtle", tr("Subtle"), StripeStrength::Subtle},
                       P{"stripePresetNormal", tr("Normal"), StripeStrength::Normal},
                       P{"stripePresetStrong", tr("Strong"), StripeStrength::Strong}}) {
        auto *b = new QPushButton(p.text, this);
        b->setObjectName(QString::fromLatin1(p.obj));
        b->setAutoDefault(false);
        const int v = stripePreset(p.s);
        connect(b, &QPushButton::clicked, this, [this, v]() { m_slider->setValue(v); });
        presets->addWidget(b);
    }
    lay->addLayout(presets);

    auto *row = new QHBoxLayout;
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setObjectName(QStringLiteral("stripeSlider"));
    m_slider->setRange(0, kStripeMax);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(10);
    m_slider->setTickPosition(QSlider::TicksBelow);
    m_slider->setTickInterval(10);
    m_slider->setValue(initial);
    m_slider->setMinimumWidth(260);
    row->addWidget(m_slider, 1);
    m_value = new QLabel(this);
    m_value->setObjectName(QStringLiteral("stripeValue"));
    m_value->setMinimumWidth(110);
    row->addWidget(m_value);
    lay->addLayout(row);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        updateLabel();
        emit strengthChanged(v);
    });
    connect(this, &QDialog::finished, this,
            [this](int result) { emit finishedWith(result == QDialog::Accepted ? strength() : m_initial); });
    updateLabel();
}

int StripesDialog::strength() const
{
    return m_slider->value();
}

void StripesDialog::updateLabel()
{
    const int v = strength();
    if (v == 0) {
        m_value->setText(tr("Off"));
        return;
    }
    // Shown so the contrast choice is explicit: stripe vs. row background.
    const double c = contrastRatio(stripeColor(QApplication::palette(), v), QApplication::palette().color(QPalette::Base));
    m_value->setText(tr("%1 %  (%2:1)").arg(v).arg(c, 0, 'f', 2));
}

} // namespace zmail::ui
