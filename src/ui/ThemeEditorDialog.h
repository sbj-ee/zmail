#pragma once

#include "ThemeFile.h"

#include <QDialog>
#include <functional>

class QCheckBox;
class QFontComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolBar;
class QToolButton;
class QTreeWidget;

namespace zmail::ui {

// View > Theme > Theme Editor: create and edit custom themes (*.ztheme.json
// in userThemesDir(); format in docs/THEMES.md "Theme files", shared with
// zterminal). Built-in themes and zterminal's are read-only: Duplicate them
// to edit. The buttons call the public functions below (tests drive those).
class ThemeEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ThemeEditorDialog(const QString &currentId, int stripes, QWidget *parent = nullptr);

    bool select(const QString &id);
    QString currentId() const;
    bool currentEditable() const;
    const sbj::theme::Theme &theme() const { return m_theme; }
    // Edits (as the colour buttons and fields do): live preview, unsaved.
    void setTheme(const sbj::theme::Theme &t);
    bool isDirty() const { return m_dirty; }

    bool duplicateCurrent(const QString &name = {}); // new custom theme, selected
    bool renameCurrent(const QString &name, QString *error = nullptr);
    bool deleteCurrent();
    bool saveCurrent(QString *error = nullptr);
    bool importFile(const QString &path, QString *error = nullptr); // into the themes dir, selected
    bool exportCurrent(const QString &path, QString *error = nullptr);
    void applyCurrent(); // save, then use it (View > Theme)

    QWidget *preview() const { return m_preview; }

signals:
    void applied(const QString &id);
    void themesChanged();

private:
    struct Item {
        QString id, name, path;
        bool editable = false;
    };
    void reloadList(const QString &selectId);
    void showTheme();     // m_theme -> widgets + preview
    void fieldsChanged(); // font / stripe widgets -> m_theme
    void updatePreview();
    void updateButtons();

    int m_stripes; // Settings > Row Stripes, for themes that don't set it
    QList<Item> m_items;
    int m_row = -1;
    sbj::theme::Theme m_theme;
    bool m_dirty = false;
    bool m_loading = false;

    QListWidget *m_list = nullptr;
    QList<std::pair<QToolButton *, int>> m_colourButtons; // role
    QCheckBox *m_setFont = nullptr, *m_setStripes = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr, *m_stripeSpin = nullptr;
    QPushButton *m_rename = nullptr, *m_delete = nullptr, *m_save = nullptr;
    QList<QWidget *> m_editors; // disabled for read-only themes
    QWidget *m_preview = nullptr;
    QToolBar *m_previewBar = nullptr;
    QTreeWidget *m_previewList = nullptr;
    QLabel *m_previewText = nullptr;
};

} // namespace zmail::ui
