#include "RulesDialog.h"

#include "Flags.h"
#include "Icons.h"
#include "NewMailSound.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace zmail::ui {

namespace {
struct Choice { const char *id; QString text; };

QList<Choice> fieldChoices()
{
    return {{"from", RulesDialog::tr("From")},
            {"to", RulesDialog::tr("To")},
            {"subject", RulesDialog::tr("Subject")},
            {"any", RulesDialog::tr("From, To or Subject")}};
}

QList<Choice> opChoices()
{
    return {{"contains", RulesDialog::tr("contains")},
            {"notContains", RulesDialog::tr("does not contain")},
            {"is", RulesDialog::tr("is")},
            {"startsWith", RulesDialog::tr("starts with")},
            {"endsWith", RulesDialog::tr("ends with")},
            {"regex", RulesDialog::tr("matches regular expression")}};
}

void select(QComboBox *box, const QString &id)
{
    const int i = box->findData(id);
    box->setCurrentIndex(i < 0 ? 0 : i);
}
} // namespace

RulesDialog::RulesDialog(const QList<Rule> &rules, const QList<Folder> &folders, QWidget *parent)
    : QDialog(parent)
    , m_rules(rules)
    , m_folders(folders)
{
    setObjectName(QStringLiteral("rulesDialog"));
    setWindowTitle(tr("Filters"));
    resize(900, 560);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Filters run from the top. The first one that matches a message is the one that applies."), this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    lay->addWidget(split, 1);

    // ---- left: the rules, in order ----
    auto *left = new QWidget(split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(left);
    m_list->setObjectName(QStringLiteral("ruleList"));
    ll->addWidget(m_list, 1);
    auto *lb = new QHBoxLayout;
    auto *add = new QPushButton(tr("New"), left);
    add->setObjectName(QStringLiteral("ruleNew"));
    m_delete = new QPushButton(tr("Delete"), left);
    m_delete->setObjectName(QStringLiteral("ruleDelete"));
    m_up = new QPushButton(tr("Up"), left);
    m_up->setObjectName(QStringLiteral("ruleUp"));
    m_down = new QPushButton(tr("Down"), left);
    m_down->setObjectName(QStringLiteral("ruleDown"));
    for (QPushButton *b : {add, m_delete, m_up, m_down}) {
        b->setAutoDefault(false);
        lb->addWidget(b);
    }
    ll->addLayout(lb);

    // ---- right: the chosen rule ----
    m_editor = new QWidget(split);
    m_editor->setObjectName(QStringLiteral("ruleEditor"));
    auto *el = new QVBoxLayout(m_editor);
    el->setContentsMargins(6, 0, 0, 0);
    auto *nameRow = new QFormLayout;
    m_name = new QLineEdit(m_editor);
    m_name->setObjectName(QStringLiteral("ruleName"));
    m_name->setPlaceholderText(tr("A name for this filter"));
    nameRow->addRow(tr("Name:"), m_name);
    el->addLayout(nameRow);

    auto *when = new QGroupBox(tr("When a message"), m_editor);
    auto *wl = new QVBoxLayout(when);
    auto *matchRow = new QHBoxLayout;
    matchRow->addWidget(new QLabel(tr("meets"), when));
    m_match = new QComboBox(when);
    m_match->setObjectName(QStringLiteral("ruleMatch"));
    m_match->addItem(tr("all of these"), false);
    m_match->addItem(tr("any of these"), true);
    matchRow->addWidget(m_match);
    matchRow->addStretch(1);
    auto *addCond = new QPushButton(tr("Add Condition"), when);
    addCond->setObjectName(QStringLiteral("ruleAddCondition"));
    addCond->setAutoDefault(false);
    matchRow->addWidget(addCond);
    wl->addLayout(matchRow);
    m_conditions = new QWidget(when);
    m_conditionRows = new QVBoxLayout(m_conditions);
    m_conditionRows->setContentsMargins(0, 0, 0, 0);
    wl->addWidget(m_conditions);
    el->addWidget(when);

    auto *then = new QGroupBox(tr("Then"), m_editor);
    auto *tl = new QFormLayout(then);
    m_color = new QComboBox(then);
    m_color->setObjectName(QStringLiteral("ruleColor"));
    m_color->addItem(tr("No colour"), QString());
    m_flag = new QComboBox(then);
    m_flag->setObjectName(QStringLiteral("ruleFlag"));
    m_flag->addItem(tr("No flag"), QString());
    for (const FlagColor &f : kFlagColors) {
        const QString id = QString::fromLatin1(f.id);
        m_color->addItem(swatch(flagColor(id)), flagName(id), flagColor(id).name());
        m_flag->addItem(icon(QStringLiteral("flag"), flagColor(id)), flagName(id), id);
    }
    tl->addRow(tr("Colour its row:"), m_color);
    tl->addRow(tr("Flag it:"), m_flag);
    m_moveTo = new QComboBox(then);
    m_moveTo->setObjectName(QStringLiteral("ruleMoveTo"));
    m_moveTo->addItem(tr("Leave it where it is"), QString());
    for (const Folder &f : m_folders) {
        m_moveTo->addItem(icon(QStringLiteral("folder")), f.second, f.first);
    }
    tl->addRow(tr("Move it to:"), m_moveTo);
    m_markRead = new QCheckBox(tr("Mark it as read"), then);
    m_markRead->setObjectName(QStringLiteral("ruleMarkRead"));
    tl->addRow(QString(), m_markRead);
    auto *soundRow = new QHBoxLayout;
    m_sound = new QComboBox(then);
    m_sound->setObjectName(QStringLiteral("ruleSound"));
    soundRow->addWidget(m_sound, 1);
    auto *test = new QPushButton(tr("Test"), then);
    test->setObjectName(QStringLiteral("ruleSoundTest"));
    test->setAutoDefault(false);
    soundRow->addWidget(test);
    tl->addRow(tr("When it arrives, play:"), soundRow);
    el->addWidget(then);
    auto *note = new QLabel(tr("The colour shows on every message the filter matches. Flag, move and mark as read happen when "
                               "a message arrives, or when you choose Message › Filter Messages."),
                            m_editor);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    el->addWidget(note);
    el->addStretch(1);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({280, 620});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_loading) {
            m_current = row;
            loadEditor();
        }
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const int row = m_list->row(item);
        if (!m_loading && row >= 0 && row < m_rules.size()) {
            m_rules[row].enabled = item->checkState() == Qt::Checked;
        }
    });
    connect(add, &QPushButton::clicked, this, [this]() { addRule(); m_name->setFocus(); });
    connect(m_delete, &QPushButton::clicked, this, &RulesDialog::removeCurrentRule);
    connect(m_up, &QPushButton::clicked, this, [this]() { moveCurrentRule(-1); });
    connect(m_down, &QPushButton::clicked, this, [this]() { moveCurrentRule(1); });
    connect(addCond, &QPushButton::clicked, this, &RulesDialog::addCondition);
    connect(m_name, &QLineEdit::textChanged, this, [this]() { commit(); });
    for (QComboBox *box : {m_match, m_color, m_flag, m_moveTo}) {
        connect(box, &QComboBox::currentIndexChanged, this, [this]() { commit(); });
    }
    connect(m_markRead, &QCheckBox::toggled, this, [this]() { commit(); });
    connect(m_sound, &QComboBox::activated, this, [this](int index) {
        if (m_sound->itemData(index).toString() == QLatin1String("\x01" "choose")) {
            chooseSoundFile();
        } else {
            commit();
        }
    });
    connect(test, &QPushButton::clicked, this, [this]() {
        if (m_current >= 0) {
            emit previewSound(m_rules.at(m_current).sound);
        }
    });

    refreshList();
    setCurrentRule(m_rules.isEmpty() ? -1 : 0);
}

