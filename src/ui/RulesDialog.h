#pragma once

#include "core/Rules.h"

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace zmail::ui {

// Settings > Filters. The rules in order on the left (the first one that
// matches a message is the one that applies; tick to switch one on or off,
// Up/Down to reorder); the chosen rule's conditions and actions on the
// right. OK keeps the list (rules()), Cancel leaves the old one.
class RulesDialog : public QDialog
{
    Q_OBJECT

public:
    using Folder = QPair<QString, QString>; // Gmail label id, name
    RulesDialog(const QList<zmail::Rule> &rules, const QList<Folder> &folders, QWidget *parent = nullptr);

    QList<zmail::Rule> rules() const { return m_rules; }
    int currentRule() const { return m_current; }
    void setCurrentRule(int index);
    int addRule(const zmail::Rule &rule = {}); // appended and selected; its index
    void removeCurrentRule();
    void moveCurrentRule(int by); // -1 up, +1 down
    void addCondition();
    void removeCondition(int index);

signals:
    void previewSound(const QString &sound); // a rule's sound, to be played now

private:
    void refreshList();
    void loadEditor();
    void rebuildConditions();
    void commit(); // editor widgets -> m_rules[m_current]
    void chooseSoundFile();
    void setSoundChoice(const QString &sound);

    QList<zmail::Rule> m_rules;
    QList<Folder> m_folders;
    int m_current = -1;
    bool m_loading = false;

    QListWidget *m_list = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QWidget *m_editor = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_match = nullptr;
    QWidget *m_conditions = nullptr;
    QVBoxLayout *m_conditionRows = nullptr;
    QComboBox *m_color = nullptr;
    QComboBox *m_flag = nullptr;
    QComboBox *m_sound = nullptr;
    QComboBox *m_moveTo = nullptr;
    QCheckBox *m_markRead = nullptr;
};

} // namespace zmail::ui
