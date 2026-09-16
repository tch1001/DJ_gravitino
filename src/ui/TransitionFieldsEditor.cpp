// Keep all serialized fields reachable without requiring users to edit YAML.
// The typed document remains authoritative; this widget only stages changes.
#include "TransitionFieldsEditor.h"
#include "../transitions/TransitionFields.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

namespace gvt {
namespace {
QString pointer(const QStringList& path)
{
    QStringList escaped;
    for (QString part : path) escaped << part.replace("~", "~0").replace("/", "~1");
    return QStringLiteral("/") + escaped.join('/');
}

QString summary(const QJsonValue& value)
{
    if (value.isObject()) return QObject::tr("%1 fields").arg(value.toObject().size());
    if (value.isArray()) return QObject::tr("%1 items").arg(value.toArray().size());
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isNull()) return QStringLiteral("null");
    return value.toString();
}

QJsonValue atPath(QJsonValue root, const QStringList& path)
{
    for (const QString& key : path)
        root = root.isArray() ? root.toArray().at(key.toInt()) : root.toObject().value(key);
    return root;
}

bool replace(QJsonValue& node, const QStringList& path, int depth,
             const QJsonValue& value, bool remove)
{
    if (depth >= path.size()) return false;
    const bool leaf = depth + 1 == path.size();
    const QString key = path[depth];
    if (node.isObject()) {
        auto object = node.toObject();
        if (leaf) {
            if (remove) { if (!object.contains(key)) return false; object.remove(key); }
            else object.insert(key, value);
        } else {
            QJsonValue child = object.value(key);
            if (!replace(child, path, depth + 1, value, remove)) return false;
            object.insert(key, child);
        }
        node = object;
    } else if (node.isArray()) {
        auto array = node.toArray();
        bool valid = false;
        const int index = key.toInt(&valid);
        if (!valid || index < 0 || index > array.size()) return false;
        if (leaf) {
            if (index == array.size()) {
                if (remove) return false;
                array.append(value);
            } else if (remove) array.removeAt(index);
            else array.replace(index, value);
        } else {
            if (index == array.size()) return false;
            QJsonValue child = array.at(index);
            if (!replace(child, path, depth + 1, value, remove)) return false;
            array.replace(index, child);
        }
        node = array;
    } else return false;
    return true;
}

bool protectedPath(const QStringList& path)
{
    return path.size() == 1 && (path[0] == "format" || path[0] == "version");
}

QJsonObject editorFields(const GvtFile& file)
{
    auto root = transitionDocumentFields(file);
    const auto textFields = [](QJsonObject object, std::initializer_list<const char*> keys) {
        for (const char* key : keys)
            if (!object.contains(QLatin1String(key))) object.insert(QLatin1String(key), QString());
        return object;
    };
    // The writer omits empty optional text. Keep its editable fields discoverable
    // even when blank, without changing what gets written to disk.
    root.insert("metadata", textFields(root.value("metadata").toObject(),
                {"author", "created_at", "description", "license"}));
    auto endpoints = root.value("endpoints").toObject();
    for (const char* side : {"outgoing", "incoming"}) {
        auto endpoint = textFields(endpoints.value(side).toObject(), {"notes"});
        auto identity = textFields(endpoint.value("identity").toObject(), {"version"});
        identity.insert("identifiers", textFields(identity.value("identifiers").toObject(),
                          {"isrc", "musicbrainz_recording"}));
        endpoint.insert("identity", identity);
        endpoints.insert(side, endpoint);
    }
    root.insert("endpoints", endpoints);
    auto performance = root.value("performance").toObject();
    for (const char* kind : {"cues", "loops"}) {
        auto definitions = performance.value(kind).toArray();
        for (int i = 0; i < definitions.size(); ++i) {
            auto item = textFields(definitions[i].toObject(), {"label", "purpose", "color", "pairing_group"});
            item.insert("preferred_input", textFields(item.value("preferred_input").toObject(), {"key"}));
            definitions[i] = item;
        }
        performance.insert(kind, definitions);
    }
    root.insert("performance", performance);
    return root;
}
} // namespace

