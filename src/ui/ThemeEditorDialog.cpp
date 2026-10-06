#include "ThemeEditorDialog.h"

#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace zmail::ui {

using namespace sbj::theme;

namespace {
QIcon swatchIcon(std::uint32_t rgb)
{
    QPixmap pm(28, 16);
    pm.fill(QColor::fromRgb(rgb));
    QPainter p(&pm);
    p.setPen(QColor(0x80, 0x80, 0x80));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

QString stemId(const QString &path)
{
    return QStringLiteral("custom:") + QFileInfo(path).fileName().chopped(qsizetype(qstrlen(kSuffix)));
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(const QString &currentId, int stripes, QWidget *parent)
    : QDialog(parent)
    , m_stripes(stripes)
{
    setWindowTitle(tr("Theme Editor"));
    setObjectName(QStringLiteral("themeEditor"));

    // Left: themes and what to do with them.
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("themeList"));
    m_list->setMinimumWidth(190);
    auto *dup = new QPushButton(tr("D&uplicate"));
    m_rename = new QPushButton(tr("&Rename\u2026"));
    m_delete = new QPushButton(tr("&Delete"));
    auto *imp = new QPushButton(tr("&Import\u2026"));
    auto *exp = new QPushButton(tr("E&xport\u2026"));
    auto *listButtons = new QGridLayout;
    listButtons->addWidget(dup, 0, 0);
    listButtons->addWidget(m_rename, 0, 1);
    listButtons->addWidget(m_delete, 1, 0);
    listButtons->addWidget(imp, 2, 0);
    listButtons->addWidget(exp, 2, 1);
    auto *left = new QVBoxLayout;
    left->addWidget(m_list, 1);
    left->addLayout(listButtons);
    auto *note = new QLabel(tr("Built-in and zterminal themes (italic) are\nread-only: duplicate one to edit it."));
    note->setForegroundRole(QPalette::PlaceholderText);
    left->addWidget(note);

    // Right: the shared palette roles, font, stripes and the preview.
    auto *paletteBox = new QGroupBox(tr("Palette (shared with zterminal)"));
    auto *pg = new QGridLayout(paletteBox);
    for (int r = 0; r < RoleCount; ++r) {
        auto *b = new QToolButton;
        b->setObjectName(QStringLiteral("role:") + QLatin1String(roleKey(r)));
        b->setIconSize(QSize(28, 16));
        connect(b, &QToolButton::clicked, this, [this, r]() {
            const QColor c = QColorDialog::getColor(QColor::fromRgb(m_theme.roles[size_t(r)]), this, tr("Choose Colour"));
            if (c.isValid()) {
                m_theme.roles[size_t(r)] = c.rgb() & 0xFFFFFF;
                m_dirty = true;
                showTheme();
            }
        });
        m_colourButtons.append({b, r});
        pg->addWidget(new QLabel(QLatin1String(roleKey(r))), r / 3, (r % 3) * 2);
        pg->addWidget(b, r / 3, (r % 3) * 2 + 1);
    }

    m_setFont = new QCheckBox(tr("Theme sets the UI font:"));
    m_setFont->setObjectName(QStringLiteral("setFont"));
    m_font = new QFontComboBox;
    m_fontSize = new QSpinBox;
    m_fontSize->setRange(kMinFontSize, kMaxFontSize);
    m_setStripes = new QCheckBox(tr("Theme sets the row stripes:"));
    m_setStripes->setObjectName(QStringLiteral("setStripes"));
    m_stripeSpin = new QSpinBox;
    m_stripeSpin->setRange(0, kStripeMax);
    m_stripeSpin->setToolTip(tr("0 = off, 100 = strongest (Settings > Row Stripes)"));
    auto *settingsBox = new QGroupBox(tr("Font and list"));
    auto *sg = new QGridLayout(settingsBox);
    sg->addWidget(m_setFont, 0, 0);
    sg->addWidget(m_font, 0, 1);
    sg->addWidget(m_fontSize, 0, 2);
    sg->addWidget(m_setStripes, 1, 0);
    sg->addWidget(m_stripeSpin, 1, 2);
    sg->setColumnStretch(1, 1);

    // Preview: toolbar band, message list (header, stripes, selection), text and a link.
    m_preview = new QWidget;
    m_preview->setObjectName(QStringLiteral("themePreview"));
    m_preview->setAutoFillBackground(true);
    m_previewBar = new QToolBar;
    m_previewBar->setAutoFillBackground(true);
    for (const QString &t : {tr("Get Mail"), tr("Compose"), tr("Reply"), tr("Delete")}) {
        m_previewBar->addAction(t);
    }
    m_previewList = new QTreeWidget;
    m_previewList->setHeaderLabels({tr("Who"), tr("Subject"), tr("Date")});
    m_previewList->setRootIsDecorated(false);
    m_previewList->setFocusPolicy(Qt::NoFocus);
    m_previewList->setSelectionMode(QAbstractItemView::SingleSelection);
    const struct { const char *who, *subject, *date; } rows[] = {
        {"Priya Raman", "Q3 planning notes", "9:41 AM"},
        {"GitHub", "[zmail] CI passed", "8:15 AM"},
        {"Marcus Lee", "Lunch Friday?", "Yesterday"},
        {"Ana Souza", "Photos from the trip", "Mon"},
        {"Newsletter", "This week in Qt", "Sun"},
    };
    for (const auto &r : rows) {
        new QTreeWidgetItem(m_previewList, {QString::fromUtf8(r.who), QString::fromUtf8(r.subject), QString::fromUtf8(r.date)});
    }
    m_previewList->setCurrentItem(m_previewList->topLevelItem(1));
    m_previewList->header()->resizeSection(0, 130);
    m_previewList->header()->resizeSection(1, 220);
    m_previewText = new QLabel;
    m_previewText->setTextFormat(Qt::RichText);
    m_previewText->setWordWrap(true);
    m_previewText->setMargin(6);
    auto *pv = new QVBoxLayout(m_preview);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(0);
    pv->addWidget(m_previewBar);
    pv->addWidget(m_previewList, 1);
    pv->addWidget(m_previewText);

    auto *right = new QVBoxLayout;
    right->addWidget(paletteBox);
    right->addWidget(settingsBox);
    right->addWidget(new QLabel(tr("Preview:")));
    right->addWidget(m_preview, 1);

    auto *body = new QHBoxLayout;
    body->addLayout(left);
    body->addLayout(right, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_save = buttons->addButton(tr("&Save"), QDialogButtonBox::ActionRole);
    QPushButton *apply = buttons->addButton(tr("&Apply"), QDialogButtonBox::ApplyRole);
    apply->setToolTip(tr("Save, then use this theme (View > Theme)"));
    auto *outer = new QVBoxLayout(this);
    outer->addLayout(body, 1);
    outer->addWidget(buttons);

    m_editors = {paletteBox, m_setFont, m_setStripes};

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading || row == m_row) {
            return;
        }
        if (m_dirty && QMessageBox::question(this, windowTitle(), tr("Discard unsaved changes to \u201c%1\u201d?")
                                                                      .arg(m_theme.name)) != QMessageBox::Yes) {
            const QSignalBlocker block(m_list);
            m_list->setCurrentRow(m_row);
            return;
        }
        m_row = row;
        m_dirty = false;
        m_theme = row >= 0 ? themeFileFor(m_items.at(row).id).value_or(Theme{}) : Theme{};
        showTheme();
    });
    for (QCheckBox *c : {m_setFont, m_setStripes}) {
        connect(c, &QCheckBox::toggled, this, &ThemeEditorDialog::fieldsChanged);
    }
    connect(m_font, &QFontComboBox::currentFontChanged, this, &ThemeEditorDialog::fieldsChanged);
    for (QSpinBox *s : {m_fontSize, m_stripeSpin}) {
        connect(s, &QSpinBox::valueChanged, this, &ThemeEditorDialog::fieldsChanged);
    }
    connect(dup, &QPushButton::clicked, this, [this]() { duplicateCurrent(); });
    connect(m_rename, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Rename Theme"), tr("Name:"), QLineEdit::Normal, m_theme.name, &ok);
        QString err;
        if (ok && !renameCurrent(n, &err)) {
            QMessageBox::warning(this, tr("Rename Theme"), err);
        }
    });
    connect(m_delete, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, tr("Delete Theme"), tr("Delete the theme \u201c%1\u201d?").arg(m_theme.name))
            == QMessageBox::Yes) {
            deleteCurrent();
        }
    });
    connect(imp, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Theme"), QString(),
                                                          tr("Themes (*.ztheme.json *.json)"));
        QString err;
        if (!path.isEmpty() && !importFile(path, &err)) {
            QMessageBox::warning(this, tr("Import Theme"), err);
        }
    });
    connect(exp, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Theme"),
                                                          fileStem(m_theme.name) + QLatin1String(kSuffix),
                                                          tr("Themes (*.ztheme.json)"));
        QString err;
        if (!path.isEmpty() && !exportCurrent(path, &err)) {
            QMessageBox::warning(this, tr("Export Theme"), err);
        }
    });
    connect(m_save, &QPushButton::clicked, this, [this]() {
        QString err;
        if (!saveCurrent(&err)) {
            QMessageBox::warning(this, tr("Save Theme"), err);
        }
    });
    connect(apply, &QPushButton::clicked, this, &ThemeEditorDialog::applyCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reloadList(currentId);
    resize(860, 680);
}

