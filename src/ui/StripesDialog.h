#pragma once

#include <QDialog>

class QLabel;
class QSlider;

namespace zmail::ui {

// Settings > Row Stripes: preset buttons over a 0..100 strength slider.
// Moving the slider previews live (strengthChanged); OK keeps the value,
// Cancel/Escape restores the one the dialog opened with (finishedWith).
class StripesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StripesDialog(int strength, QWidget *parent = nullptr);
    int strength() const;
    QSlider *slider() const { return m_slider; }

signals:
    void strengthChanged(int strength);
    void finishedWith(int strength);

private:
    void updateLabel();

    int m_initial;
    QSlider *m_slider = nullptr;
    QLabel *m_value = nullptr;
};

} // namespace zmail::ui