void RulesDialog::refreshList()
{
    const bool was = std::exchange(m_loading, true);
    m_list->clear();
    for (const Rule &r : std::as_const(m_rules)) {
        auto *item = new QListWidgetItem(r.name.trimmed().isEmpty() ? tr("(unnamed filter)") : r.name, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
        if (!r.color.isEmpty()) {
            item->setIcon(swatch(QColor(r.color)));
        }
    }
    m_list->setCurrentRow(m_current);
    m_loading = was;
    m_delete->setEnabled(m_current >= 0);
    m_up->setEnabled(m_current > 0);
    m_down->setEnabled(m_current >= 0 && m_current < m_rules.size() - 1);
}

void RulesDialog::setCurrentRule(int index)
{
    m_current = index >= 0 && index < m_rules.size() ? index : -1;
    refreshList();
    loadEditor();
}

int RulesDialog::addRule(const Rule &rule)
{
    Rule r = rule;
    if (r.name.isEmpty() && r.conditions.isEmpty()) {
        r.name = tr("New filter");
        r.conditions.append(RuleCondition());
    }
    m_rules.append(r);
    setCurrentRule(int(m_rules.size()) - 1);
    return m_current;
}

void RulesDialog::removeCurrentRule()
{
    if (m_current < 0) {
        return;
    }
    m_rules.removeAt(m_current);
    setCurrentRule(std::min(m_current, int(m_rules.size()) - 1));
}

void RulesDialog::moveCurrentRule(int by)
{
    const int to = m_current + by;
    if (m_current < 0 || to < 0 || to >= m_rules.size()) {
        return;
    }
    m_rules.swapItemsAt(m_current, to);
    setCurrentRule(to);
}

void RulesDialog::addCondition()
{
    if (m_current < 0) {
        return;
    }
    m_rules[m_current].conditions.append(RuleCondition());
    rebuildConditions();
}

void RulesDialog::removeCondition(int index)
{
    if (m_current < 0 || index < 0 || index >= m_rules.at(m_current).conditions.size()) {
        return;
    }
    m_rules[m_current].conditions.removeAt(index);
    rebuildConditions();
}

void RulesDialog::setSoundChoice(const QString &sound)
{
    m_sound->clear();
    m_sound->addItem(tr("The usual new-mail sound"), QString());
    m_sound->addItem(tr("No sound"), QString::fromLatin1(kRuleSoundNone));
    if (!sound.isEmpty() && sound != QLatin1String(kRuleSoundNone)) {
        m_sound->addItem(QFileInfo(sound).fileName(), sound);
        m_sound->setItemData(m_sound->count() - 1, sound, Qt::ToolTipRole);
    }
    m_sound->addItem(tr("Choose a WAV File…"), QStringLiteral("\x01" "choose"));
    select(m_sound, sound);
}

void RulesDialog::chooseSoundFile()
{
    const QString now = m_current >= 0 ? m_rules.at(m_current).sound : QString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a Sound"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation), tr("WAV files (*.wav)"));
    if (path.isEmpty() || !NewMailSound::isUsableSoundFile(path)) {
        if (!path.isEmpty()) {
            QMessageBox::warning(this, tr("Choose a Sound"), tr("%1 isn't a WAV file zmail can play.").arg(path));
        }
        const bool was = std::exchange(m_loading, true);
        setSoundChoice(now); // back to what it was
        m_loading = was;
        return;
    }
    {
        const bool was = std::exchange(m_loading, true);
        setSoundChoice(path);
        m_loading = was;
    }
    commit();
}

void RulesDialog::loadEditor()
{
    const bool was = std::exchange(m_loading, true);
    m_editor->setEnabled(m_current >= 0);
    const Rule r = m_current >= 0 ? m_rules.at(m_current) : Rule();
    m_name->setText(r.name);
    m_match->setCurrentIndex(r.matchAny ? 1 : 0);
    // A colour written by hand in rules.json that isn't one of ours still shows.
    if (!r.color.isEmpty() && m_color->findData(QColor(r.color).name()) < 0) {
        m_color->addItem(swatch(QColor(r.color)), QColor(r.color).name(), QColor(r.color).name());
    }
    select(m_color, r.color.isEmpty() ? QString() : QColor(r.color).name());
    select(m_flag, r.flag);
    if (!r.moveTo.isEmpty() && m_moveTo->findData(r.moveTo) < 0) {
        m_moveTo->addItem(tr("(a folder that no longer exists)"), r.moveTo);
    }
    select(m_moveTo, r.moveTo);
    m_markRead->setChecked(r.markRead);
    setSoundChoice(r.sound);
    m_loading = was;
    rebuildConditions();
}

void RulesDialog::rebuildConditions()
{
    const bool was = std::exchange(m_loading, true);
    while (QLayoutItem *it = m_conditionRows->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QList<RuleCondition> conds = m_current >= 0 ? m_rules.at(m_current).conditions : QList<RuleCondition>();
    if (conds.isEmpty() && m_current >= 0) {
        auto *every = new QLabel(tr("No conditions: this filter matches every message."), m_conditions);
        every->setObjectName(QStringLiteral("ruleNoConditions"));
        every->setForegroundRole(QPalette::PlaceholderText);
        m_conditionRows->addWidget(every);
    }
    for (int i = 0; i < conds.size(); ++i) {
        auto *row = new QWidget(m_conditions);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto *field = new QComboBox(row);
        field->setObjectName(QStringLiteral("condField_%1").arg(i));
        for (const Choice &c : fieldChoices()) {
            field->addItem(c.text, QString::fromLatin1(c.id));
        }
        select(field, conds.at(i).field);
        auto *op = new QComboBox(row);
        op->setObjectName(QStringLiteral("condOp_%1").arg(i));
        for (const Choice &c : opChoices()) {
            op->addItem(c.text, QString::fromLatin1(c.id));
        }
        select(op, conds.at(i).op);
        auto *value = new QLineEdit(conds.at(i).value, row);
        value->setObjectName(QStringLiteral("condValue_%1").arg(i));
        auto *remove = new QPushButton(tr("Remove"), row);
        remove->setObjectName(QStringLiteral("condRemove_%1").arg(i));
        remove->setAutoDefault(false);
        // The combos keep their own width; what is typed gets the rest.
        field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        op->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        value->setMinimumWidth(180);
        rl->addWidget(field);
        rl->addWidget(op);
        rl->addWidget(value, 1);
        rl->addWidget(remove);
        m_conditionRows->addWidget(row);
        const auto changed = [this, i, field, op, value]() {
            if (m_loading || m_current < 0 || i >= m_rules.at(m_current).conditions.size()) {
                return;
            }
            RuleCondition &c = m_rules[m_current].conditions[i];
            c.field = field->currentData().toString();
            c.op = op->currentData().toString();
            c.value = value->text();
        };
        connect(field, &QComboBox::currentIndexChanged, this, changed);
        connect(op, &QComboBox::currentIndexChanged, this, changed);
        connect(value, &QLineEdit::textChanged, this, changed);
        // Queued: the row (and this button) is deleted by the rebuild.
        connect(remove, &QPushButton::clicked, this, [this, i]() { removeCondition(i); }, Qt::QueuedConnection);
    }
    m_loading = was;
}

void RulesDialog::commit()
{
    if (m_loading || m_current < 0) {
        return;
    }
    Rule &r = m_rules[m_current];
    r.name = m_name->text();
    r.matchAny = m_match->currentData().toBool();
    r.color = m_color->currentData().toString();
    r.flag = m_flag->currentData().toString();
    r.moveTo = m_moveTo->currentData().toString();
    r.markRead = m_markRead->isChecked();
    const QString sound = m_sound->currentData().toString();
    if (!sound.startsWith(QLatin1Char('\x01'))) {
        r.sound = sound;
    }
    // The list shows the name and the colour.
    const bool was = std::exchange(m_loading, true);
    if (QListWidgetItem *item = m_list->item(m_current)) {
        item->setText(r.name.trimmed().isEmpty() ? tr("(unnamed filter)") : r.name);
        item->setIcon(r.color.isEmpty() ? QIcon() : swatch(QColor(r.color)));
    }
    m_loading = was;
}

} // namespace zmail::ui
