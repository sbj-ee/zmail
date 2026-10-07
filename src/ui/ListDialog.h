#pragma once

#include <QDialog>

class QSpinBox;

namespace zmail::ui {

// Message-list text size and row spacing (Settings > Message List).
inline constexpr int kListFontMin = 7;       // pt
inline constexpr int kListFontMax = 28;      // pt
inline constexpr int kListFontDefault = 0;   // 0: the application font's size
inline constexpr int kListSpacingMax = 24;   // px added to each row
inline constexpr int kListSpacingDefault = 6; // 3 px above and below, like the mailbox rows

// Settings > Message List: the text size of the message list (Default
// follows the application font) and the space between its rows, with
// Compact / Comfortable / Roomy presets. Changes preview live (changed);
// OK keeps them, Cancel/Escape restores what the dialog opened with
// (finishedWith).
class ListDialog : public QDialog
{
    Q_OBJECT

public:
    ListDialog(int fontSize, int rowSpacing, QWidget *parent = nullptr);
    int fontSize() const; // pt, or kListFontDefault
    int rowSpacing() const;
    QSpinBox *fontSpin() const { return m_font; }
    QSpinBox *spacingSpin() const { return m_spacing; }

signals:
    void changed(int fontSize, int rowSpacing);
    void finishedWith(int fontSize, int rowSpacing);

private:
    int m_initialFont;
    int m_initialSpacing;
    QSpinBox *m_font = nullptr;
    QSpinBox *m_spacing = nullptr;
};

} // namespace zmail::ui