void ThemeEditorDialog::reloadList(const QString &selectId)
{
    m_loading = true;
    m_items.clear();
    for (ThemeMode m : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::Boilermakers, ThemeMode::Badgers, ThemeMode::Packers}) {
        const QString id = themeId(m);
        m_items.append({id, themeFileFor(id)->name, QString(), false});
    }
    for (const CustomTheme &c : customThemes()) {
        m_items.append({c.id, c.name, c.path, c.editable});
    }
    m_list->clear();
    int sel = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items.at(i);
        auto *item = new QListWidgetItem(it.name, m_list);
        if (!it.editable) { // read-only: italic
            QFont f = item->font();
            f.setItalic(true);
            item->setFont(f);
            item->setToolTip(tr("Read-only: duplicate it to edit"));
        }
        item->setData(Qt::UserRole, it.id);
        if (const std::optional<Theme> t = themeFileFor(it.id)) {
            item->setIcon(swatchIcon(t->roles[Selection]));
        }
        if (it.id == selectId) {
            sel = i;
        }
    }
    m_loading = false;
    m_row = -1;
    m_dirty = false;
    m_list->setCurrentRow(sel);
}

bool ThemeEditorDialog::select(const QString &id)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id) {
            m_dirty = false;
            m_list->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

QString ThemeEditorDialog::currentId() const
{
    return m_row >= 0 ? m_items.at(m_row).id : QString();
}

bool ThemeEditorDialog::currentEditable() const
{
    return m_row >= 0 && m_items.at(m_row).editable;
}

void ThemeEditorDialog::setTheme(const Theme &t)
{
    m_theme = t;
    m_dirty = true;
    showTheme();
}

void ThemeEditorDialog::showTheme()
{
    m_loading = true;
    for (auto &[button, role] : m_colourButtons) {
        button->setIcon(swatchIcon(m_theme.roles[size_t(role)]));
        button->setToolTip(hex(m_theme.roles[size_t(role)]));
    }
    const QFont app = QApplication::font();
    m_setFont->setChecked(!m_theme.fonts.ui.isEmpty() || m_theme.fonts.uiSize > 0);
    m_font->setCurrentFont(QFont(m_theme.fonts.ui.isEmpty() ? app.family() : m_theme.fonts.ui));
    m_fontSize->setValue(m_theme.fonts.uiSize > 0 ? m_theme.fonts.uiSize : std::max(app.pointSize(), kMinFontSize));
    m_setStripes->setChecked(m_theme.ui.rowStripes.has_value());
    m_stripeSpin->setValue(m_theme.ui.rowStripes.value_or(m_stripes));
    m_loading = false;
    updatePreview();
    updateButtons();
}

void ThemeEditorDialog::fieldsChanged()
{
    if (m_loading) {
        return;
    }
    const bool font = m_setFont->isChecked();
    m_theme.fonts.ui = font ? m_font->currentFont().family() : QString();
    m_theme.fonts.uiSize = font ? m_fontSize->value() : 0;
    m_theme.ui.rowStripes = m_setStripes->isChecked() ? std::optional<int>(m_stripeSpin->value()) : std::nullopt;
    m_dirty = true;
    updatePreview();
    updateButtons();
}

void ThemeEditorDialog::updatePreview()
{
    const QPalette p = rolesPalette(m_theme.roles);
    m_preview->setPalette(p);
    m_previewBar->setPalette(rolesToolBarPalette(m_theme.roles));
    QPalette list = p;
    list.setColor(QPalette::AlternateBase, stripeColor(p, m_theme.ui.rowStripes.value_or(m_stripes)));
    m_previewList->setPalette(list);
    m_previewList->setAlternatingRowColors(m_theme.ui.rowStripes.value_or(m_stripes) > 0);
    m_previewList->header()->setPalette(rolesHeaderPalette(m_theme.roles));
    QFont f = QApplication::font();
    if (!m_theme.fonts.ui.isEmpty()) {
        f.setFamilies({m_theme.fonts.ui});
    }
    if (m_theme.fonts.uiSize > 0) {
        f.setPointSize(m_theme.fonts.uiSize);
    }
    m_preview->setFont(f);
    m_previewText->setText(tr("Hi Stephen, the notes are in <a href=\"#\" style=\"color:%1\">the planning doc</a>."
                              "<br><span style=\"color:%2\">Sent from zmail \u2022 accent </span>"
                              "<span style=\"color:%3\">\u25A0</span>")
                               .arg(hex(m_theme.roles[Link]), hex(m_theme.roles[Muted]), hex(m_theme.roles[Accent])));
}

void ThemeEditorDialog::updateButtons()
{
    const bool editable = currentEditable();
    for (QWidget *w : m_editors) {
        w->setEnabled(editable);
    }
    m_font->setEnabled(editable && m_setFont->isChecked());
    m_fontSize->setEnabled(editable && m_setFont->isChecked());
    m_stripeSpin->setEnabled(editable && m_setStripes->isChecked());
    m_rename->setEnabled(editable);
    m_delete->setEnabled(editable);
    m_save->setEnabled(editable && m_dirty);
}

bool ThemeEditorDialog::duplicateCurrent(const QString &name)
{
    if (m_row < 0) {
        return false;
    }
    Theme t = m_theme;
    t.basedOn = currentId();
    t.name = name.trimmed().isEmpty() ? (m_theme.name + tr(" copy")).left(kMaxNameLength) : name.trimmed();
    const QString path = uniquePath(userThemesDir(), t.name);
    if (!save(t, path)) {
        return false;
    }
    reloadList(stemId(path));
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::saveCurrent(QString *error)
{
    if (!currentEditable()) {
        if (error) {
            *error = tr("Built-in and zterminal themes are read-only; duplicate it first.");
        }
        return false;
    }
    if (!save(m_theme, m_items.at(m_row).path, error)) {
        return false;
    }
    const QString id = currentId();
    reloadList(id);
    emit themesChanged();
    if (id == currentThemeId()) { // editing the theme in use: show the change
        emit applied(id);
    }
    return true;
}

bool ThemeEditorDialog::renameCurrent(const QString &name, QString *error)
{
    const QString n = name.trimmed();
    if (!currentEditable() || n.isEmpty() || n.size() > kMaxNameLength) {
        if (error) {
            *error = tr("Enter a name of 1 to %1 characters.").arg(kMaxNameLength);
        }
        return false;
    }
    const QString oldPath = m_items.at(m_row).path, oldId = currentId();
    m_theme.name = n;
    QString path = oldPath;
    if (fileStem(n) != QFileInfo(oldPath).fileName().chopped(qsizetype(qstrlen(kSuffix)))) {
        path = uniquePath(userThemesDir(), n);
    }
    if (!save(m_theme, path, error)) {
        return false;
    }
    if (path != oldPath) {
        QFile::remove(oldPath);
    }
    const QString id = stemId(path);
    reloadList(id);
    if (currentThemeId() == oldId) { // keep using it under its new id
        emit applied(id);
    }
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::deleteCurrent()
{
    const QString id = currentId();
    if (!currentEditable() || !QFile::remove(m_items.at(m_row).path)) {
        return false;
    }
    const QString back = themeFileFor(m_theme.basedOn) ? m_theme.basedOn : themeId(ThemeMode::Light);
    reloadList(back); // back to the theme it came from (if it's still there)
    if (currentThemeId() == id) {
        emit applied(back);
    }
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::importFile(const QString &path, QString *error)
{
    const std::optional<Theme> t = load(path, error);
    if (!t) {
        return false;
    }
    const QString dest = uniquePath(userThemesDir(), t->name);
    if (!save(*t, dest, error)) {
        return false;
    }
    reloadList(stemId(dest));
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::exportCurrent(const QString &path, QString *error)
{
    return save(m_theme, path, error);
}

void ThemeEditorDialog::applyCurrent()
{
    if (m_dirty && currentEditable() && !saveCurrent()) {
        return;
    }
    emit applied(currentId());
}

} // namespace zmail::ui
