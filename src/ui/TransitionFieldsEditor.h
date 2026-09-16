// A lossless, searchable field inspector; edits are staged until the existing
// safe transition parser accepts the whole document as one undoable change.
#pragma once
#include "../transitions/Transition.h"
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace gvt {
class TransitionFieldsEditor final : public QWidget {
    Q_OBJECT
public:
    explicit TransitionFieldsEditor(QWidget* parent = nullptr);
    void setDocument(const GvtFile& file, bool reset = false);
    bool hasPendingChanges() const { return pending_ != base_; }
    bool applyPending();
    void discardPending();
    // Paths are components, not dot-separated strings: arbitrary extension
    // keys containing dots, slashes or numeric names remain unambiguous.
    bool setField(const QStringList& path, const QJsonValue& value);
    bool removeField(const QStringList& path);
    const QJsonObject& fields() const { return pending_; }

signals:
    void applied(const gvt::GvtFile& file);
    void pendingChanged();

private:
    void rebuild(const QStringList& selected = {});
    void selectField();
    void filterTree();
    void updateSelected();
    void addField();
    void updateButtons();
    QJsonObject base_, pending_, latest_;
    bool conflict_ = false;
    QTreeWidget* tree_;
    QLineEdit* search_;
    QLabel* path_;
    QLabel* help_;
    QLabel* status_;
    QComboBox* type_;
    QStackedWidget* values_;
    QPlainTextEdit* text_;
    QLineEdit* number_;
    QCheckBox* boolean_;
    QPushButton* update_;
    QPushButton* add_;
    QPushButton* remove_;
    QPushButton* apply_;
};
}
