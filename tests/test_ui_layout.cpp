#include "audio/AudioEngine.h"
#include "audio/MasterRecorder.h"
#include "control/ControlBus.h"
#include "library/TrackLibrary.h"
#include "midi/MidiEngine.h"
#include "transitions/TransitionEngine.h"
#include "transitions/TransitionPlayback.h"
#include "transitions/TransitionPlayerExt.h"
#include "ui/LibraryWidget.h"
#include "ui/DeckWidget.h"
#include "ui/Flx4TutorialWidget.h"
#include "ui/MainWindow.h"
#include "ui/MixerWidget.h"
#include "ui/TransitionEditor.h"
#include "ui/TransitionFieldsEditor.h"
#include "ui/TransitionPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

QString headerText(QTableWidget* table, int column)
{
    QTableWidgetItem* item = table ? table->horizontalHeaderItem(column)
                                   : nullptr;
    return item ? item->text() : QString();
}

gvt::TrackDataPtr makeTrack(const QString& title,
                            const QString& fingerprint)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->title = title;
    track->fingerprint = fingerprint;
    track->durationSec = 16.0;
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;
    track->pcm.resize(
        static_cast<std::size_t>(gvt::kSampleRate) * 16U * 2U);
    return track;
}

void sendMouse(QWidget* receiver, QEvent::Type type,
               const QPoint& localPosition, const QPoint& globalPosition,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(localPosition), QPointF(globalPosition),
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(receiver, &event);
}

void checkVirtualKnobOrientation()
{
    gvt::Flx4TutorialWidget surface;
    surface.resize(1000, 620); // Exact design coordinates for pixel checks.
    const auto render = [&] {
        QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        surface.render(&image);
        return image;
    };
    const auto hasWhitePointer = [](const QImage& image, QPoint point) {
        const QColor pixel = image.pixelColor(point);
        return pixel.red() > 180 && pixel.green() > 180 && pixel.blue() > 180;
    };
    for (double value : {0.0, 0.5, 1.0}) {
        gvt::Flx4TutorialLiveState live;
        live.trim.fill(value);
        live.eqHigh.fill(value);
        live.eqMid.fill(value);
        live.eqLow.fill(value);
        live.filter.fill(value);
        live.headphoneMix = live.fxWet = value;
        surface.setLiveState(live);
        const QImage image = render();
        const QPoint pointerOffset = value == 0.0 ? QPoint(-5, 5)
            : value == 1.0 ? QPoint(5, 5) : QPoint(0, -7);
        for (int x : {414, 536})
            for (int y : {164, 214, 264, 314, 365}) {
                CHECK(hasWhitePointer(image, QPoint(x, y) + pointerOffset));
                if (value == 0.5)
                    CHECK(!hasWhitePointer(image, QPoint(x + 7, y)));
            }
        CHECK(hasWhitePointer(image, QPoint(492, 178) + pointerOffset));
        CHECK(hasWhitePointer(image, QPoint(601, 355) + pointerOffset));
    }
    // Both teaching targets and hardware pickup markers use the same sweep.
    for (int deck : {0, 1}) {
        const int x = deck == 0 ? 414 : 536;
        surface.setExpected({deck, gvt::ControlId::EqHigh, 0.5}, {}, {},
                            0.0, true, {},
                            gvt::flx4TutorialMapping(gvt::ControlId::EqHigh, 0.5));
        CHECK(hasWhitePointer(render(), QPoint(x, 201)));
        CHECK(!hasWhitePointer(render(), QPoint(x + 13, 214)));
        surface.clearExpected();
        surface.setTakeovers({{deck, gvt::ControlId::EqHigh, 0.5,
                              0.0, true, true}});
        CHECK(hasWhitePointer(render(), QPoint(x, 201)));
        CHECK(!hasWhitePointer(render(), QPoint(x + 13, 214)));
        surface.setTakeovers({});
    }
}

void checkPrimeRetries(QApplication& app)
{
    const QByteArray originalDirectory = qgetenv("GRAVITINO_TRANSITIONS_DIR");
    QTemporaryDir directory;
    CHECK(directory.isValid());
    qputenv("GRAVITINO_TRANSITIONS_DIR", directory.path().toUtf8());
    gvt::ControlBus bus;
    gvt::AudioEngine engine(&bus);
    gvt::TransitionStore store;
    gvt::TransitionRecorder recorder(&bus, &engine);
    gvt::TransitionPlayer player(&bus, &engine);
    gvt::GvtFile file;
    file.name = QStringLiteral("PRIME retry fixture");
    file.from.title = QStringLiteral("Retry outgoing");
    file.from.fingerprint = QStringLiteral("gvfp1:prime-retry-outgoing");
    file.to.title = QStringLiteral("Retry incoming");
    file.to.fingerprint = QStringLiteral("gvfp1:prime-retry-incoming");
    file.from.bpm = file.to.bpm = file.masterBpm = 120.0;
    file.from.durationSec = file.to.durationSec = 16.0;
    // The entry is just before LOOP OUT: one audio callback can cross it and
    // wrap before the scheduler polls. A retry near this boundary must fire.
    file.anchorFromBeat = 7.995;
    file.anchorToBeat = 2.0;
    file.initialComplete = true;
    file.initialFrom.captured = file.initialTo.captured = true;
    file.initialFrom.playing = true;
    file.initialFrom.positionBeat = file.anchorFromBeat;
    file.initialFrom.loopActive = true;
    file.initialFrom.loopStartBeat = 4.0;
    file.initialFrom.loopEndBeat = 8.0;
    file.initialTo.positionBeat = 2.0;
    file.initialTo.fxWet = 0.75;
    file.events = {{0.0, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
                    gvt::Curve::Step}};
    file.endBeat = 4.0;
    QString error;
    const QString path = store.save(file, &error);
    CHECK(!path.isEmpty());
    store.reload();
    for (int fromDeck : {0, 1}) {
        const int toDeck = 1 - fromDeck;
        engine.deck(fromDeck).loadTrack(makeTrack(file.from.title, file.from.fingerprint));
        engine.deck(toDeck).loadTrack(makeTrack(file.to.title, file.to.fingerprint));
        gvt::TransitionPanel panel(&bus, &engine, &store, &recorder, &player);
        panel.selectTransitionFile(path);
        auto* prime = panel.findChild<QPushButton*>(QStringLiteral("transitionPrime"));
        auto* status = panel.findChild<QLabel*>(QStringLiteral("transitionSetupStatus"));
        CHECK(prime && status);
        if (!prime || !status) continue;
        const auto refresh = [&] {
            for (auto* timer : panel.findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        };
        for (int attempt = 0; attempt < 2; ++attempt) {
            engine.deck(fromDeck).seekSec(3.9);
            engine.deck(fromDeck).tempoRatio.store(0.9);
            engine.deck(fromDeck).play();
            refresh();
            CHECK(prime->isEnabled());
            prime->click();
            CHECK(!player.isActive());
            CHECK(status->text().contains(QStringLiteral("PRIME not armed")));
        }
        engine.deck(fromDeck).tempoRatio.store(1.0);
        refresh();
        CHECK(!player.isActive()); // Correcting the setup does not auto-arm.
        prime->click();
        CHECK(player.isActive());
        CHECK(!engine.deck(toDeck).playing.load());
        std::vector<float> audio(7200 * 2);
        engine.renderOffline(audio.data(), 7200); // Cross entry and LOOP OUT.
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec();
        CHECK(engine.deck(toDeck).playing.load());
        CHECK(status->text().contains(QStringLiteral("in progress")));
        player.abort();
        app.processEvents();
    }
    qputenv("GRAVITINO_TRANSITIONS_DIR", originalDirectory);
}

void checkCustomBankSwitching(QApplication& app)
{
    // Only synthetic in-memory tracks: inspecting/switching either bank must
    // never emit a request to persist song metadata.
    gvt::ControlBus bus;
    gvt::AudioEngine engine(&bus);
    for (int deckIndex : {0, 1}) {
        const auto track = makeTrack(QStringLiteral("Bank fixture"), {});
        track->hotCues[0] = 0.5;
        track->savedLoops[0] = {1.0, 2.0, QStringLiteral("Normal")};
        engine.deck(deckIndex).loadTrack(track);
        gvt::DeckWidget widget(deckIndex, &bus, &engine);
        widget.resize(600, 350);
        widget.show();
        widget.setPerformancePadMode(gvt::PerformancePadMode::Sampler);
        app.processEvents();
        auto* bank = widget.findChild<QComboBox*>(
            QStringLiteral("deck%1CustomBank").arg(deckIndex));
        auto* pad = widget.findChild<QPushButton*>(
            QStringLiteral("deck%1PerformancePad1").arg(deckIndex));
        CHECK(bank && pad);
        if (!bank || !pad) continue;
        CHECK(bank->isVisible() && !bank->isEnabled());
        CHECK(bank->currentText() == QStringLiteral("CUSTOM: NORMAL"));
        CHECK(pad->text() == QStringLiteral("Normal"));
        const QSize normalSize = widget.sizeHint();
        const QRect normalPadGeometry = pad->geometry();
        int metadataChanges = 0;
        QObject::connect(&widget, &gvt::DeckWidget::trackPerformanceMetadataChanged,
                         &widget, [&](int) { ++metadataChanges; });
        QList<gvt::ControlEvent> padEvents;
        QObject::connect(&bus, &gvt::ControlBus::eventDispatched, &widget,
                         [&](const gvt::ControlEvent& event, gvt::Origin) {
            if (event.deck == deckIndex &&
                (event.id == gvt::ControlId::SavedLoop1 ||
                 event.id == gvt::ControlId::TransitionCue1))
                padEvents.append(event);
        });
        const auto trigger = [&](bool pressed) {
            widget.triggerPerformancePad(gvt::PerformancePadMode::Sampler,
                                         0, pressed);
        };
        const auto lastEventIs = [&](gvt::ControlId control, double value) {
            return !padEvents.isEmpty() && padEvents.back().id == control &&
                   padEvents.back().value == value;
        };
        std::array<double, 8> starts, ends;
        starts.fill(-1.0);
        ends.fill(-1.0);
        starts[0] = 4.0;
        ends[0] = 6.0;
        starts[1] = 8.0; // A transition cue, not a loop.
        const auto installBank = [&] {
            widget.setTemporaryTransitionCues(
                starts, ends, {QStringLiteral("Temp"), QStringLiteral("Cue")}, {});
        };

        trigger(true);
        CHECK(lastEventIs(gvt::ControlId::SavedLoop1, 1.0));
        CHECK(engine.deck(deckIndex).previewActive());
        installBank();
        CHECK(lastEventIs(gvt::ControlId::SavedLoop1, 0.0));
        CHECK(!engine.deck(deckIndex).previewActive());
        CHECK(bank->isEnabled() && bank->currentIndex() == 1);
        CHECK(bank->currentText() == QStringLiteral("CUSTOM: TRANSITION"));
        CHECK(pad->text() == QStringLiteral("Temp"));
        CHECK(widget.performancePadLedMask(gvt::PerformancePadMode::Sampler) == 3U);
        CHECK(QMetaObject::invokeMethod(pad, "customContextMenuRequested",
                                       Qt::DirectConnection, Q_ARG(QPoint, QPoint())));
        auto* feedback = widget.findChild<QLabel*>(
            QStringLiteral("deck%1PadFeedback").arg(deckIndex));
        CHECK(feedback && feedback->toolTip().contains(QStringLiteral("protected")));

        trigger(true);
        CHECK(lastEventIs(gvt::ControlId::TransitionCue1, 1.0));
        CHECK(engine.deck(deckIndex).positionSec() == 4.0);
        bank->setCurrentIndex(0);
        CHECK(lastEventIs(gvt::ControlId::TransitionCue1, 0.0));
        CHECK(!engine.deck(deckIndex).previewActive());
        CHECK(widget.performancePadPressedMask() == 0U);
        CHECK(pad->text() == QStringLiteral("Normal"));
        CHECK(widget.performancePadLedMask(gvt::PerformancePadMode::Sampler) == 1U);

        // Re-announcing the selected transition must not undo a user's bank
        // choice. Replaying semantic cues still uses the isolated slots.
        installBank();
        CHECK(bank->currentIndex() == 0);
        engine.deck(deckIndex).handleTransitionCue(0, true);
        CHECK(engine.deck(deckIndex).positionSec() == 4.0);
        engine.deck(deckIndex).handleTransitionCue(0, false);
        engine.deck(deckIndex).handleTransitionCue(1, true);
        CHECK(engine.deck(deckIndex).positionSec() == 8.0);
        engine.deck(deckIndex).handleTransitionCue(1, false);

        bool normalMenuAvailable = false;
        QTimer::singleShot(0, &widget, [&] {
            for (auto* menu : widget.findChildren<QMenu*>()) {
                if (!menu->isVisible()) continue;
                for (const auto* action : menu->actions()) {
                    if (action->text() == QStringLiteral("Clear captured loop"))
                        normalMenuAvailable = action->isEnabled();
                }
                menu->close(); // Inspect only; never clear even this fixture.
            }
        });
        CHECK(QMetaObject::invokeMethod(pad, "customContextMenuRequested",
                                       Qt::DirectConnection, Q_ARG(QPoint, QPoint())));
        CHECK(normalMenuAvailable);
        trigger(true);
        CHECK(lastEventIs(gvt::ControlId::SavedLoop1, 1.0));
        CHECK(engine.deck(deckIndex).positionSec() == 1.0);
        bank->setCurrentIndex(1);
        CHECK(lastEventIs(gvt::ControlId::SavedLoop1, 0.0));
        CHECK(!engine.deck(deckIndex).previewActive());
        const auto countAfterSwitch = padEvents.size();
        trigger(false); // A late release cannot leak into the new bank.
        CHECK(padEvents.size() == countAfterSwitch);
        trigger(true);
        const auto countWhileHeld = padEvents.size();
        installBank();
        CHECK(padEvents.size() == countWhileHeld);
        CHECK(engine.deck(deckIndex).previewActive());
        widget.clearTemporaryTransitionCues();
        CHECK(lastEventIs(gvt::ControlId::TransitionCue1, 0.0));
        CHECK(!engine.deck(deckIndex).previewActive());
        CHECK(bank->currentIndex() == 0 && !bank->isEnabled());
        CHECK(pad->text() == QStringLiteral("Normal"));
        CHECK(track->hotCues[0] == 0.5);
        CHECK(track->savedLoops[0].startSec == 1.0);
        CHECK(track->savedLoops[0].endSec == 2.0);
        CHECK(track->savedLoops[0].label == QStringLiteral("Normal"));
        CHECK(metadataChanges == 0);
        app.processEvents();
        CHECK(widget.sizeHint() == normalSize);
        CHECK(pad->geometry() == normalPadGeometry);
        widget.setPerformancePadMode(gvt::PerformancePadMode::HotCue);
    }
}
}