TransitionFieldsEditor::TransitionFieldsEditor(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("transitionEditorAllFields"));
    auto* layout = new QVBoxLayout(this);
    auto* introduction = new QLabel(tr("Every saved YAML field, including extensions. Select a field, "
        "edit its value, then Update field. Apply fields validates the complete working copy; "
        "only Save writes the transition file."), this);
    introduction->setWordWrap(true);
    introduction->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(introduction);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("transitionFieldsSearch"));
    search_->setPlaceholderText(tr("Find a field, path or value…"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("transitionFieldsTree"));
    tree_->setHeaderLabels({tr("Field"), tr("Value")});
    tree_->setColumnCount(2);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->setColumnWidth(0, 200);
    tree_->setMinimumHeight(130);
    layout->addWidget(tree_, 1);
    path_ = new QLabel(this);
    path_->setWordWrap(true);
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(path_);
    help_ = new QLabel(this);
    help_->setWordWrap(true);
    help_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(help_);
    type_ = new QComboBox(this);
    type_->setObjectName(QStringLiteral("transitionFieldType"));
    for (const auto& entry : {std::pair{tr("Text"), QJsonValue::String},
            {tr("Number"), QJsonValue::Double}, {tr("On / Off"), QJsonValue::Bool},
            {tr("Empty (null)"), QJsonValue::Null}, {tr("Group (mapping)"), QJsonValue::Object},
            {tr("List"), QJsonValue::Array}})
        type_->addItem(entry.first, entry.second);
    layout->addWidget(type_);
    values_ = new QStackedWidget(this);
    values_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    text_ = new QPlainTextEdit(values_);
    text_->setObjectName(QStringLiteral("transitionFieldText"));
    text_->setFixedHeight(58);
    number_ = new QLineEdit(values_);
    number_->setObjectName(QStringLiteral("transitionFieldNumber"));
    boolean_ = new QCheckBox(tr("Enabled / true"), values_);
    boolean_->setObjectName(QStringLiteral("transitionFieldBoolean"));
    values_->addWidget(text_);
    values_->addWidget(number_);
    values_->addWidget(boolean_);
    values_->addWidget(new QLabel(tr("No scalar value. Add/select a child for groups and lists."), values_));
    layout->addWidget(values_);
    auto* editing = new QHBoxLayout;
    update_ = new QPushButton(tr("Update field"), this);
    update_->setObjectName(QStringLiteral("transitionFieldUpdate"));
    add_ = new QPushButton(tr("Add child…"), this);
    remove_ = new QPushButton(tr("Remove field"), this);
    editing->addWidget(update_);
    editing->addWidget(add_);
    editing->addWidget(remove_);
    layout->addLayout(editing);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("transitionFieldsStatus"));
    status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    auto* revert = new QPushButton(tr("Revert fields"), this);
    apply_ = new QPushButton(tr("Apply fields"), this);
    apply_->setObjectName(QStringLiteral("transitionFieldsApply"));
    actions->addWidget(revert);
    actions->addWidget(apply_);
    layout->addLayout(actions);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { selectField(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { filterTree(); });
    connect(type_, &QComboBox::currentIndexChanged, this, [this] {
        const int type = type_->currentData().toInt();
        values_->setCurrentIndex(type == QJsonValue::String ? 0 :
            type == QJsonValue::Double ? 1 : type == QJsonValue::Bool ? 2 : 3);
    });
    connect(update_, &QPushButton::clicked, this, &TransitionFieldsEditor::updateSelected);
    connect(add_, &QPushButton::clicked, this, &TransitionFieldsEditor::addField);
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (auto* item = tree_->currentItem()) removeField(item->data(0, Qt::UserRole).toStringList());
    });
    connect(apply_, &QPushButton::clicked, this, [this] { applyPending(); });
    connect(revert, &QPushButton::clicked, this, &TransitionFieldsEditor::discardPending);
}

void TransitionFieldsEditor::setDocument(const GvtFile& file, bool reset)
{
    latest_ = editorFields(file);
    if (!reset && hasPendingChanges()) {
        conflict_ = latest_ != base_;
        updateButtons();
        return;
    }
    if (!reset && base_ == latest_) return;
    base_ = pending_ = latest_;
    conflict_ = false;
    rebuild();
}

void TransitionFieldsEditor::discardPending()
{
    base_ = pending_ = latest_;
    conflict_ = false;
    rebuild();
    emit pendingChanged();
}

