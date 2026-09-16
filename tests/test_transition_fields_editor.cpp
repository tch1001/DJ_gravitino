// Protect lossless access to every saved field without editing a user's files.
#include "ui/TransitionFieldsEditor.h"
#include "transitions/TransitionFields.h"
#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

QTreeWidgetItem* findField(QTreeWidget* tree, const QStringList& path)
{
    QTreeWidgetItemIterator it(tree);
    for (; *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toStringList() == path) return *it;
    return nullptr;
}

void every_saved_field_has_a_tree_item(QTreeWidget* tree, const QJsonValue& value,
                                     const QStringList& path = {})
{
    const auto* item = findField(tree, path);
    CHECK(item != nullptr);
    if (item && value.isDouble()) CHECK(item->text(1).toDouble() == value.toDouble());
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            every_saved_field_has_a_tree_item(tree, it.value(), path + QStringList{it.key()});
    } else if (value.isArray()) {
        const auto array = value.toArray();
        for (int i = 0; i < array.size(); ++i)
            every_saved_field_has_a_tree_item(tree, array[i], path + QStringList{QString::number(i)});
    }
}
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    using namespace gvt;
    GvtFile file;
    file.id = QStringLiteral("dd308cff-f9b2-40fd-9c8f-bcfdf0cbf4e9");
    file.name = QStringLiteral("Synthetic field editor fixture");
    file.masterBpm = 132.0;
    file.from.title = QStringLiteral("Outgoing fixture");
    file.from.bpm = 132.0;
    file.from.durationSec = 180.0;
    file.from.durationBeats = 396.0;
    file.to = file.from;
    file.to.title = QStringLiteral("Incoming fixture");
    file.to.bpm = 132.002941;
    file.anchorFromBeat = 100.125;
    file.anchorToBeat = -0.0466821378107263;
    file.initialComplete = true;
    file.initialFrom.captured = file.initialTo.captured = true;
    file.initialCrossfaderPresent = true;
    file.initialCrossfader = 0.125;
    file.events.push_back({4.25, Role::Mixer, ControlId::Crossfader, 0.75, Curve::Step});
    TransitionSavedLoop loop;
    loop.id = QStringLiteral("incoming-loop-1");
    loop.role = Role::ToDeck;
    loop.startTrackBeat = 0.0466821378107263;
    loop.endTrackBeat = 8.0;
    file.transitionLoops.push_back(loop);
    file.extensions.insert(QStringLiteral("example.vendor"), QJsonObject{
        {"key/with~slashes", QJsonArray{QJsonObject{{"0", true}}, QJsonValue::Null, "value"}},
        {"memo", "line 1\nline 2"}});
    file.metadataExtraYaml.insert(QStringLiteral("unknown_credit"), QStringLiteral("Guest"));
    GvtFile normalized;
    QString error;
    CHECK(transitionParse(transitionSerialize(file), normalized, &error));
    if (!error.isEmpty()) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
    file = normalized;
    CHECK(transitionFieldsYaml(transitionDocumentFields(file)) == transitionSerialize(file));

    TransitionFieldsEditor editor;
    editor.resize(510, 720);
    editor.setDocument(file);
    editor.show();
    app.processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("transitionFieldsTree");
    auto* search = editor.findChild<QLineEdit*>("transitionFieldsSearch");
    auto* number = editor.findChild<QLineEdit*>("transitionFieldNumber");
    auto* text = editor.findChild<QPlainTextEdit*>("transitionFieldText");
    auto* update = editor.findChild<QPushButton*>("transitionFieldUpdate");
    auto* apply = editor.findChild<QPushButton*>("transitionFieldsApply");
    auto* status = editor.findChild<QLabel*>("transitionFieldsStatus");
    CHECK(tree && search && number && text && update && apply && status);
    if (!tree || !search || !number || !text || !update || !apply || !status) return 1;
    every_saved_field_has_a_tree_item(tree, editor.fields());
    CHECK(!editor.hasPendingChanges());
    int applied = 0;
    GvtFile accepted;
    QObject::connect(&editor, &TransitionFieldsEditor::applied,
                     [&](const GvtFile& changed) { ++applied; accepted = changed; });

    // Actual GUI selection/update preserves full double precision and arrays.
    const QStringList tempoPath{"performance", "initial_state", "incoming", "tempo_ratio"};
    const QStringList loopPath{"performance", "loops", "0", "end_track_beat"};
    search->setText("tempo_ratio");
    auto* tempoItem = findField(tree, tempoPath);
    CHECK(tempoItem && !tempoItem->isHidden());
    CHECK(findField(tree, {"metadata", "name"})->isHidden());
    tree->setCurrentItem(tempoItem);
    number->setText("0.9999777201933706");
    update->click();
    CHECK(editor.hasPendingChanges());
    CHECK(editor.setField(loopPath, 8.0466821378107263));
    search->clear();
    tree->setCurrentItem(findField(tree, {"metadata", "unknown_credit"}));
    text->setPlainText("New guest\nsecond line");
    update->click();
    CHECK(editor.applyPending());
    CHECK(applied == 1);
    CHECK(accepted.initialTo.tempoRatio == 0.9999777201933706);
    CHECK(accepted.transitionLoops[0].endTrackBeat == 8.0466821378107263);
    CHECK(accepted.transitionLoops[0].startTrackBeat == file.transitionLoops[0].startTrackBeat);
    CHECK(accepted.extensions == file.extensions);
    CHECK(accepted.initialCrossfader == file.initialCrossfader);
    CHECK(accepted.events[0].control == ControlId::Crossfader);
    CHECK(accepted.events[0].value == file.events[0].value);
    CHECK(accepted.metadataExtraYaml.value("unknown_credit") == "New guest\nsecond line");
    CHECK(!editor.hasPendingChanges());

    // Nested extension keys, booleans, nulls and list insertion/removal work.
    const QStringList arrayPath{"extensions", "example.vendor", "key/with~slashes"};
    CHECK(editor.setField(arrayPath + QStringList{"0", "0"}, false));
    CHECK(editor.setField(arrayPath + QStringList{"3"}, QJsonObject{{"new", 1.25}}));
    CHECK(editor.removeField(arrayPath + QStringList{"2"}));
    CHECK(editor.applyPending());
    const auto array = accepted.extensions.value("example.vendor").toObject()
                           .value("key/with~slashes").toArray();
    CHECK(array.size() == 3);
    CHECK(array[0].toObject().value("0") == false);
    CHECK(array[1].isNull());
    CHECK(array[2].toObject().value("new") == 1.25);
    CHECK(!editor.setField({"version"}, 99));
    CHECK(!editor.removeField({"format"}));
    CHECK(!editor.setField({}, QJsonObject{}));
    CHECK(!editor.setField(arrayPath + QStringList{"-1"}, 1));
    CHECK(!editor.setField(arrayPath + QStringList{"invalid"}, 1));

    // Invalid numeric input/whole-document validation never changes the model.
    tree->setCurrentItem(findField(tree, tempoPath));
    number->setText("nan");
    update->click();
    CHECK(!editor.hasPendingChanges());
    CHECK(status->text().contains("finite number"));
    CHECK(editor.setField(tempoPath, -1.0));
    CHECK(!editor.applyPending());
    CHECK(applied == 2);
    CHECK(status->text().startsWith("Not applied:"));
    editor.discardPending();
    CHECK(editor.setField(loopPath, -1.0));
    CHECK(!editor.applyPending());
    CHECK(applied == 2);
    editor.discardPending();

    // A new document edit cannot be clobbered by an old field draft.
    CHECK(editor.setField({"metadata", "author"}, "Draft author"));
    auto externalEdit = accepted;
    externalEdit.name = "Changed elsewhere";
    editor.setDocument(externalEdit);
    CHECK(editor.hasPendingChanges());
    CHECK(!apply->isEnabled());
    CHECK(!editor.applyPending());
    CHECK(applied == 2);
    editor.discardPending();
    CHECK(editor.fields().value("metadata").toObject().value("name") == externalEdit.name);
    CHECK(!editor.hasPendingChanges());
    CHECK(editor.setField({"metadata", "author"}, "New author"));
    CHECK(editor.applyPending());
    CHECK(accepted.name == externalEdit.name);
    CHECK(accepted.author == "New author");
    every_saved_field_has_a_tree_item(tree, editor.fields());
    std::printf("Transition fields editor: %d failures\n", failures);
    return failures ? 1 : 0;
}