void editor_event_types_and_auto_apply_keep_the_selected_action(QApplication& app)
{
    using namespace gvt;
    ControlBus bus;
    AudioEngine engine(&bus);
    TrackLibrary library;
    TransitionStore store;
    TransitionRecorder recorder(&bus, &engine);
    TransitionPlayer player(&bus, &engine);
    GvtFile original;
    original.name = "Editor auto-apply synthetic fixture";
    original.from.title = "Outgoing fixture";
    original.to.title = "Incoming fixture";
    original.from.bpm = original.to.bpm = original.masterBpm = 120;
    original.from.durationSec = original.to.durationSec = 16;
    original.endBeat = 16;
    original.requirements = {"timeline.v1", "temporary-cues.v1", "temporary-loops.v1"};
    TransitionHotCue cue;
    cue.id = "launch"; cue.role = Role::FromDeck; cue.trackBeat = 2;
    original.transitionCues.push_back(cue);
    TransitionSavedLoop loop;
    loop.id = "intro"; loop.role = Role::FromDeck;
    loop.startTrackBeat = 4; loop.endTrackBeat = 8;
    original.transitionLoops.push_back(loop);
    GvtEvent cueEvent {1, Role::FromDeck, ControlId::TransitionCue1, 1, Curve::Step};
    cueEvent.cueId = cue.id;
    GvtEvent loopEvent = cueEvent;
    loopEvent.beat = 2; loopEvent.cueId.clear(); loopEvent.loopId = loop.id;
    original.events = {cueEvent, loopEvent,
        {3, Role::FromDeck, ControlId::EqLow, 0.5, Curve::Linear},
        {4, Role::ToDeck, ControlId::Play, 1, Curve::Step}};
    QString error;
    const QString path = store.save(original, &error);
    CHECK(!path.isEmpty());
    CHECK(loadTransitionFile(path, original, &error));
    const auto readSource = [&] {
        QFile source(path);
        CHECK(source.open(QIODevice::ReadOnly));
        return source.readAll();
    };
    const QByteArray sourceBefore = readSource();
    TransitionEditorWindow editor(&engine, &library, &store, &recorder, &player);
    editor.openTransition(original);
    app.processEvents();
    auto* document = editor.findChild<TransitionEditorDocument*>();
    auto* table = editor.findChild<QTableWidget*>("transitionEditorEvents");
    auto* control = editor.findChild<QComboBox*>("transitionEditorEventControl");
    auto* role = editor.findChild<QComboBox*>("transitionEditorEventRole");
    auto* curve = editor.findChild<QComboBox*>("transitionEditorEventCurve");
    auto* gesture = editor.findChild<QComboBox*>("transitionEditorEventGesture");
    auto* padMode = editor.findChild<QComboBox*>("transitionEditorEventPadMode");
    auto* beat = editor.findChild<QDoubleSpinBox*>("transitionEditorEventBeat");
    auto* value = editor.findChild<QDoubleSpinBox*>("transitionEditorEventValue");
    auto* reference = editor.findChild<QLineEdit*>("transitionEditorEventReference");
    auto* apply = editor.findChild<QPushButton*>("transitionEditorApplyEvent");
    auto* automatic = editor.findChild<QCheckBox*>("transitionEditorAutoApplyEvent");
    CHECK(document && table && control && role && curve && gesture && padMode &&
          beat && value && reference && apply && automatic);
    if (!document || !table || !control || !role || !curve || !gesture || !padMode ||
        !beat || !value || !reference || !apply || !automatic) return;
    CHECK(automatic->isChecked());
    auto* undo = document->undoStack();
    for (int row : {0, 2, 1, 3, 0}) table->setCurrentCell(row, 0);
    CHECK(!document->isDirty() && undo->count() == 0);

    // Reproduce the stuck-type bug for both semantic cue and saved-loop IDs.
    automatic->setChecked(false);
    for (int row : {0, 1}) {
        document->reset(original);
        table->setCurrentCell(row, 0);
        CHECK(!reference->text().isEmpty());
        control->setCurrentIndex(control->findData(static_cast<int>(ControlId::StemMelody)));
        CHECK(document->file().events[row].control == ControlId::TransitionCue1);
        CHECK(reference->text().isEmpty() && !reference->isEnabled());
        apply->click();
        const auto& event = document->file().events[row];
        CHECK(event.control == ControlId::StemMelody);
        CHECK(event.cueId.isEmpty() && event.loopId.isEmpty());
        CHECK(document->file().transitionCues.size() == 1);
        CHECK(document->file().transitionLoops.size() == 1);
        CHECK(undo->count() == 1);
        undo->undo();
        CHECK(document->file().events[row].control == ControlId::TransitionCue1);
        CHECK(reference->isEnabled());
        CHECK(!document->isDirty());
        undo->redo();
        CHECK(document->file().events[row].control == ControlId::StemMelody);
    }

    document->reset(original);
    table->setCurrentCell(2, 0);
    automatic->setChecked(true);
    CHECK(undo->count() == 0); // enabling with unchanged fields isn't an edit
    for (ControlId stem : {ControlId::StemVocals, ControlId::StemMelody,
                           ControlId::StemBass, ControlId::StemDrums}) {
        document->reset(original);
        table->setCurrentCell(2, 0);
        control->setCurrentIndex(control->findData(static_cast<int>(stem)));
        CHECK(document->file().events[2].control == stem);
        CHECK(document->file().events[2].curve == Curve::Linear);
        CHECK(undo->count() == 1);
        apply->click();
        CHECK(undo->count() == 1); // no duplicate undo after auto-apply
        undo->undo();
        CHECK(document->file().events[2].control == ControlId::EqLow);
        CHECK(!document->isDirty());
        undo->redo();
        CHECK(document->file().events[2].control == stem);
    }
    value->setValue(0.25);
    CHECK(document->file().events[2].value == 0.25);
    CHECK(!value->keyboardTracking() && !beat->keyboardTracking());
    value->setFocus();
    value->findChild<QLineEdit*>()->setText("0.125");
    CHECK(document->file().events[2].value == 0.25); // don't apply half-typed numbers
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(value, &enter);
    CHECK(document->file().events[2].value == 0.125);
    beat->setValue(7.25);
    CHECK(table->currentRow() == 3);
    CHECK(document->file().events[3].beat == 7.25);
    CHECK(document->file().events[3].control == ControlId::StemDrums);
    curve->setCurrentIndex(curve->findData(static_cast<int>(Curve::SCurve)));
    CHECK(document->file().events[3].curve == Curve::SCurve);
    role->setCurrentIndex(role->findData(static_cast<int>(Role::ToDeck)));
    CHECK(document->file().events[3].role == Role::ToDeck);
    gesture->setCurrentIndex(gesture->findData(static_cast<int>(ControlId::PerformancePad1)));
    CHECK(document->file().events[3].gestureControl == ControlId::PerformancePad1);
    padMode->setCurrentIndex(padMode->findData(static_cast<int>(PerformancePadMode::PadFx1)));
    CHECK(document->file().events[3].gesturePadMode == static_cast<int>(PerformancePadMode::PadFx1));

    // Incomplete/invalid cue references remain a visible draft, with a useful
    // non-modal error. They must not change the model or open a dialog on typing.
    document->reset(original);
    table->setCurrentCell(2, 0);
    control->setCurrentIndex(control->findData(static_cast<int>(ControlId::TransitionCue1)));
    CHECK(document->file().events[2].control == ControlId::EqLow);
    CHECK(editor.statusBar()->currentMessage().startsWith("Not applied:"));
    reference->setText("missing-cue");
    QMetaObject::invokeMethod(reference, "editingFinished", Qt::DirectConnection);
    CHECK(document->file().events[2].control == ControlId::EqLow);
    reference->setText("launch");
    QMetaObject::invokeMethod(reference, "editingFinished", Qt::DirectConnection);
    CHECK(document->file().events[2].control == ControlId::TransitionCue1);
    CHECK(document->file().events[2].cueId == "launch");
    CHECK(undo->count() == 1);
    CHECK(!editor.statusBar()->currentMessage().startsWith("Not applied:"));

    // Turning auto-apply back on applies a staged edit, while no selection
    // remains a form for Add and must not alter any existing point.
    automatic->setChecked(false);
    value->setValue(0.0);
    CHECK(document->file().events[2].value == 0.5);
    automatic->setChecked(true);
    CHECK(document->file().events[2].value == 0.0);
    CHECK(readSource() == sourceBefore); // Apply isn't Save
    document->reset(original);
    table->setCurrentCell(-1, -1);
    value->setValue(0.75);
    CHECK(!document->isDirty());
    table->setCurrentCell(2, 0);
    const QByteArray screenshot = qgetenv("GRAVITINO_AUTO_APPLY_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        editor.resize(1100, 700);
        app.processEvents();
        CHECK(automatic->width() >= automatic->sizeHint().width());
        CHECK(automatic->geometry().right() < automatic->parentWidget()->width());
        CHECK(editor.grab().save(QString::fromUtf8(screenshot)));
    }
    editor.close();
}