void TransitionFieldsEditor::rebuild(const QStringList& selected)
{
    QSet<QString> expanded;
    const auto remember = [&](auto&& self, QTreeWidgetItem* item) -> void {
        if (item->isExpanded()) expanded.insert(pointer(item->data(0, Qt::UserRole).toStringList()));
        for (int i = 0; i < item->childCount(); ++i) self(self, item->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) remember(remember, tree_->topLevelItem(i));
    QStringList selection = selected;
    if (selection.isEmpty() && tree_->currentItem())
        selection = tree_->currentItem()->data(0, Qt::UserRole).toStringList();
    QSignalBlocker blocker(tree_);
    tree_->clear();
    QTreeWidgetItem* selectedItem = nullptr;
    const auto add = [&](auto&& self, QTreeWidgetItem* parent, const QString& name,
                         const QJsonValue& value, const QStringList& path) -> void {
        auto* item = new QTreeWidgetItem({name, summary(value)});
        if (parent) parent->addChild(item); else tree_->addTopLevelItem(item);
        item->setData(0, Qt::UserRole, path);
        item->setToolTip(0, pointer(path));
        item->setToolTip(1, summary(value));
        if (path == selection) selectedItem = item;
        if (value.isObject()) {
            const auto object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
                self(self, item, it.key(), it.value(), path + QStringList{it.key()});
        } else if (value.isArray()) {
            const auto array = value.toArray();
            for (int i = 0; i < array.size(); ++i)
                self(self, item, QStringLiteral("[%1]").arg(i), array[i], path + QStringList{QString::number(i)});
        }
        item->setExpanded(path.size() <= 1 || expanded.contains(pointer(path)));
    };
    add(add, nullptr, tr("Document"), pending_, {});
    tree_->setCurrentItem(selectedItem ? selectedItem : tree_->topLevelItem(0));
    selectField();
    filterTree();
    updateButtons();
}

void TransitionFieldsEditor::selectField()
{
    auto* item = tree_->currentItem();
    if (!item) return;
    const auto path = item->data(0, Qt::UserRole).toStringList();
    const auto value = atPath(pending_, path);
    path_->setText(pointer(path));
    type_->setCurrentIndex(type_->findData(value.type()));
    text_->setPlainText(value.isString() ? value.toString() : QString());
    number_->setText(value.isDouble() ? summary(value) : QString());
    boolean_->setChecked(value.toBool());
    const bool editable = !path.isEmpty() && !protectedPath(path);
    type_->setEnabled(editable);
    values_->setEnabled(editable);
    update_->setEnabled(editable);
    remove_->setEnabled(editable);
    add_->setEnabled((value.isArray() || value.isObject()) && path.size() < 30);
    QString explanation;
    const auto location = pointer(path);
    const auto parent = atPath(pending_, path.mid(0, path.size() - 1));
    if (location.contains("crossfader") || value.toString() == "mixer.xfader" ||
        value.toObject().value("control").toString() == "mixer.xfader" ||
        parent.toObject().value("control").toString() == "mixer.xfader")
        explanation = tr("Compatibility data only: crossfader transition values are preserved but ignored during playback.");
    else if (location.startsWith("/performance/initial_state/outgoing/"))
        explanation = tr("Authored snapshot. Outgoing playback position/tempo/playing are derived from Transition details, not these historical fields.");
    else if (location.endsWith("tempo_ratio"))
        explanation = tr("Ratio × the endpoint's native_bpm gives its authored playback BPM. 1 means native speed.");
    else if (location.contains("/loops/"))
        explanation = tr("Song-relative loop coordinates. Loop length = end_track_beat − start_track_beat; moving only one edge changes repetition timing.");
    else if (protectedPath(path))
        explanation = tr("Read-only format identity: incompatible versions require a migration reader.");
    else if (location.startsWith("/requires"))
        explanation = tr("Required capabilities. Unsupported entries prevent playback.");
    else if (location.startsWith("/endpoints") || location == "/id")
        explanation = tr("Endpoint identity/assumption edits may require Save As. No audio tags or song cues are changed.");
    else explanation = tr("Edits remain in this field draft until Apply fields; Undo is available after applying.");
    help_->setText(explanation);
}

void TransitionFieldsEditor::filterTree()
{
    const QString query = search_->text().trimmed();
    const auto filter = [&](auto&& self, QTreeWidgetItem* item, bool parentMatches) -> bool {
        const bool matches = parentMatches || query.isEmpty() ||
            pointer(item->data(0, Qt::UserRole).toStringList()).contains(query, Qt::CaseInsensitive) ||
            item->text(1).contains(query, Qt::CaseInsensitive);
        bool visible = matches;
        for (int i = 0; i < item->childCount(); ++i)
            visible = self(self, item->child(i), matches) || visible;
        item->setHidden(!visible);
        if (!query.isEmpty() && visible && item->childCount()) item->setExpanded(true);
        return visible;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) filter(filter, tree_->topLevelItem(i), false);
}

bool TransitionFieldsEditor::setField(const QStringList& path, const QJsonValue& value)
{
    if (path.isEmpty() || protectedPath(path) || path.size() > 30 || value.isUndefined()) return false;
    if (value.isDouble() && !std::isfinite(value.toDouble())) return false;
    QJsonValue root(pending_);
    if (!replace(root, path, 0, value, false)) return false;
    pending_ = root.toObject();
    rebuild(path);
    emit pendingChanged();
    return true;
}

bool TransitionFieldsEditor::removeField(const QStringList& path)
{
    if (path.isEmpty() || protectedPath(path)) return false;
    QJsonValue root(pending_);
    if (!replace(root, path, 0, {}, true)) return false;
    pending_ = root.toObject();
    rebuild(path.mid(0, path.size() - 1));
    emit pendingChanged();
    return true;
}

void TransitionFieldsEditor::updateSelected()
{
    if (!tree_->currentItem()) return;
    const auto path = tree_->currentItem()->data(0, Qt::UserRole).toStringList();
    const auto current = atPath(pending_, path);
    QJsonValue value;
    switch (type_->currentData().toInt()) {
    case QJsonValue::String: value = text_->toPlainText(); break;
    case QJsonValue::Double: {
        bool ok = false;
        const double number = number_->text().toDouble(&ok);
        if (!ok || !std::isfinite(number)) {
            status_->setText(tr("Enter a finite number; the document has not changed."));
            return;
        }
        value = number;
        break;
    }
    case QJsonValue::Bool: value = boolean_->isChecked(); break;
    case QJsonValue::Object: value = current.isObject() ? current : QJsonObject{}; break;
    case QJsonValue::Array: value = current.isArray() ? current : QJsonArray{}; break;
    default: value = QJsonValue::Null; break;
    }
    setField(path, value);
}

void TransitionFieldsEditor::addField()
{
    if (!tree_->currentItem()) return;
    auto path = tree_->currentItem()->data(0, Qt::UserRole).toStringList();
    const auto parent = atPath(pending_, path);
    if (parent.isArray()) path << QString::number(parent.toArray().size());
    else if (parent.isObject()) {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, tr("Add field"), tr("Field name"),
                                                QLineEdit::Normal, {}, &accepted);
        if (!accepted || name.isEmpty()) return;
        if (parent.toObject().contains(name)) {
            status_->setText(tr("That field already exists. Select it to edit its value."));
            return;
        }
        path << name;
    } else return;
    setField(path, QString());
}

void TransitionFieldsEditor::updateButtons()
{
    apply_->setEnabled(hasPendingChanges() && !conflict_);
    status_->setText(conflict_
        ? tr("The working document changed elsewhere. Revert fields to reload it before applying; your field draft has been kept.")
        : hasPendingChanges() ? tr("Unapplied field changes — Apply fields to update preview.")
                              : tr("All fields match the working document."));
}

bool TransitionFieldsEditor::applyPending()
{
    if (!hasPendingChanges()) return true;
    if (conflict_) { updateButtons(); return false; }
    GvtFile parsed;
    QString error;
    QStringList warnings;
    if (!transitionParse(transitionFieldsYaml(pending_), parsed, &error, &warnings)) {
        status_->setText(tr("Not applied: %1").arg(error));
        return false;
    }
    base_ = pending_ = latest_ = editorFields(parsed);
    emit applied(parsed);
    rebuild();
    if (!warnings.isEmpty()) status_->setText(warnings.join('\n'));
    emit pendingChanged();
    return true;
}
} // namespace gvt