void editor_audio_matches_perform_and_explains_the_two_beat_rulers(QApplication& app)
{
    using namespace gvt;
    ControlBus bus, referenceBus;
    AudioEngine engine(&bus), reference(&referenceBus);
    TrackLibrary library;
    TransitionStore store;
    TransitionRecorder recorder(&bus, &engine);
    TransitionPlayer livePlayer(&bus, &engine), referencePlayer(&referenceBus, &reference);
    auto outgoing = makeTrack("Preview parity outgoing", "gvfp1:preview-parity-out");
    auto incoming = makeTrack("Preview parity incoming", "gvfp1:preview-parity-in");
    for (const auto& track : {outgoing, incoming})
        for (std::size_t i = 0; i < track->pcm.size(); ++i)
            track->pcm[i] = 0.1f * static_cast<float>(std::sin(i *
                (track == outgoing ? 0.025 : 0.031)));
    engine.deck(1).loadTrack(outgoing); // verify reversed physical roles
    engine.deck(0).loadTrack(incoming);
    reference.deck(0).loadTrack(outgoing);
    reference.deck(1).loadTrack(incoming);
    engine.crossfader.store(0.37f);
    engine.deck(1).trim.store(0.65f);
    engine.deck(1).preservePitch.store(true);
    GvtFile file;
    file.name = "Editor audition parity fixture";
    file.from.title = outgoing->title;
    file.from.fingerprint = outgoing->fingerprint;
    file.to.title = incoming->title;
    file.to.fingerprint = incoming->fingerprint;
    file.from.bpm = file.to.bpm = file.masterBpm = 120;
    file.from.durationSec = file.to.durationSec = 16;
    file.anchorFromBeat = 4;
    file.anchorToBeat = 8;
    file.initialComplete = true;
    file.initialFrom.captured = file.initialTo.captured = true;
    file.initialFrom.positionBeat = 1; // old editor wrongly previewed this
    file.initialFrom.playing = false;
    file.initialTo.positionBeat = 0;
    file.initialTo.fxWet = 0.25;
    file.endBeat = 4;
    TransitionHotCue cue;
    cue.id = "launch";
    cue.role = Role::ToDeck;
    cue.trackBeat = 2.125;
    file.transitionCues.push_back(cue);
    file.requirements << "timeline.v1" << "temporary-cues.v1" << "timeline-end.v1";
    GvtEvent press {0, Role::ToDeck, ControlId::TransitionCue1, 1, Curve::Step};
    press.cueId = cue.id;
    GvtEvent release = press;
    release.beat = 0.04;
    release.value = 0;
    file.events = {press,
        {0.02, Role::ToDeck, ControlId::Play, 1, Curve::Step}, release,
        {0.05, Role::ToDeck, ControlId::FxWet, 0.7, Curve::Linear}};
    QString error;
    const QString path = store.save(file, &error);
    CHECK(!path.isEmpty());
    CHECK(loadTransitionFile(path, file, &error));
    TransitionEditorWindow editor(&engine, &library, &store, &recorder, &livePlayer);
    editor.openTransition(file);
    editor.resize(1200, 800);
    app.processEvents();
    auto* play = editor.findChild<QPushButton*>("transitionEditorPlay");
    auto* stop = editor.findChild<QPushButton*>("transitionEditorStop");
    auto* timeline = editor.findChild<TransitionTimelineView*>("transitionEditorTimeline");
    auto* table = editor.findChild<QTableWidget*>("transitionEditorEvents");
    auto* tabs = editor.findChild<QTabWidget*>("transitionEditorInspector");
    auto* meaning = editor.findChild<QLabel*>("transitionEditorEventMeaning");
    auto* source = editor.findChild<QPushButton*>("transitionEditorEditSource");
    auto* snap = editor.findChild<QComboBox*>("transitionEditorSnap");
    auto* initial = editor.findChild<QTableWidget*>("transitionEditorInitialState");
    CHECK(play && stop && timeline && table && tabs && meaning && source && snap && initial);
    if (!play || !stop || !timeline || !table || !tabs || !meaning || !source || !snap || !initial)
        return;
    CHECK(snap->currentData().toDouble() == 0.0);
    CHECK(table->horizontalHeaderItem(0)->text().contains("When"));
    table->setCurrentCell(0, 0);
    app.processEvents();
    CHECK(meaning->text().contains("cue press"));
    CHECK(source->isEnabled());
    source->click();
    CHECK(tabs->tabText(tabs->currentIndex()) == "Cues / Loops");
    auto* definitions = editor.findChild<QTableWidget*>("transitionEditorPerformanceDefinitions");
    CHECK(definitions && definitions->currentColumn() == 3);
    CHECK(initial->item(1, 1)->text().toDouble() == file.anchorFromBeat);
    CHECK(!(initial->item(1, 1)->flags() & Qt::ItemIsEditable));
    CHECK(initial->item(0, 1)->text().toDouble() == 1.0);
    tabs->setCurrentIndex(1);
    const auto screenshot = qgetenv("GRAVITINO_EDITOR_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        app.processEvents();
        CHECK(editor.grab().save(QString::fromUtf8(screenshot)));
    }
    copyTransitionPlaybackContext(engine, 1, referenceBus, reference);
    prepareTransitionSetup(referenceBus, reference, file, 0);
    positionTransitionPerform(reference, file, 0);
    transitionPlayerUseExternalClock(&referencePlayer, true);
    CHECK(referencePlayer.arm(file, 0, false, &error));
    referenceBus.dispatch({0, ControlId::Play, 1}, Origin::Replay);
    timeline->setPlayheadBeat(0);
    play->click(); // real editor reset / ring / exclusive MASTER path
    CHECK(engine.exclusivePreviewActive());
    std::array<float, 3072> expected {}, actual {};
    double beat = 0;
    for (int block = 0; block < 6; ++block) {
        transitionPlayerAdvanceToBeat(&referencePlayer, beat);
        reference.renderOffline(expected.data() + block * 512, 256);
        beat += 256.0 / kSampleRate * reference.deck(0).effectiveBpm() / 60.0;
    }
    engine.renderOffline(actual.data(), 1536);
    double maximumError = 0, energy = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        maximumError = std::max(maximumError, static_cast<double>(std::fabs(actual[i] - expected[i])));
        energy += std::fabs(actual[i]);
    }
    CHECK(maximumError < 1e-6 && energy > 1.0);
    // Waiting for GUI updates cannot advance the visible beat beyond the
    // consumed audio, even though production fills another buffer ahead.
    QEventLoop tick;
    QTimer::singleShot(40, &tick, &QEventLoop::quit);
    tick.exec();
    CHECK(std::fabs(timeline->playheadBeat() - beat) < 1e-9);
    CHECK(engine.deck(1).positionSec() == 0.0);
    CHECK(engine.deck(0).positionSec() == 0.0);
    CHECK(std::fabs(engine.crossfader.load() - 0.37f) < 1e-7);
    stop->click();
    CHECK(!engine.exclusivePreviewActive());
    CHECK(timeline->playheadBeat() == 0.0);
    // Editing WHEN on PLAY shifts the full cue gesture, not its source cue.
    auto* document = editor.findChild<TransitionEditorDocument*>();
    auto* eventBeat = editor.findChild<QDoubleSpinBox*>("transitionEditorEventBeat");
    auto* apply = editor.findChild<QPushButton*>("transitionEditorApplyEvent");
    auto* moveTogether = editor.findChild<QCheckBox*>("transitionEditorMoveLaunchTogether");
    CHECK(document && eventBeat && apply && moveTogether);
    if (document && eventBeat && apply && moveTogether) {
        table->setCurrentCell(1, 0);
        CHECK(moveTogether->isChecked() && moveTogether->isVisible());
        CHECK(source->isEnabled()); // PLAY links to the launch cue too
        eventBeat->setValue(0.22);
        apply->click();
        const auto& changed = document->file();
        CHECK(changed.events[0].control == ControlId::FxWet); // untouched event
        CHECK(std::fabs(changed.events[1].beat - 0.2) < 1e-9);
        CHECK(changed.events[2].control == ControlId::Play);
        CHECK(std::fabs(changed.events[2].beat - 0.22) < 1e-9);
        CHECK(std::fabs(changed.events[3].beat - 0.24) < 1e-9);
        CHECK(table->currentRow() == 2);
        CHECK(changed.transitionCues.front().trackBeat == 2.125);
        document->undoStack()->undo();
        CHECK(document->file().events[0].beat == 0.0);
        CHECK(!document->isDirty());
        table->setCurrentCell(1, 0);
        moveTogether->setChecked(false);
        eventBeat->setValue(0.025);
        apply->click();
        CHECK(document->file().events[0].beat == 0.0);
        CHECK(document->file().events[2].beat == 0.04);
        document->undoStack()->undo();
    }
    editor.close();
}

void editor_drag_guides_follow_the_marker_not_the_pointer(QApplication& app)
{
    using namespace gvt;
    TransitionEditorDocument document;
    GvtFile file;
    file.sourceFormat = TransitionSourceFormat::PortableYaml;
    file.masterBpm = file.from.bpm = file.to.bpm = 120;
    file.initialComplete = true;
    file.initialFrom.captured = file.initialTo.captured = true;
    file.initialFrom.playing = true;
    file.endBeat = 16;
    TransitionHotCue cue;
    cue.id = "drag-cue";
    cue.role = Role::FromDeck;
    cue.trackBeat = 2;
    file.transitionCues.push_back(cue);
    TransitionSavedLoop loop;
    loop.id = "drag-loop";
    loop.role = Role::FromDeck;
    loop.startTrackBeat = 8;
    loop.endTrackBeat = 10;
    file.transitionLoops.push_back(loop);
    file.cues.push_back({6, "Drag label"});
    file.events = {
        {3, Role::ToDeck, ControlId::Play, 1, Curve::Step},
        {5, Role::FromDeck, ControlId::Fader, 0.5, Curve::Step}};
    document.reset(file);
    TransitionTimelineView view(&document);
    view.setTracks(makeTrack("Guide outgoing", "guide-out"),
                   makeTrack("Guide incoming", "guide-in"));
    view.setSnapBeats(1);
    view.resize(1100, 460);
    view.show();
    app.processEvents();
    const auto xAt = [](double beat) { return 104 + static_cast<int>(std::lround(beat * 28)); };
    const auto render = [&] {
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        view.render(&image);
        return image;
    };
    const auto hasGuide = [&](const QImage& image, double beat) {
        return image.pixelColor(xAt(beat), 190) == QColor(98, 224, 244) &&
               image.pixelColor(xAt(beat), view.height() - 5) == QColor(98, 224, 244);
    };
    const auto mouse = [&](QEvent::Type type, QPoint point,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, QPointF(point), QPointF(view.mapToGlobal(point)),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            modifiers);
        QApplication::sendEvent(&view, &event);
    };
    // Cue: press slightly off-center, then snap to a beat while dragging.
    mouse(QEvent::MouseButtonPress, QPoint(xAt(2) + 5, 70));
    CHECK(hasGuide(render(), 2));
    mouse(QEvent::MouseMove, QPoint(xAt(3.3), 70));
    CHECK(hasGuide(render(), 3));
    CHECK(document.file().transitionCues[0].trackBeat == 2); // not committed yet
    const auto screenshot = qgetenv("GRAVITINO_DRAG_SCREENSHOT");
    if (!screenshot.isEmpty()) CHECK(render().save(QString::fromUtf8(screenshot)));
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(3.3), 70));
    CHECK(document.file().transitionCues[0].trackBeat == 3);
    CHECK(!hasGuide(render(), 3));
    CHECK(document.undoStack()->count() == 1);
    document.undoStack()->undo();
    CHECK(document.file().transitionCues[0].trackBeat == 2);

    // Discrete action cards must themselves follow the temporary position.
    mouse(QEvent::MouseButtonPress, QPoint(xAt(3) + 4, 240));
    CHECK(hasGuide(render(), 3));
    mouse(QEvent::MouseMove, QPoint(xAt(4.4), 240));
    const QImage actionPreview = render();
    CHECK(hasGuide(actionPreview, 4));
    CHECK(actionPreview.pixelColor(xAt(4) + 4, 240) == QColor(232, 93, 117));
    CHECK(actionPreview.pixelColor(xAt(3) + 4, 240) != QColor(232, 93, 117));
    CHECK(document.file().events[0].beat == 3);
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(4.4), 240));
    CHECK(document.file().events[0].beat == 4);
    CHECK(!hasGuide(render(), 4));
    document.undoStack()->undo();

    // Automation and Option-drag bypass snapping; the guide follows the dot.
    mouse(QEvent::MouseButtonPress, QPoint(xAt(5), 303));
    mouse(QEvent::MouseMove, QPoint(xAt(7) + 7, 303), Qt::AltModifier);
    CHECK(hasGuide(render(), 7.25));
    CHECK(document.file().events[1].beat == 5);
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(7) + 7, 303));
    CHECK(document.file().events[1].beat == 7.25);
    document.undoStack()->undo();

    // Loop IN cannot cross OUT: the guide follows the clamped marker, not
    // the pointer or the requested snapped beat.
    mouse(QEvent::MouseButtonPress, QPoint(xAt(8), 70));
    mouse(QEvent::MouseMove, QPoint(xAt(12), 70));
    CHECK(hasGuide(render(), 9.75));
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(12), 70));
    CHECK(document.file().transitionLoops[0].startTrackBeat == 9.75);
    document.undoStack()->undo();

    mouse(QEvent::MouseButtonPress, QPoint(xAt(6), 10));
    mouse(QEvent::MouseMove, QPoint(xAt(7.1), 10));
    CHECK(hasGuide(render(), 7));
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(7.1), 10));
    CHECK(!hasGuide(render(), 7));
    document.undoStack()->undo();

    mouse(QEvent::MouseButtonPress, QPoint(xAt(16), 100));
    mouse(QEvent::MouseMove, QPoint(xAt(17.2), 100));
    CHECK(hasGuide(render(), 17));
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(17.2), 100));
    CHECK(!hasGuide(render(), 17));
    document.undoStack()->undo();

    // An unrolled loop contains several copies of a cue: stay on the copy
    // being dragged instead of jumping the guide back to the first pass.
    file.initialFrom.loopActive = true;
    file.initialFrom.loopStartBeat = 0;
    file.initialFrom.loopEndBeat = 4;
    file.transitionLoops.clear();
    document.reset(file);
    mouse(QEvent::MouseButtonPress, QPoint(xAt(10) + 4, 70));
    CHECK(hasGuide(render(), 10));
    mouse(QEvent::MouseMove, QPoint(xAt(11.2), 70));
    CHECK(hasGuide(render(), 11));
    CHECK(!hasGuide(render(), 3));
    mouse(QEvent::MouseButtonRelease, QPoint(xAt(11.2), 70));
    CHECK(document.file().transitionCues[0].trackBeat == 3);
    CHECK(!hasGuide(render(), 11));
    document.undoStack()->undo();
    view.close();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("GravitinoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_ui_layout"));
    QTemporaryDir settingsDirectory;
    CHECK(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());
    qputenv("GRAVITINO_TRANSITIONS_DIR",
            (settingsDirectory.path() + QStringLiteral("/transitions"))
                .toUtf8());

    checkCustomBankSwitching(app);
    checkVirtualKnobOrientation();
    editor_drag_guides_follow_the_marker_not_the_pointer(app);
    checkPrimeRetries(app);
    {
        const QByteArray previousDirectory = qgetenv("GRAVITINO_TRANSITIONS_DIR");
        QTemporaryDir parityDirectory;
        qputenv("GRAVITINO_TRANSITIONS_DIR", parityDirectory.path().toUtf8());
        editor_audio_matches_perform_and_explains_the_two_beat_rulers(app);
        editor_event_types_and_auto_apply_keep_the_selected_action(app);
        qputenv("GRAVITINO_TRANSITIONS_DIR", previousDirectory);
    }

    gvt::ControlBus bus;
    gvt::ControlId lastMousePlatterControl = gvt::ControlId::Count;
    double lastMousePlatterValue = 0.0;
    int mousePlatterTouchEvents = 0;
    QObject::connect(
        &bus, &gvt::ControlBus::eventDispatched,
        [&lastMousePlatterControl, &lastMousePlatterValue,
         &mousePlatterTouchEvents](
            const gvt::ControlEvent& event, gvt::Origin origin) {
            if (origin != gvt::Origin::Ui || event.deck != 0 ||
                (event.id != gvt::ControlId::Jog &&
                 event.id != gvt::ControlId::PlatterScratch &&
                 event.id != gvt::ControlId::PlatterTouch))
                return;
            lastMousePlatterControl = event.id;
            lastMousePlatterValue = event.value;
            if (event.id == gvt::ControlId::PlatterTouch)
                ++mousePlatterTouchEvents;
        });
    gvt::AudioEngine engine(&bus);
    gvt::TrackLibrary library;
    gvt::TransitionStore store;
    gvt::TransitionRecorder transitionRecorder(&bus, &engine);
    gvt::TransitionPlayer transitionPlayer(&bus, &engine);
    gvt::MidiEngine midi(&bus, &engine);

    gvt::GvtFile sample;
    sample.name = QStringLiteral("Progress UI fixture");
    sample.from.title = QStringLiteral("Outgoing fixture");
    sample.from.fingerprint = QStringLiteral("gvfp1:outgoing-fingerprint");
    sample.from.bpm = 120.0;
    sample.from.durationSec = 16.0;
    sample.to.title = QStringLiteral("Incoming fixture");
    sample.to.fingerprint = QStringLiteral("gvfp1:incoming-fingerprint");
    sample.to.bpm = 120.0;
    sample.to.durationSec = 16.0;
    sample.anchorFromBeat = 100.0;
    sample.anchorToBeat = 200.0;
    sample.masterBpm = 120.0;
    sample.initialComplete = true;
    sample.initialFrom.captured = true;
    sample.initialFrom.fader = 0.25;
    sample.initialFrom.eqHigh = 0.8;
    sample.initialFrom.quantizeCaptured = true;
    sample.initialFrom.quantize = false;
    sample.initialTo.captured = true;
    sample.events = {
        {2.0, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
         gvt::Curve::Step},
        {4.0, gvt::Role::Mixer, gvt::ControlId::Crossfader, 1.0,
         gvt::Curve::Step},
    };
    sample.endBeat = 8.0;
    sample.requirements = {QStringLiteral("timeline.v1"),
                           QStringLiteral("timeline-end.v1")};
    QString saveError;
    const QString portableSamplePath = store.save(sample, &saveError);
    CHECK(!portableSamplePath.isEmpty());
    const QString legacyPath = store.directory() +
        QStringLiteral("/progress-ui-fixture.gvt");
    CHECK(gvtSaveFile(sample, legacyPath, &saveError));
    store.reload();
    CHECK(store.all().size() == 2);

    // The editor's typed working document is undoable and delegates final
    // schema/reference validation to the normal safe portable reader.
    {
        gvt::TransitionEditorDocument document;
        document.reset(sample);
        CHECK(!document.isDirty());
        CHECK(document.validationErrors().isEmpty());
        document.mutate(QStringLiteral("shorten end"), [](gvt::GvtFile& file) {
            file.endBeat = 3.0;
        });
        CHECK(document.isDirty());
        CHECK(document.validationErrors().isEmpty());
        document.undoStack()->undo();
        CHECK(document.validationErrors().isEmpty());
        CHECK(!document.isDirty());
        document.mutate(QStringLiteral("broken semantic reference"),
                        [](gvt::GvtFile& file) {
            file.events.front().role = gvt::Role::ToDeck;
            file.events.front().control = gvt::ControlId::TransitionCue1;
            file.events.front().cueId = QStringLiteral("missing-cue");
        });
        CHECK(document.validationErrors().join(QStringLiteral(" ")).contains(
            QStringLiteral("unknown cue"), Qt::CaseInsensitive));
    }
    auto outgoingTrack =
        makeTrack(sample.from.title, sample.from.fingerprint);
    outgoingTrack->firstBeatSec = 0.5;
    outgoingTrack->canonicalBeatOffset = 0.25;
    engine.deck(0).loadTrack(outgoingTrack);
    engine.deck(1).loadTrack(
        makeTrack(sample.to.title, sample.to.fingerprint));
    engine.deck(0).tempoRatio.store(0.98);
    engine.deck(0).track()->hotCues[0] = 1.0;

    {
        gvt::MainWindow window(&bus, &engine, &library, &store,
                               &transitionRecorder, &transitionPlayer, &midi);
        auto* libraryToggle = window.findChild<QPushButton*>(
            QStringLiteral("libraryVisibilityToggle"));
        CHECK(libraryToggle != nullptr);
        CHECK(libraryToggle && window.statusBar()->isAncestorOf(libraryToggle));
        CHECK(window.findChild<QPushButton*>(
                  QStringLiteral("masterRecordButton")) == nullptr);
        auto* libraryPanel = window.findChild<gvt::LibraryWidget*>();
        auto* crateTree = window.findChild<QTreeWidget*>(
            QStringLiteral("libraryCrateTree"));
        auto* trackTable = window.findChild<QTableView*>(
            QStringLiteral("trackLibraryTable"));
        auto* recommendedFilter = window.findChild<QCheckBox*>(
            QStringLiteral("recommendedLibraryFilter"));
        auto* hardwareStatus = window.findChild<QLabel*>(
            QStringLiteral("hardwareSyncStatus"));
        auto* showHardware = window.findChild<QCheckBox*>(
            QStringLiteral("showHardwareState"));
        auto* getHardwareState = window.findChild<QPushButton*>(
            QStringLiteral("getHardwareState"));
        auto* freezeHardware = window.findChild<QCheckBox*>(
            QStringLiteral("freezeHardwareInput"));
        auto* tutorView = window.findChild<QPushButton*>(
            QStringLiteral("transitionTutorView"));
        window.resize(1200, 720);
        window.show();
        app.processEvents();
        CHECK(libraryPanel != nullptr);
        CHECK(crateTree != nullptr);
        CHECK(recommendedFilter != nullptr);
        CHECK(recommendedFilter && recommendedFilter->isChecked());
        CHECK(hardwareStatus != nullptr);
        CHECK(hardwareStatus && hardwareStatus->text() ==
                                    QStringLiteral("HW UNKNOWN"));
        CHECK(showHardware != nullptr);
        CHECK(showHardware && !showHardware->isChecked());
        CHECK(showHardware && !showHardware->isEnabled());
        CHECK(getHardwareState != nullptr);
        CHECK(getHardwareState && !getHardwareState->isEnabled());
        CHECK(freezeHardware != nullptr);
        CHECK(freezeHardware && !freezeHardware->isChecked());
        CHECK(freezeHardware && !freezeHardware->isEnabled());
        CHECK(tutorView != nullptr);

        CHECK(QMetaObject::invokeMethod(
            &window, "onMidiConnection", Qt::DirectConnection,
            Q_ARG(bool, true),
            Q_ARG(QString, QStringLiteral("Test FLX4"))));
        CHECK(showHardware && showHardware->isEnabled());
        CHECK(getHardwareState && getHardwareState->isEnabled());
        CHECK(freezeHardware && freezeHardware->isEnabled());

        // Manual freeze is session state, survives the no-controller state,
        // and prevents opening Tutor until explicitly cleared. While Tutor
        // is open, the freeze control itself cannot be enabled.
        if (freezeHardware) freezeHardware->click();
        CHECK(midi.hardwareInputFrozen());
        CHECK(hardwareStatus && hardwareStatus->text().contains(
                                     QStringLiteral("FROZEN")));
        if (tutorView) tutorView->click();
        CHECK(tutorView && !tutorView->isChecked());
        if (freezeHardware) freezeHardware->click();
        CHECK(!midi.hardwareInputFrozen());
        // WET is on each deck, unlike the EQ/fader controls in the mixer.
        // Unfreezing must make its pending hardware pickup visible there.
        for (auto* deckWidget : window.findChildren<gvt::DeckWidget*>()) {
            QWidget* wet = deckWidget->controlWidget(gvt::ControlId::FxWet);
            CHECK(wet != nullptr);
            QWidget* pickup = wet ? wet->findChild<QWidget*>(
                QStringLiteral("pickupTargetOverlay")) : nullptr;
            CHECK(pickup && pickup->isVisible());
            CHECK(pickup && pickup->geometry() == wet->rect());
        }
        if (tutorView) tutorView->click();
        CHECK(tutorView && tutorView->isChecked());
        CHECK(freezeHardware && !freezeHardware->isEnabled());
        auto* tutorialSurface =
            window.findChild<gvt::Flx4TutorialWidget*>();
        CHECK(tutorialSurface != nullptr);
        if (tutorialSurface) {
            for (int deck = 0; deck < 2; ++deck) {
                const QRectF shift = tutorialSurface
                    ->property(deck == 0 ? "deck0ShiftRect"
                                         : "deck1ShiftRect").toRectF();
                const QRectF cue = tutorialSurface
                    ->property(deck == 0 ? "deck0CueRect"
                                         : "deck1CueRect").toRectF();
                const QRectF play = tutorialSurface
                    ->property(deck == 0 ? "deck0PlayRect"
                                         : "deck1PlayRect").toRectF();
                CHECK(shift.center().x() == cue.center().x());
                CHECK(cue.center().x() == play.center().x());
                CHECK(shift.bottom() < cue.top());
                CHECK(cue.bottom() < play.top());
                CHECK(shift.width() < cue.width());
                CHECK(cue.width() < play.width());
            }
            CHECK(!tutorialSurface->property("vinylModeDrawn").toBool());
        }
        if (tutorView) tutorView->click();
        CHECK(tutorView && !tutorView->isChecked());
        CHECK(freezeHardware && freezeHardware->isEnabled());
        CHECK(QMetaObject::invokeMethod(
            &window, "onMidiConnection", Qt::DirectConnection,
            Q_ARG(bool, false), Q_ARG(QString, QString())));
        CHECK(showHardware && !showHardware->isEnabled());
        CHECK(getHardwareState && !getHardwareState->isEnabled());
        CHECK(freezeHardware && !freezeHardware->isEnabled());
        CHECK(crateTree && crateTree->topLevelItemCount() >= 2);
        CHECK(crateTree &&
              crateTree->topLevelItem(1)->text(0).startsWith(
                  QStringLiteral("Undone")));
        CHECK(crateTree && crateTree->property("undoneCount").toInt() == 0);
        auto* trackProxy = trackTable
            ? qobject_cast<QSortFilterProxyModel*>(trackTable->model())
            : nullptr;
        CHECK(trackProxy != nullptr);
        CHECK(trackProxy && trackProxy->sortRole() == Qt::UserRole);
        CHECK(libraryPanel && !libraryPanel->isHidden());
        if (libraryToggle) libraryToggle->click();
        CHECK(libraryPanel && libraryPanel->isHidden());
        CHECK(libraryToggle && libraryToggle->text().contains(
                                   QStringLiteral("SHOW LIBRARY")));
        if (libraryToggle) libraryToggle->click();
        CHECK(libraryPanel && !libraryPanel->isHidden());
        CHECK(libraryToggle && libraryToggle->text().contains(
                                   QStringLiteral("HIDE LIBRARY")));
        auto* transitionTab = window.findChild<QPushButton*>(
            QStringLiteral("transitionLibraryTab"));
        auto* newTransition = window.findChild<QPushButton*>(
            QStringLiteral("newTransitionButton"));
        auto* transitionGraph = window.findChild<QPushButton*>(
            QStringLiteral("transitionGraphButton"));
        auto* librarySearch = window.findChild<QLineEdit*>(
            QStringLiteral("librarySearchField"));
        auto* legacyFilter = window.findChild<QCheckBox*>(
            QStringLiteral("legacyTransitionFilter"));
        auto* portableFilter = window.findChild<QCheckBox*>(
            QStringLiteral("portableTransitionFilter"));
        auto* transitionTable = window.findChild<QTableView*>(
            QStringLiteral("transitionLibraryTable"));
        CHECK(transitionTab != nullptr);
        CHECK(newTransition != nullptr);
        CHECK(transitionGraph != nullptr);
        CHECK(librarySearch != nullptr);
        CHECK(legacyFilter != nullptr);
        CHECK(portableFilter != nullptr);
        CHECK(transitionTable != nullptr);
        if (transitionTab) transitionTab->click();
        app.processEvents();
        CHECK(legacyFilter && legacyFilter->isVisible());
        CHECK(portableFilter && portableFilter->isVisible());
        CHECK(transitionGraph && transitionGraph->isVisible());
        const int graphRight = transitionGraph
                                   ? transitionGraph
                                         ->mapTo(&window,
                                                 transitionGraph->rect().topRight())
                                         .x()
                                   : 0;
        const int graphLeft = transitionGraph
                                  ? transitionGraph
                                        ->mapTo(&window,
                                                transitionGraph->rect().topLeft())
                                        .x()
                                  : 0;
        const int searchRight = librarySearch
                                    ? librarySearch
                                          ->mapTo(&window,
                                                  librarySearch->rect().topRight())
                                          .x()
                                    : 0;
        const int newLeft = newTransition
                                ? newTransition
                                      ->mapTo(&window,
                                              newTransition->rect().topLeft())
                                      .x()
                                : 0;
        CHECK(transitionGraph && newTransition && graphRight <= newLeft);
        CHECK(librarySearch && transitionGraph && searchRight <= graphLeft);
        CHECK(legacyFilter && !legacyFilter->isChecked());
        CHECK(portableFilter && portableFilter->isChecked());
        CHECK(transitionTable && transitionTable->model()->rowCount() == 1);
        if (legacyFilter) legacyFilter->setChecked(true);
        CHECK(transitionTable && transitionTable->model()->rowCount() == 2);
        if (portableFilter) portableFilter->setChecked(false);
        CHECK(transitionTable && transitionTable->model()->rowCount() == 1);
        if (legacyFilter) legacyFilter->setChecked(false);
        CHECK(transitionTable && transitionTable->model()->rowCount() == 0);
        if (portableFilter) portableFilter->setChecked(true);
        CHECK(transitionTable && transitionTable->model()->rowCount() == 1);

        // The graph button consumes chrome-row search space immediately to
        // the left of New and opens a distinct force-directed planning window.
        if (transitionGraph) transitionGraph->click();
        app.processEvents();
        auto* graphWindow = window.findChild<QWidget*>(
            QStringLiteral("transitionGraphWindow"));
        auto* graphCanvas = graphWindow
            ? graphWindow->findChild<QWidget*>(
                  QStringLiteral("transitionGraphCanvas"))
            : nullptr;
        auto* graphZoom = graphWindow
            ? graphWindow->findChild<QSlider*>(
                  QStringLiteral("transitionGraphZoomSlider"))
            : nullptr;
        CHECK(graphWindow != nullptr);
        CHECK(graphWindow && graphWindow->isWindow());
        CHECK(graphWindow && graphWindow->isVisible());
        CHECK(graphCanvas != nullptr);
        CHECK(graphZoom != nullptr);
        CHECK(graphZoom && graphZoom->value() == 100);
        CHECK(graphZoom && graphZoom->minimum() == 35);
        CHECK(graphZoom && graphZoom->maximum() == 250);
        CHECK(graphCanvas && graphCanvas->property("nodeCount").toInt() == 2);
        CHECK(graphCanvas && graphCanvas->property("edgeCount").toInt() == 2);
        CHECK(graphCanvas &&
              graphCanvas->property("edgeLabelCount").toInt() == 2);
        CHECK(graphCanvas &&
              graphCanvas->property("currentDeckANode").toInt() == 0);
        CHECK(graphCanvas &&
              graphCanvas->property("currentDeckBNode").toInt() == 1);
        if (graphZoom) graphZoom->setValue(150);
        app.processEvents();
        CHECK(graphCanvas &&
              graphCanvas->property("zoomPercent").toInt() == 150);
        if (graphCanvas) {
            const QPoint nodeCenter =
                graphCanvas->property("node0Center").toPointF().toPoint();
            sendMouse(graphCanvas, QEvent::MouseMove, nodeCenter,
                      graphCanvas->mapToGlobal(nodeCenter), Qt::NoButton,
                      Qt::NoButton);
            app.processEvents();
            CHECK(graphCanvas->property("hoveredNodeIndex").toInt() == 0);
            CHECK(graphCanvas->property("hoverRouteNodeCount").toInt() == 2);
            CHECK(graphCanvas->property("hoverRouteDurationSeconds")
                      .toDouble() > 0.0);

            // Exercise real drag event paths. In particular, these must not
            // ask macOS to synthesize a new Qt cursor at mouse-down time.
            const QPoint draggedCenter = nodeCenter + QPoint(36, 24);
            sendMouse(graphCanvas, QEvent::MouseButtonPress, nodeCenter,
                      graphCanvas->mapToGlobal(nodeCenter), Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(graphCanvas, QEvent::MouseMove, draggedCenter,
                      graphCanvas->mapToGlobal(draggedCenter), Qt::NoButton,
                      Qt::LeftButton);
            sendMouse(graphCanvas, QEvent::MouseButtonRelease, draggedCenter,
                      graphCanvas->mapToGlobal(draggedCenter), Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            const QPoint movedCenter =
                graphCanvas->property("node0Center").toPointF().toPoint();
            CHECK((movedCenter - nodeCenter).manhattanLength() > 10);

            const QPoint emptyStart(8, graphCanvas->height() - 8);
            const QPoint emptyEnd = emptyStart + QPoint(28, -18);
            const QPointF oldPan = graphCanvas->property("panOffset").toPointF();
            sendMouse(graphCanvas, QEvent::MouseButtonPress, emptyStart,
                      graphCanvas->mapToGlobal(emptyStart), Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(graphCanvas, QEvent::MouseMove, emptyEnd,
                      graphCanvas->mapToGlobal(emptyEnd), Qt::NoButton,
                      Qt::LeftButton);
            sendMouse(graphCanvas, QEvent::MouseButtonRelease, emptyEnd,
                      graphCanvas->mapToGlobal(emptyEnd), Qt::LeftButton,
                      Qt::NoButton);
            CHECK(graphCanvas->property("panOffset").toPointF() != oldPan);
        }
        if (graphWindow) graphWindow->close();
        auto* midiStatus = window.findChild<QLabel*>(
            QStringLiteral("midiConnectionStatus"));
        CHECK(midiStatus != nullptr);
        const int libraryRight = libraryToggle
                                     ? libraryToggle
                                           ->mapTo(window.statusBar(),
                                                   libraryToggle->rect().topRight())
                                           .x()
                                     : 0;
        const int midiLeft = midiStatus
                                 ? midiStatus
                                       ->mapTo(window.statusBar(),
                                               midiStatus->rect().topLeft())
                                       .x()
                                 : 0;
        CHECK(libraryToggle && midiStatus && libraryRight <= midiLeft);
        window.statusBar()->showMessage(
            QStringLiteral("Tutor view opened — temporary status text"));
        app.processEvents();
        CHECK(libraryToggle && libraryToggle->isVisible());
        window.statusBar()->clearMessage();

        // A mouse can reproduce the controller's HOT CUE hold + PLAY latch:
        // press a mapped pad, drag onto PLAY, then release there.
        auto* pad = window.findChild<QPushButton*>(
            QStringLiteral("deck0PerformancePad1"));
        auto* play = window.findChild<QPushButton*>(
            QStringLiteral("deck0PlayButton"));
        auto* wheel = window.findChild<QWidget*>(
            QStringLiteral("deck0JogWheel"));
        auto* deckPanel = window.findChild<QWidget*>(
            QStringLiteral("deckWidget0"));
        auto* beatCounter = window.findChild<QLabel*>(
            QStringLiteral("deck0BeatCounter"));
        auto* customMode = window.findChild<QPushButton*>(
            QStringLiteral("deck0PerformanceModesampler"));
        auto* hotCueMode = window.findChild<QPushButton*>(
            QStringLiteral("deck0PerformanceModehotCue"));
        auto* padFeedback = window.findChild<QLabel*>(
            QStringLiteral("deck0PadFeedback"));
        auto* loopIn = window.findChild<QPushButton*>(
            QStringLiteral("deck0LoopInButton"));
        CHECK(pad != nullptr);
        CHECK(play != nullptr);
        CHECK(wheel != nullptr);
        CHECK(deckPanel != nullptr);
        CHECK(beatCounter != nullptr);
        CHECK(customMode != nullptr);
        CHECK(hotCueMode != nullptr);
        CHECK(padFeedback != nullptr);
        CHECK(loopIn != nullptr);

        // Setup guidance carries exact target geometry for the tempo slider,
        // a channel fader, a centered dial, and a discrete state control.
        auto* setupTransitionPanel =
            window.findChild<gvt::TransitionPanel*>();
        CHECK(setupTransitionPanel != nullptr);
        if (setupTransitionPanel)
            setupTransitionPanel->selectTransitionFile(portableSamplePath);
        app.processEvents();
        const QList<QWidget*> setupTargets =
            window.findChildren<QWidget*>(QStringLiteral("setupTargetOverlay"));
        CHECK(setupTargets.size() >= 4);
        auto hasSetupTarget = [&setupTargets](QWidget* parent, double fraction,
                                               const QString& tooltipPart) {
            return std::any_of(
                setupTargets.begin(), setupTargets.end(),
                [=](QWidget* overlay) {
                    return overlay && overlay->parentWidget() == parent &&
                           std::fabs(overlay->property("targetFraction")
                                         .toDouble() - fraction) < 0.02 &&
                           overlay->property("targetText").toString().contains(
                               tooltipPart);
                });
        };
        auto* typedDeckPanel = qobject_cast<gvt::DeckWidget*>(deckPanel);
        auto* mixerPanel = window.findChild<gvt::MixerWidget*>();
        CHECK(typedDeckPanel != nullptr);
        CHECK(mixerPanel != nullptr);
        CHECK(typedDeckPanel && hasSetupTarget(
                  typedDeckPanel->controlWidget(gvt::ControlId::Tempo), 0.5,
                  QStringLiteral("×1.000")));
        CHECK(mixerPanel && hasSetupTarget(
                  mixerPanel->controlWidget(0, gvt::ControlId::Fader),
                  0.25, QStringLiteral("25.0%")));
        CHECK(mixerPanel && hasSetupTarget(
                  mixerPanel->controlWidget(0, gvt::ControlId::EqHigh),
                  0.8, QStringLiteral("80.0%")));
        CHECK(typedDeckPanel && hasSetupTarget(
                  typedDeckPanel->controlWidget(gvt::ControlId::Quantize),
                  0.0, QStringLiteral("Off")));

        // An unchanged PRIME tempo target must be reprojected as soon as the
        // range changes, including when it was clipped at either old limit.
        for (int deckIndex : {0, 1}) {
            auto* deckWidget = window.findChild<gvt::DeckWidget*>(
                QStringLiteral("deckWidget%1").arg(deckIndex));
            auto* range = window.findChild<QToolButton*>(
                QStringLiteral("deck%1TempoRange").arg(deckIndex));
            CHECK(deckWidget && range && range->menu());
            if (!deckWidget || !range || !range->menu()) continue;
            const double initialRange = engine.deck(deckIndex).tempoRange.load();
            const double initialRatio = engine.deck(deckIndex).tempoRatio.load();
            bus.dispatch({deckIndex, gvt::ControlId::TempoRange, 0.08}, gvt::Origin::Ui);
            const QList<gvt::ControlEvent> targets {
                {deckIndex, gvt::ControlId::Tempo, deckIndex == 0 ? 1.12 : 0.88}};
            CHECK(QMetaObject::invokeMethod(
                &window, "refreshSetupMismatchUi", Qt::DirectConnection,
                Q_ARG(QList<gvt::ControlEvent>, targets)));
            auto targetFraction = [&] {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                const auto overlays = deckWidget->controlWidget(gvt::ControlId::Tempo)
                    ->findChildren<QWidget*>(QStringLiteral("setupTargetOverlay"));
                CHECK(overlays.size() == 1);
                return overlays.isEmpty() ? -1.0
                    : overlays.front()->property("targetFraction").toDouble();
            };
            CHECK(std::fabs(targetFraction() - (deckIndex == 0 ? 1.0 : 0.0)) < 0.001);
            // Exercise the actual range-menu action, then a hardware event.
            for (auto* action : range->menu()->actions())
                if (std::fabs(action->data().toDouble() - 0.16) < 1.0e-9)
                    action->trigger();
            CHECK(std::fabs(targetFraction() - (deckIndex == 0 ? 0.875 : 0.125)) < 0.001);
            bus.dispatch({deckIndex, gvt::ControlId::TempoRange, 0.5}, gvt::Origin::Midi);
            CHECK(std::fabs(targetFraction() - (deckIndex == 0 ? 0.62 : 0.38)) < 0.001);
            CHECK(engine.deck(deckIndex).tempoRatio.load() == initialRatio);
            bus.dispatch({deckIndex, gvt::ControlId::TempoRange, initialRange}, gvt::Origin::Ui);
        }
        const QList<gvt::ControlEvent> noTargets;
        CHECK(QMetaObject::invokeMethod(
            &window, "refreshSetupMismatchUi", Qt::DirectConnection,
            Q_ARG(QList<gvt::ControlEvent>, noTargets)));
        engine.deck(0).tempoRatio.store(1.0);

        // The beat counter uses the same canonical grid (including the local
        // catalog offset) that portable transition coordinates use.
        engine.deck(0).seekSec(1.1);
        CHECK(deckPanel && QMetaObject::invokeMethod(
              deckPanel, "refresh", Qt::DirectConnection));
        CHECK(beatCounter && beatCounter->text() == QStringLiteral("BEAT 1.45"));
        engine.deck(0).seekSec(0.125);
        CHECK(deckPanel && QMetaObject::invokeMethod(
              deckPanel, "refresh", Qt::DirectConnection));
        CHECK(beatCounter && beatCounter->text() == QStringLiteral("BEAT -0.50"));
        outgoingTrack->bpm = 0.0;
        CHECK(deckPanel && QMetaObject::invokeMethod(
              deckPanel, "refresh", Qt::DirectConnection));
        CHECK(beatCounter && beatCounter->text() == QStringLiteral("BEAT —"));
        outgoingTrack->bpm = 120.0;

        // A transient, deliberately long CUSTOM hint is constrained to the
        // existing feedback row and cannot widen the deck or move controls.
        if (deckPanel && customMode && padFeedback && loopIn) {
            const int widthBefore = deckPanel->sizeHint().width();
            const QPoint loopBefore = loopIn->mapTo(deckPanel, QPoint());
            customMode->click();
            app.processEvents();
            CHECK(padFeedback->toolTip().contains(QStringLiteral("CUSTOM")));
            CHECK(deckPanel->sizeHint().width() == widthBefore);
            CHECK(loopIn->mapTo(deckPanel, QPoint()) == loopBefore);
            if (hotCueMode) hotCueMode->click();
        }

        engine.deck(0).stop();
        engine.deck(0).seekSec(0.25);
        app.processEvents();
        if (pad && play) {
            const QPoint padLocal = pad->rect().center();
            const QPoint padGlobal = pad->mapToGlobal(padLocal);
            sendMouse(pad, QEvent::MouseButtonPress, padLocal, padGlobal,
                      Qt::LeftButton, Qt::LeftButton);
            CHECK(engine.deck(0).previewActive());
            const QPoint playGlobal = play->mapToGlobal(play->rect().center());
            const QPoint overPlay = pad->mapFromGlobal(playGlobal);
            sendMouse(pad, QEvent::MouseMove, overPlay, playGlobal,
                      Qt::NoButton, Qt::LeftButton);
            CHECK(play->property("hotCueDropTarget").toBool());
            sendMouse(pad, QEvent::MouseButtonRelease, overPlay, playGlobal,
                      Qt::LeftButton, Qt::NoButton);
            CHECK(!engine.deck(0).previewActive());
            CHECK(engine.deck(0).playing.load());
            CHECK(!play->property("hotCueDropTarget").toBool());

            // Releasing on the pad retains ordinary momentary-preview
            // behavior; only a drop over PLAY latches it.
            engine.deck(0).stop();
            engine.deck(0).seekSec(0.25);
            sendMouse(pad, QEvent::MouseButtonPress, padLocal, padGlobal,
                      Qt::LeftButton, Qt::LeftButton);
            CHECK(engine.deck(0).previewActive());
            sendMouse(pad, QEvent::MouseButtonRelease, padLocal, padGlobal,
                      Qt::LeftButton, Qt::NoButton);
            CHECK(!engine.deck(0).previewActive());
            CHECK(!engine.deck(0).playing.load());
        }

        // While playing, bottom-left is forward and top-right is backward.
        // Projection is incremental, so reversing within one drag reverses
        // naturally; movement on the perpendicular diagonal is ignored.
        if (wheel) {
            engine.deck(0).seekSec(1.0);
            engine.deck(0).play();
            const QPoint center = wheel->rect().center();
            const QPoint bottomLeft = center + QPoint(-24, 24);
            const QPoint topRight = center + QPoint(24, -24);
            const QPoint bottomRight = center + QPoint(20, 20);
            lastMousePlatterControl = gvt::ControlId::Count;
            sendMouse(wheel, QEvent::MouseButtonPress, center,
                      wheel->mapToGlobal(center), Qt::LeftButton,
                      Qt::LeftButton);
            CHECK(engine.deck(0).playing.load());
            sendMouse(wheel, QEvent::MouseMove, bottomLeft,
                      wheel->mapToGlobal(bottomLeft), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterControl ==
                  gvt::ControlId::PlatterScratch);
            CHECK(lastMousePlatterValue > 0.67);
            CHECK(lastMousePlatterValue < 0.69);
            const double playingAdjustment =
                engine.deck(0).positionSec() - 1.0;
            CHECK(playingAdjustment > 0.0065);
            CHECK(playingAdjustment < 0.0071);
            CHECK(engine.deck(0).playing.load());

            sendMouse(wheel, QEvent::MouseMove, center,
                      wheel->mapToGlobal(center), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterValue < -0.67);
            CHECK(lastMousePlatterValue > -0.69);
            CHECK(std::fabs(engine.deck(0).positionSec() - 1.0) < 0.001);
            sendMouse(wheel, QEvent::MouseMove, topRight,
                      wheel->mapToGlobal(topRight), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterValue < -0.67);
            CHECK(lastMousePlatterValue > -0.69);
            CHECK(engine.deck(0).positionSec() > 0.993);
            CHECK(engine.deck(0).positionSec() < 0.994);
            CHECK(deckPanel && QMetaObject::invokeMethod(
                  deckPanel, "refresh", Qt::DirectConnection));
            const double expectedRotation =
                std::fmod(engine.deck(0).positionSec() * 200.0, 360.0);
            CHECK(std::fabs(wheel->property("rotationDegrees").toDouble() -
                            expectedRotation) < 0.1);
            float scratchAudio[512 * 2] {};
            engine.renderOffline(scratchAudio, 512);
            CHECK(engine.deck(0).positionSec() >
                  0.993 + 512.0 / gvt::kSampleRate);
            sendMouse(wheel, QEvent::MouseButtonRelease, topRight,
                      wheel->mapToGlobal(topRight), Qt::LeftButton,
                      Qt::NoButton);
            CHECK(engine.deck(0).playing.load());

            engine.deck(0).stop();
            engine.deck(0).seekSec(1.0);
            lastMousePlatterControl = gvt::ControlId::Count;
            sendMouse(wheel, QEvent::MouseButtonPress, center,
                      wheel->mapToGlobal(center), Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(wheel, QEvent::MouseMove, bottomRight,
                      wheel->mapToGlobal(bottomRight), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterControl == gvt::ControlId::Count);
            CHECK(std::fabs(engine.deck(0).positionSec() - 1.0) < 0.001);
            sendMouse(wheel, QEvent::MouseButtonRelease, bottomRight,
                      wheel->mapToGlobal(bottomRight), Qt::LeftButton,
                      Qt::NoButton);

            // A paused deck gets the same direct fine positioning and remains
            // paused throughout the gesture.
            engine.deck(0).stop();
            engine.deck(0).seekSec(1.0);
            lastMousePlatterControl = gvt::ControlId::Count;
            sendMouse(wheel, QEvent::MouseButtonPress, center,
                      wheel->mapToGlobal(center), Qt::LeftButton,
                      Qt::LeftButton);
            CHECK(!engine.deck(0).playing.load());
            sendMouse(wheel, QEvent::MouseMove, bottomLeft,
                      wheel->mapToGlobal(bottomLeft), Qt::NoButton,
                      Qt::LeftButton);
            CHECK(lastMousePlatterControl ==
                  gvt::ControlId::PlatterScratch);
            CHECK(lastMousePlatterValue > 0.67);
            CHECK(lastMousePlatterValue < 0.69);
            const double mouseAdjustment =
                engine.deck(0).positionSec() - 1.0;
            CHECK(mouseAdjustment > 0.0065);
            CHECK(mouseAdjustment < 0.0071);
            sendMouse(wheel, QEvent::MouseButtonRelease, bottomLeft,
                      wheel->mapToGlobal(bottomLeft), Qt::LeftButton,
                      Qt::NoButton);
            CHECK(!engine.deck(0).playing.load());
            CHECK(mousePlatterTouchEvents == 0);
        }

        auto* table = window.findChild<QTableWidget*>(
            QStringLiteral("transitionEventSequence"));
        auto* human = window.findChild<QPushButton*>(
            QStringLiteral("humanSequenceMode"));
        auto* raw = window.findChild<QPushButton*>(
            QStringLiteral("rawSequenceMode"));
        auto* showCues = window.findChild<QCheckBox*>(
            QStringLiteral("showImportantTransitionCues"));
        CHECK(table != nullptr);
        CHECK(human && human->isChecked());
        CHECK(raw && !raw->isChecked());
        CHECK(showCues && showCues->isChecked());
        CHECK(showCues && showCues->isVisible());
        CHECK(headerText(table, 0) == QStringLiteral("Outgoing Beat"));
        CHECK(headerText(table, 1) == QStringLiteral("Outgoing Action"));
        CHECK(headerText(table, 2) == QStringLiteral("Incoming Action"));
        CHECK(headerText(table, 3) == QStringLiteral("Incoming Beat"));
        CHECK(headerText(table, 4) == QStringLiteral("Label"));
        CHECK(table && table->rowCount() == 2);
        CHECK(table && table->item(0, 1) &&
              table->item(0, 1)->text().contains(
                  QStringLiteral("TRANSITION STARTS")));
        CHECK(table && table->item(0, 0) &&
              table->item(0, 0)->data(Qt::UserRole).toInt() == -1);

        auto* transitionPanel = window.findChild<gvt::TransitionPanel*>();
        CHECK(transitionPanel != nullptr);
        std::array<int, 2> importantCueCounts {-1, -1};
        if (transitionPanel) {
            QObject::connect(
                transitionPanel, &gvt::TransitionPanel::cueMarkersChanged,
                [&importantCueCounts](int deck, const QList<double>& seconds,
                                      const QStringList&) {
                    if (deck >= 0 && deck < 2)
                        importantCueCounts[static_cast<std::size_t>(deck)] =
                            seconds.size();
                });
        }
        if (showCues) showCues->setChecked(false);
        CHECK(importantCueCounts[0] == 0);
        CHECK(importantCueCounts[1] == 0);
        if (showCues) showCues->setChecked(true);
        CHECK(importantCueCounts[0] == 0);
        CHECK(importantCueCounts[1] == 1);
        CHECK(transitionPanel && QMetaObject::invokeMethod(
              transitionPanel, "onProgress", Qt::DirectConnection,
              Q_ARG(double, 1.0), Q_ARG(double, 4.0)));
        CHECK(table && table->property("timelineProgressRow").toInt() == 0);
        CHECK(table && std::fabs(
                  table->property("timelineProgressFraction").toDouble() -
                  0.5) < 0.01);

        if (raw) raw->click();
        CHECK(raw && raw->isChecked());
        CHECK(showCues && !showCues->isVisible());
        CHECK(headerText(table, 0) == QStringLiteral("Beat"));
        CHECK(headerText(table, 1) == QStringLiteral("Target"));
        CHECK(headerText(table, 2) == QStringLiteral("Action"));
        CHECK(headerText(table, 3) == QStringLiteral("Value"));
        CHECK(headerText(table, 4) == QStringLiteral("Cue label"));
        CHECK(table && table->rowCount() == 2);
        CHECK(table && table->item(0, 2) &&
              table->item(0, 2)->text() ==
                  QStringLiteral("Transition starts"));
        CHECK(table && table->property("timelineProgressRow").toInt() == 0);
        CHECK(transitionPanel && QMetaObject::invokeMethod(
              transitionPanel, "onProgress", Qt::DirectConnection,
              Q_ARG(double, 2.0), Q_ARG(double, 4.0)));
        CHECK(table && table->property("timelineProgressRow").toInt() == 1);

        if (human) human->click();
        CHECK(human && human->isChecked());
        CHECK(showCues && showCues->isVisible());
        CHECK(headerText(table, 0) == QStringLiteral("Outgoing Beat"));

        // The main-control Tutorial layer starts from authored initial state,
        // follows linear/S-curve values in real time, and never derives a
        // target from preserved compatibility-only crossfader data.
        gvt::GvtFile tutorialSample = sample;
        tutorialSample.id.clear();
        tutorialSample.filePath.clear();
        tutorialSample.sourceFormat =
            gvt::TransitionSourceFormat::Unsaved;
        tutorialSample.name = QStringLiteral("Tutorial targets UI fixture");
        tutorialSample.anchorFromBeat = 0.0;
        tutorialSample.anchorToBeat = 0.0;
        tutorialSample.initialFrom.positionBeat = 0.0;
        tutorialSample.initialFrom.cueBeat = 0.0;
        tutorialSample.initialFrom.fader = 0.2;
        tutorialSample.initialFrom.fxWet = 0.75;
        tutorialSample.initialTo.positionBeat = 0.0;
        tutorialSample.initialTo.cueBeat = 0.0;
        tutorialSample.initialTo.eqLow = 0.8;
        tutorialSample.initialTo.fxWet = 0.25;
        tutorialSample.events = {
            {0.0, gvt::Role::FromDeck, gvt::ControlId::Fader, 0.2,
             gvt::Curve::Step},
            {0.0, gvt::Role::ToDeck, gvt::ControlId::EqLow, 0.8,
             gvt::Curve::Step},
            {1.0, gvt::Role::Mixer, gvt::ControlId::Crossfader, 0.9,
             gvt::Curve::Step},
            {4.0, gvt::Role::FromDeck, gvt::ControlId::Fader, 0.8,
             gvt::Curve::Linear},
            {4.0, gvt::Role::ToDeck, gvt::ControlId::EqLow, 0.2,
             gvt::Curve::SCurve},
        };
        tutorialSample.endBeat = 8.0;
        const QString tutorialSamplePath =
            store.save(tutorialSample, &saveError);
        CHECK(!tutorialSamplePath.isEmpty());
        app.processEvents();
        if (transitionPanel)
            transitionPanel->selectTransitionFile(tutorialSamplePath);
        engine.deck(0).fader.store(1.0);
        engine.deck(1).eqLow.store(0.0);
        engine.deck(0).fxWet.store(0.5);
        engine.deck(1).fxWet.store(0.5);
        engine.crossfader.store(0.37f);
        QList<gvt::ControlEvent> tutorialTargets;
        if (transitionPanel) {
            QObject::connect(
                transitionPanel,
                &gvt::TransitionPanel::tutorialTargetsChanged,
                [&tutorialTargets](const QList<gvt::ControlEvent>& targets) {
                    tutorialTargets = targets;
                });
        }
        auto* tutorialPerform = window.findChild<QPushButton*>(
            QStringLiteral("transitionPerform"));
        auto* tutorialPrime = window.findChild<QPushButton*>(
            QStringLiteral("transitionPrime"));
        auto* tutorialAbort = window.findChild<QPushButton*>(
            QStringLiteral("transitionAbort"));
        auto* primeSetupStatus = window.findChild<QLabel*>(
            QStringLiteral("transitionSetupStatus"));
        CHECK(tutorialPerform != nullptr);
        CHECK(tutorialPrime != nullptr);
        CHECK(tutorialAbort != nullptr);
        if (tutorView && !tutorView->isChecked()) tutorView->click();

        // Tutor PRIME is strict and does not silently move the musical
        // controls. Its failed target set persists, resolves control by
        // control, and a second explicit click is needed after correction.
        engine.deck(0).stop();
        engine.deck(0).seekSec(0.0);
        if (transitionPanel) {
            for (QTimer* timer : transitionPanel->findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(
                        timer, "timeout", Qt::DirectConnection);
        }
        CHECK(tutorialPrime && tutorialPrime->isEnabled());
        if (tutorialPrime) tutorialPrime->click();
        CHECK(!transitionPlayer.isActive());
        CHECK(std::fabs(engine.crossfader.load() - 0.37) < 1e-6);
        CHECK(primeSetupStatus && primeSetupStatus->text().contains(
                                        QStringLiteral("PRIME not armed")));
        const int failedTargetCount = window.findChildren<QWidget*>(
            QStringLiteral("setupTargetOverlay")).size();
        CHECK(failedTargetCount >= 3);
        const auto wetSetupTarget = [&window](int deck) -> QWidget* {
            auto* deckWidget = window.findChild<gvt::DeckWidget*>(
                QStringLiteral("deckWidget%1").arg(deck));
            QWidget* wet = deckWidget
                ? deckWidget->controlWidget(gvt::ControlId::FxWet) : nullptr;
            return wet ? wet->findChild<QWidget*>(
                QStringLiteral("setupTargetOverlay")) : nullptr;
        };
        for (int deck = 0; deck < 2; ++deck) {
            QWidget* target = wetSetupTarget(deck);
            CHECK(target && target->isVisible());
            CHECK(target && std::fabs(
                target->property("targetFraction").toDouble() -
                (deck == 0 ? 0.75 : 0.25)) < 1e-9);
        }
        engine.deck(0).fader.store(0.2);
        if (transitionPanel) {
            for (QTimer* timer : transitionPanel->findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(
                        timer, "timeout", Qt::DirectConnection);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const int oneResolvedCount = window.findChildren<QWidget*>(
            QStringLiteral("setupTargetOverlay")).size();
        CHECK(oneResolvedCount > 0 && oneResolvedCount < failedTargetCount);
        engine.deck(0).eqHigh.store(0.8);
        engine.deck(0).quantizeHotCues.store(false);
        engine.deck(1).eqLow.store(0.8);
        engine.deck(0).fxWet.store(0.75);
        engine.deck(1).fxWet.store(0.25);
        if (transitionPanel) {
            for (QTimer* timer : transitionPanel->findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(
                        timer, "timeout", Qt::DirectConnection);
        }
        if (tutorialPrime) tutorialPrime->click();
        CHECK(transitionPlayer.isActive());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(wetSetupTarget(0) == nullptr);
        CHECK(wetSetupTarget(1) == nullptr);
        CHECK(std::fabs(engine.crossfader.load() - 0.37) < 1e-6);
        if (tutorialAbort) tutorialAbort->click();
        CHECK(!transitionPlayer.isActive());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CHECK(window.findChildren<QWidget*>(
                  QStringLiteral("setupTargetOverlay")).isEmpty());

        // Discrete loop mismatches must name the actual FLX4 gesture instead
        // of leaving a beginner with the opaque "loop on/off" field name.
        engine.deck(1).loopStartSec.store(0.0);
        engine.deck(1).loopEndSec.store(2.0);
        engine.deck(1).loopActive.store(true);
        if (transitionPanel) {
            for (QTimer* timer : transitionPanel->findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(
                        timer, "timeout", Qt::DirectConnection);
        }
        CHECK(tutorialPrime && tutorialPrime->isEnabled());
        if (tutorialPrime) tutorialPrime->click();
        CHECK(!transitionPlayer.isActive());
        CHECK(primeSetupStatus && primeSetupStatus->text().contains(
            QStringLiteral("press 4 BEAT/EXIT on Deck B to turn it OFF")));
        engine.deck(1).loopActive.store(false);

        // Tutor Perform accepts the deliberately restored imperfect state as
        // advisory guidance and produces the live green targets below.
        engine.deck(0).fader.store(1.0);
        engine.deck(0).eqHigh.store(0.5);
        engine.deck(0).quantizeHotCues.store(true);
        engine.deck(1).eqLow.store(0.0);
        if (tutorialPerform) tutorialPerform->click();
        CHECK(transitionPlayer.isActive());
        CHECK(transitionPanel && QMetaObject::invokeMethod(
              transitionPanel, "onProgress", Qt::DirectConnection,
              Q_ARG(double, 2.0), Q_ARG(double, 8.0)));
        if (transitionPanel) {
            for (QTimer* timer : transitionPanel->findChildren<QTimer*>())
                if (timer->interval() == 50)
                    QMetaObject::invokeMethod(
                        timer, "timeout", Qt::DirectConnection);
        }
        const auto targetValue = [&tutorialTargets](
                                     int deck, gvt::ControlId control) {
            const auto found = std::find_if(
                tutorialTargets.begin(), tutorialTargets.end(),
                [=](const gvt::ControlEvent& target) {
                    return target.deck == deck && target.id == control;
                });
            return found == tutorialTargets.end()
                       ? std::numeric_limits<double>::quiet_NaN()
                       : found->value;
        };
        CHECK(std::fabs(targetValue(0, gvt::ControlId::Fader) - 0.5) <
              1e-9);
        CHECK(std::fabs(targetValue(1, gvt::ControlId::EqLow) - 0.5) <
              1e-9);
        CHECK(std::none_of(
            tutorialTargets.begin(), tutorialTargets.end(),
            [](const gvt::ControlEvent& target) {
                return target.id == gvt::ControlId::Crossfader;
            }));
        CHECK(std::fabs(engine.crossfader.load() - 0.37) < 1e-6);
        CHECK(window.findChildren<QWidget*>(
                  QStringLiteral("tutorialTargetOverlay")).size() >= 2);
        if (tutorialAbort) tutorialAbort->click();
        CHECK(!transitionPlayer.isActive());
        CHECK(tutorialTargets.isEmpty());
        if (tutorView && tutorView->isChecked()) tutorView->click();

        // Stem-dependent imports are visibly blocked until their required
        // deck stems are prepared, and the action routes through MainWindow's
        // normal stem preparation path.
        gvt::GvtFile stemSample = sample;
        stemSample.id.clear();
        stemSample.filePath.clear();
        stemSample.sourceFormat = gvt::TransitionSourceFormat::Unsaved;
        stemSample.name = QStringLiteral("Stem preparation UI fixture");
        stemSample.events.insert(
            stemSample.events.begin() + 1,
            {3.0, gvt::Role::ToDeck, gvt::ControlId::StemVocals, 0.0,
             gvt::Curve::Step});
        const QString stemPath = store.save(stemSample, &saveError);
        CHECK(!stemPath.isEmpty());
        app.processEvents();
        int preparedDeck = -1;
        if (transitionPanel) {
            QObject::connect(
                transitionPanel,
                &gvt::TransitionPanel::stemPreparationRequested,
                [&preparedDeck](int deck) { preparedDeck = deck; });
            transitionPanel->selectTransitionFile(stemPath);
        }
        app.processEvents();
        auto* prepareStems = window.findChild<QPushButton*>(
            QStringLiteral("transitionPrepareStems"));
        auto* perform = window.findChild<QPushButton*>(
            QStringLiteral("transitionPerform"));
        auto* setupStatus = window.findChild<QLabel*>(
            QStringLiteral("transitionSetupStatus"));
        CHECK(prepareStems && prepareStems->isVisible());
        CHECK(prepareStems && prepareStems->isEnabled());
        CHECK(perform && !perform->isEnabled());
        CHECK(setupStatus && setupStatus->text().contains(
                                   QStringLiteral("uses stems"),
                                   Qt::CaseInsensitive));
        if (prepareStems) prepareStems->click();
        CHECK(preparedDeck == 1);

        // Library/context Edit opens the full visual editor window rather
        // than the retired source-only dialog.
        auto* editor = window.findChild<gvt::TransitionEditorWindow*>();
        CHECK(editor != nullptr);
        const auto portable = std::find_if(
            store.all().begin(), store.all().end(), [](const gvt::GvtFile& file) {
                return file.sourceFormat ==
                           gvt::TransitionSourceFormat::PortableYaml &&
                       file.name == QStringLiteral("Progress UI fixture");
            });
        CHECK(portable != store.all().end());
        if (editor && portable != store.all().end()) {
            editor->openTransition(*portable);
            app.processEvents();
            CHECK(editor->isVisible());
            CHECK(editor->isWindow());
            auto* editorTimeline = editor->findChild<gvt::TransitionTimelineView*>(
                QStringLiteral("transitionEditorTimeline"));
            CHECK(editorTimeline != nullptr);
            auto* editorEvents = editor->findChild<QTableWidget*>(
                QStringLiteral("transitionEditorEvents"));
            CHECK(editorEvents != nullptr);
            CHECK(editorEvents && editorEvents->rowCount() == 2);
            CHECK(editorEvents && !editorEvents->isRowHidden(0));
            CHECK(editorEvents && editorEvents->isRowHidden(1));
            auto* inspector = editor->findChild<QTabWidget*>(
                QStringLiteral("transitionEditorInspector"));
            // All sections remain reachable in a compact window, with direct
            // precise controls for the two fields users previously could not find.
            {
                auto* doc = editor->findChild<gvt::TransitionEditorDocument*>();
                auto* section = editor->findChild<QComboBox*>("transitionEditorSection");
                auto* tempoShortcut = editor->findChild<QPushButton*>("transitionEditorTempoShortcut");
                auto* loopsShortcut = editor->findChild<QPushButton*>("transitionEditorLoopsShortcut");
                auto* fieldsShortcut = editor->findChild<QPushButton*>("transitionEditorFieldsShortcut");
                auto* ratio = editor->findChild<QLineEdit*>("transitionEditorIncomingTempoRatio");
                auto* applyTempo = editor->findChild<QPushButton*>("transitionEditorApplyIncomingTempo");
                auto* start = editor->findChild<QLineEdit*>("transitionEditorDefinitionStart");
                auto* end = editor->findChild<QLineEdit*>("transitionEditorDefinitionEnd");
                auto* length = editor->findChild<QLabel*>("transitionEditorLoopLength");
                auto* applyPosition = editor->findChild<QPushButton*>("transitionEditorApplyDefinitionPosition");
                auto* fields = editor->findChild<gvt::TransitionFieldsEditor*>();
                CHECK(doc && section && tempoShortcut && loopsShortcut && fieldsShortcut && ratio &&
                      applyTempo && start && end && length && applyPosition && fields && inspector);
                if (doc && section && tempoShortcut && loopsShortcut && fieldsShortcut && ratio &&
                    applyTempo && start && end && length && applyPosition && fields && inspector) {
                    CHECK(section->count() == inspector->count());
                    editor->resize(1100, 700);
                    app.processEvents();
                    CHECK(editor->size() == QSize(1100, 700));
                    const auto visibleInEditor = [&](QWidget* widget) {
                        return widget->isVisible() && editor->rect().contains(
                            QRect(widget->mapTo(editor, QPoint{}), widget->size()));
                    };
                    CHECK(visibleInEditor(section));
                    CHECK(visibleInEditor(tempoShortcut));
                    CHECK(visibleInEditor(loopsShortcut));
                    CHECK(visibleInEditor(fieldsShortcut));
                    const auto capture = [&](const char* name) {
                        const auto directory = qEnvironmentVariable("GRAVITINO_EDITOR_QA_DIR");
                        if (!directory.isEmpty()) {
                            QImage image(editor->size(), QImage::Format_ARGB32_Premultiplied);
                            image.fill(Qt::transparent);
                            editor->render(&image);
                            CHECK(image.save(directory + '/' + QString::fromLatin1(name) + ".png"));
                        }
                    };
                    tempoShortcut->click();
                    app.processEvents();
                    CHECK(section->currentText() == "Initial State");
                    CHECK(visibleInEditor(ratio));
                    ratio->setText("0.9999777201933706");
                    applyTempo->click();
                    CHECK(doc->file().initialTo.tempoRatio == 0.9999777201933706);
                    capture("tempo");
                    doc->undoStack()->undo();

                    doc->mutate("Add loop editor fixture", [](gvt::GvtFile& file) {
                        gvt::TransitionSavedLoop loop;
                        loop.id = "incoming-loop-1";
                        loop.label = "LOOP 1";
                        loop.role = gvt::Role::ToDeck;
                        loop.startTrackBeat = 0.0466821378107263;
                        loop.endTrackBeat = 8.0;
                        file.transitionLoops = {loop};
                        file.transitionCues.clear();
                        file.cues.clear();
                    });
                    loopsShortcut->click();
                    app.processEvents();
                    CHECK(section->currentText() == "Cues / Loops");
                    CHECK(visibleInEditor(end));
                    CHECK(start->text().toDouble() == 0.0466821378107263);
                    end->setText("8.0466821378107263");
                    applyPosition->click();
                    CHECK(doc->file().transitionLoops[0].startTrackBeat == 0.0466821378107263);
                    CHECK(doc->file().transitionLoops[0].endTrackBeat == 8.0466821378107263);
                    CHECK(length->text().contains("8 beats"));
                    capture("loops");
                    doc->undoStack()->undo();
                    doc->undoStack()->undo();

                    fieldsShortcut->click();
                    app.processEvents();
                    CHECK(section->currentText() == "All fields");
                    CHECK(fields->isVisible());
                    CHECK(fields->setField({"metadata", "author"}, "Synthetic GUI author"));
                    CHECK(fields->applyPending());
                    CHECK(doc->file().author == "Synthetic GUI author");
                    doc->undoStack()->undo();
                    CHECK(doc->file().author == portable->author);
                    CHECK(!fields->hasPendingChanges());
                    CHECK(fields->fields().value("metadata").toObject().value("author").toString() == portable->author);
                    // Delete within the field tree must not delete a timeline event.
                    auto* fieldTree = fields->findChild<QTreeWidget*>("transitionFieldsTree");
                    fieldTree->setFocus();
                    const auto eventsBefore = doc->file().events.size();
                    QKeyEvent deletion(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                    QApplication::sendEvent(fieldTree, &deletion);
                    CHECK(doc->file().events.size() == eventsBefore);
                    capture("all-fields");
                    CHECK(!doc->isDirty());
                    editor->resize(1450, 900);
                    inspector->setCurrentIndex(0);
                    app.processEvents();
                }
            }
            auto* sourceBeat = editor->findChild<QLabel*>(
                QStringLiteral("transitionEditorEventSourceBeat"));
            auto* eventBeat = editor->findChild<QDoubleSpinBox*>(
                QStringLiteral("transitionEditorEventBeat"));
            auto* applyEvent = editor->findChild<QPushButton*>(
                QStringLiteral("transitionEditorApplyEvent"));
            auto* deleteEvent = editor->findChild<QPushButton*>(
                QStringLiteral("transitionEditorDeleteEvent"));
            CHECK(deleteEvent != nullptr);
            if (editorTimeline && editorEvents && inspector && sourceBeat) {
                inspector->setCurrentIndex(0);
                const QPoint eventPoint(104 + 2 * 28,
                                        28 + 2 * 92 + 54 / 2);
                sendMouse(editorTimeline, QEvent::MouseButtonPress,
                          eventPoint, editorTimeline->mapToGlobal(eventPoint),
                          Qt::LeftButton, Qt::LeftButton);
                app.processEvents();
                CHECK(inspector->tabText(inspector->currentIndex()) ==
                      QStringLiteral("Events"));
                CHECK(editorEvents->currentRow() == 0);
                CHECK(sourceBeat->text() != QStringLiteral("—"));
                const QPoint movedPoint(104 + 6 * 28, eventPoint.y());
                sendMouse(editorTimeline, QEvent::MouseMove,
                          movedPoint, editorTimeline->mapToGlobal(movedPoint),
                          Qt::NoButton, Qt::LeftButton);
                sendMouse(editorTimeline, QEvent::MouseButtonRelease,
                          movedPoint, editorTimeline->mapToGlobal(movedPoint),
                          Qt::LeftButton, Qt::NoButton);
                auto* editorDocument =
                    editor->findChild<gvt::TransitionEditorDocument*>();
                CHECK(editorDocument != nullptr);
                CHECK(editorEvents->currentRow() == 1);
                if (editorDocument) {
                    editorDocument->undoStack()->undo();
                    editorEvents->setCurrentCell(0, 0);
                    if (eventBeat && applyEvent) {
                        eventBeat->setValue(7.0);
                        applyEvent->click();
                        app.processEvents();
                        CHECK(editorEvents->currentRow() == 1);
                        editorDocument->undoStack()->undo();
                        editorEvents->setCurrentCell(0, 0);
                    }
                }
            }

            // Moving the editor cursor exposes and centers the next
            // executable event. Action-card deletion keeps chronological
            // following; deleting a sole automation point clears its lane.
            auto* editorDocument =
                editor->findChild<gvt::TransitionEditorDocument*>();
            if (editorTimeline && editorEvents && editorDocument &&
                deleteEvent) {
                editorDocument->mutate(
                    QStringLiteral("Add event-follow fixtures"),
                    [](gvt::GvtFile& file) {
                        file.events.push_back(
                            {5.0, gvt::Role::FromDeck,
                             gvt::ControlId::Fader, 0.5,
                             gvt::Curve::Step});
                        file.events.push_back(
                            {6.0, gvt::Role::ToDeck,
                             gvt::ControlId::EqLow, 0.25,
                             gvt::Curve::Step});
                        std::stable_sort(
                            file.events.begin(), file.events.end(),
                            [](const gvt::GvtEvent& left,
                               const gvt::GvtEvent& right) {
                                return left.beat < right.beat;
                            });
                    });
                const QPoint cursorPoint(
                    104 + static_cast<int>(4.5 *
                                            editorTimeline->pixelsPerBeat()),
                    48);
                sendMouse(editorTimeline, QEvent::MouseButtonPress,
                          cursorPoint,
                          editorTimeline->mapToGlobal(cursorPoint),
                          Qt::LeftButton, Qt::LeftButton);
                app.processEvents();
                CHECK(editorEvents->currentRow() == 2);
                CHECK(inspector &&
                      inspector->tabText(inspector->currentIndex()) ==
                          QStringLiteral("Events"));

                editorEvents->setCurrentCell(0, 0);
                deleteEvent->click();
                CHECK(editorEvents->rowCount() == 3);
                CHECK(editorEvents->currentRow() == 1);
                deleteEvent->click();
                CHECK(editorEvents->rowCount() == 2);
                CHECK(editorEvents->currentRow() == -1);
                editorDocument->undoStack()->undo();
                editorDocument->undoStack()->undo();
                editorDocument->undoStack()->undo();
                CHECK(editorEvents->rowCount() == 2);

                editorDocument->mutate(
                    QStringLiteral("Add keyboard-delete fixture"),
                    [](gvt::GvtFile& file) {
                        file.events.push_back(
                            {5.0, gvt::Role::FromDeck,
                             gvt::ControlId::Fader, 0.5,
                             gvt::Curve::Step});
                        std::stable_sort(
                            file.events.begin(), file.events.end(),
                            [](const gvt::GvtEvent& left,
                               const gvt::GvtEvent& right) {
                                return left.beat < right.beat;
                            });
                    });
                editorEvents->setCurrentCell(0, 0);
                editorEvents->setFocus();
                QKeyEvent deletePress(QEvent::KeyPress, Qt::Key_Delete,
                                      Qt::NoModifier);
                QApplication::sendEvent(editorEvents, &deletePress);
                CHECK(editorEvents->rowCount() == 2);
                CHECK(editorEvents->currentRow() == 1);
                editorDocument->undoStack()->undo();
                editorDocument->undoStack()->undo();
                CHECK(editorEvents->rowCount() == 2);

                // Repeated point deletion stays in the same deck/control
                // stream despite interleaved controls, decks, curves and ties.
                for (const auto control : {gvt::ControlId::EqLow, gvt::ControlId::Fader,
                                           gvt::ControlId::Tempo, gvt::ControlId::FxWet}) {
                    editorDocument->mutate("Add independent automation lanes",
                        [control](gvt::GvtFile& file) {
                            file.events = {
                                {1.0, gvt::Role::FromDeck, control, 0.5, gvt::Curve::Step},
                                {2.0, gvt::Role::ToDeck, control, 0.5, gvt::Curve::Step},
                                {4.0, gvt::Role::FromDeck, gvt::ControlId::EqHigh, 0.5, gvt::Curve::Step},
                                {4.0, gvt::Role::FromDeck, control, 0.7, gvt::Curve::Linear},
                                {4.5, gvt::Role::Mixer, gvt::ControlId::Crossfader, 0.5, gvt::Curve::Step},
                                {5.0, gvt::Role::FromDeck, gvt::ControlId::EqHigh, 0.6, gvt::Curve::Linear},
                                {6.0, gvt::Role::ToDeck, control, 0.8, gvt::Curve::Linear},
                                {7.0, gvt::Role::FromDeck, control, 0.9, gvt::Curve::SCurve},
                                {8.0, gvt::Role::FromDeck, gvt::ControlId::EqHigh, 0.7, gvt::Curve::Linear}
                            };
                        });
                    const auto original = gvt::transitionSerialize(editorDocument->file());
                    const auto expectPoint = [&](int row, double beat) {
                        CHECK(editorEvents->currentRow() == row);
                        const auto& events = editorDocument->file().events;
                        CHECK(row >= 0 && row < static_cast<int>(events.size()));
                        if (row >= 0 && row < static_cast<int>(events.size())) {
                            CHECK(events[row].role == gvt::Role::FromDeck);
                            CHECK(events[row].control == control);
                            CHECK(events[row].beat == beat);
                            CHECK(eventBeat && eventBeat->value() == beat);
                        }
                    };
                    editorEvents->setCurrentCell(0, 0);
                    deleteEvent->click();
                    expectPoint(2, 4.0);
                    editorEvents->setFocus();
                    QKeyEvent deletePoint(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                    QApplication::sendEvent(editorEvents, &deletePoint);
                    expectPoint(5, 7.0);
                    QKeyEvent backspacePoint(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
                    QApplication::sendEvent(editorEvents, &backspacePoint);
                    CHECK(editorEvents->rowCount() == 6);
                    CHECK(editorEvents->currentRow() == -1);
                    CHECK(editorEvents->selectedItems().isEmpty());
                    CHECK(!deleteEvent->isEnabled());
                    const auto remaining = gvt::transitionSerialize(editorDocument->file());
                    QApplication::sendEvent(editorEvents, &deletePoint);
                    CHECK(gvt::transitionSerialize(editorDocument->file()) == remaining);
                    editorDocument->undoStack()->undo();
                    editorDocument->undoStack()->undo();
                    editorDocument->undoStack()->undo();
                    CHECK(gvt::transitionSerialize(editorDocument->file()) == original);

                    // Deleting the final point falls back within that lane,
                    // skipping both the other deck and the other knob.
                    editorEvents->setCurrentCell(7, 0);
                    deleteEvent->click();
                    expectPoint(3, 4.0);
                    editorDocument->undoStack()->undo();
                    CHECK(gvt::transitionSerialize(editorDocument->file()) == original);
                    editorDocument->undoStack()->undo();
                    CHECK(editorEvents->rowCount() == 2);
                }
            }
            CHECK(editor->findChild<QTableWidget*>(
                      QStringLiteral("transitionEditorPerformanceDefinitions")) != nullptr);
            CHECK(editor->findChild<QPlainTextEdit*>(
                      QStringLiteral("transitionEditorYaml")) != nullptr);
            auto* editorPlay = editor->findChild<QPushButton*>(
                QStringLiteral("transitionEditorPlay"));
            auto* editorStop = editor->findChild<QPushButton*>(
                QStringLiteral("transitionEditorStop"));
            CHECK(editorPlay != nullptr);
            CHECK(editorStop != nullptr);
            CHECK(editorPlay &&
                  editorPlay->text().contains(QStringLiteral("(C)")));
            if (editorTimeline) {
                auto* timelineScroll = qobject_cast<QScrollArea*>(
                    editorTimeline->parentWidget()->parentWidget());
                CHECK(timelineScroll != nullptr);
                if (timelineScroll) {
                    editorTimeline->setPixelsPerBeat(80.0);
                    timelineScroll->horizontalScrollBar()->setValue(0);
                    QWheelEvent scrollWheel(
                        QPointF(500, 50),
                        QPointF(editorTimeline->mapToGlobal(QPoint(500, 50))),
                        QPoint(), QPoint(0, -120), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(editorTimeline, &scrollWheel);
                    CHECK(timelineScroll->horizontalScrollBar()->value() > 0);

                    const double beforeZoom = editorTimeline->pixelsPerBeat();
                    const double pointerViewportX =
                        500.0 - timelineScroll->horizontalScrollBar()->value();
                    const double beatUnderPointerBefore =
                        (500.0 - 104.0) / beforeZoom;
                    QWheelEvent zoomWheel(
                        QPointF(500, 50),
                        QPointF(editorTimeline->mapToGlobal(QPoint(500, 50))),
                        QPoint(), QPoint(0, 120), Qt::NoButton,
                        Qt::ControlModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(editorTimeline, &zoomWheel);
                    CHECK(editorTimeline->pixelsPerBeat() > beforeZoom);
                    const double beatUnderPointerAfter =
                        (pointerViewportX +
                         timelineScroll->horizontalScrollBar()->value() - 104.0) /
                        editorTimeline->pixelsPerBeat();
                    CHECK(std::fabs(beatUnderPointerAfter -
                                    beatUnderPointerBefore) < 0.02);
                }
            }
            if (editorPlay && editorStop) {
                engine.crossfader.store(0.37f);
                editorTimeline->setPlayheadBeat(1.25);
                editorPlay->click();
                app.processEvents();
                CHECK(engine.exclusivePreviewActive());
                CHECK(std::fabs(engine.crossfader.load() - 0.37) < 1e-6);
                CHECK(editorPlay->text().contains(QStringLiteral("PAUSE")));
                editorTimeline->setPlayheadBeat(3.0);
                editorStop->click();
                app.processEvents();
                CHECK(!engine.exclusivePreviewActive());
                CHECK(std::fabs(editorTimeline->playheadBeat() - 1.25) <
                      1e-9);
                CHECK(editorPlay->text().contains(QStringLiteral("(C)")));

                if (editorTimeline) {
                    // C and Space still work after clicking STOP: focused
                    // buttons are not text-entry fields. C is momentary and
                    // its release returns to the stored cue.
                    editorStop->setFocus();
                    QKeyEvent cuePress(QEvent::KeyPress, Qt::Key_C,
                                       Qt::NoModifier);
                    QApplication::sendEvent(editorStop, &cuePress);
                    app.processEvents();
                    CHECK(engine.exclusivePreviewActive());
                    editorTimeline->setPlayheadBeat(3.0);
                    QKeyEvent cueRelease(QEvent::KeyRelease, Qt::Key_C,
                                         Qt::NoModifier);
                    QApplication::sendEvent(editorStop, &cueRelease);
                    app.processEvents();
                    CHECK(!engine.exclusivePreviewActive());
                    CHECK(std::fabs(editorTimeline->playheadBeat() - 1.25) <
                          1e-9);

                    QKeyEvent buttonSpace(QEvent::KeyPress, Qt::Key_Space,
                                          Qt::NoModifier);
                    QApplication::sendEvent(editorStop, &buttonSpace);
                    app.processEvents();
                    CHECK(engine.exclusivePreviewActive());
                    editorStop->click();
                    app.processEvents();
                    CHECK(!engine.exclusivePreviewActive());

                    editorTimeline->setFocus();
                    QKeyEvent latchCuePress(QEvent::KeyPress, Qt::Key_C,
                                            Qt::NoModifier);
                    QApplication::sendEvent(editor, &latchCuePress);
                    QKeyEvent spacePress(QEvent::KeyPress, Qt::Key_Space,
                                         Qt::NoModifier);
                    QApplication::sendEvent(editor, &spacePress);
                    QApplication::sendEvent(editor, &cueRelease);
                    app.processEvents();
                    CHECK(engine.exclusivePreviewActive());
                    QApplication::sendEvent(editor, &spacePress);
                    app.processEvents();
                    CHECK(!engine.exclusivePreviewActive());
                }
            }
            editor->close();
        }

        engine.deck(0).loadTrack(nullptr);
        CHECK(deckPanel && QMetaObject::invokeMethod(
              deckPanel, "refresh", Qt::DirectConnection));
        CHECK(beatCounter && beatCounter->text() == QStringLiteral("BEAT —"));
    }

    {
        gvt::MasterRecorder masterRecorder;
        gvt::MainWindow window(&bus, &engine, &library, &store,
                               &transitionRecorder, &transitionPlayer, &midi,
                               &masterRecorder);
        auto* master = window.findChild<QPushButton*>(
            QStringLiteral("masterRecordButton"));
        auto* libraryToggle = window.findChild<QPushButton*>(
            QStringLiteral("libraryVisibilityToggle"));
        CHECK(master != nullptr);
        CHECK(libraryToggle != nullptr);
        CHECK(master && window.statusBar()->isAncestorOf(master));
        CHECK(libraryToggle && window.statusBar()->isAncestorOf(libraryToggle));
        window.resize(1200, 720);
        window.show();
        app.processEvents();
        const int masterRight = master
                                    ? master->mapTo(window.statusBar(),
                                                    master->rect().topRight())
                                          .x()
                                    : 0;
        const int libraryLeft = libraryToggle
                                    ? libraryToggle
                                          ->mapTo(window.statusBar(),
                                                  libraryToggle->rect().topLeft())
                                          .x()
                                    : 0;
        CHECK(master && libraryToggle && masterRight <= libraryLeft);
    }

    if (failures) return 1;
    std::puts("test_ui_layout: layout, sequence, and mouse deck controls passed");
    return 0;
}
