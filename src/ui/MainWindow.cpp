#include "MainWindow.h"

#include "DeckWidget.h"
#include "DetailWaveformView.h"
#include "FitButton.h"
#include "LibraryWidget.h"
#include "MixerWidget.h"
#include "SoftTakeoverOverlay.h"
#include "Theme.h"
#include "TransitionPanel.h"
#include "../analysis/BeatGridEditor.h"
#include "../analysis/StemSeparator.h"
#include "../audio/MasterRecorder.h"
#include "../library/History.h"

#include <QDesktopServices>
#include <QActionGroup>
#include <QSplitter>
#include <QTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

namespace gvt {

QString appStyleSheet()
{
    return QStringLiteral(R"(
* { font-size: 11px; }
QMainWindow, QDialog { background: #16181d; }
QWidget { color: #d8dce4; background: #16181d; }
QWidget[panel="true"] { background: #2a2e37; border-radius: 6px; }
QLabel { background: transparent; }
QPushButton {
    background: #383d48; border: 1px solid #4a505c; border-radius: 4px;
    padding: 2px 8px; color: #d8dce4;
}
QPushButton:hover { background: #434956; }
QPushButton:pressed { background: #2a2e37; }
QPushButton:disabled { color: #6a707d; background: #2e323b; }
QLineEdit {
    background: #16181d; border: 1px solid #4a505c; border-radius: 4px;
    padding: 4px 6px; selection-background-color: #35c8e8;
    selection-color: black;
}
QTableView, QListWidget {
    background: #1c1f25; alternate-background-color: #21242b;
    border: 1px solid #383d48; gridline-color: #2a2e37;
    selection-background-color: #35c8e8; selection-color: black;
}
QHeaderView::section {
    background: #2a2e37; color: #8a909c; border: none;
    border-right: 1px solid #383d48; padding: 2px 5px;
}
QSlider:vertical { min-height: 84px; min-width: 22px; }
QSlider:horizontal { min-width: 120px; min-height: 22px; }
QSlider::groove:vertical { background: #383d48; width: 6px; border-radius: 3px; }
QSlider::groove:horizontal { background: #383d48; height: 6px; border-radius: 3px; }
QSlider::handle:vertical {
    background: #b8bec9; height: 14px; margin: 0 -6px; border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #b8bec9; width: 14px; margin: -6px 0; border-radius: 3px;
}
QSlider::handle:hover { background: #ffffff; }
QDial { background: #383d48; }
QProgressBar {
    background: #16181d; border: 1px solid #383d48; border-radius: 4px;
}
QProgressBar::chunk { background: #35c8e8; border-radius: 3px; }
QMenuBar { background: #16181d; }
QMenuBar::item:selected { background: #2a2e37; }
QMenu { background: #2a2e37; border: 1px solid #4a505c; }
QMenu::item:selected { background: #35c8e8; color: black; }
QStatusBar { background: #1c1f25; color: #8a909c; }
QToolTip { background: #2a2e37; color: #d8dce4; border: 1px solid #4a505c; }
QScrollBar { background: #16181d; }
QSplitter::handle { background: #383d48; }
QSplitter::handle:vertical { height: 5px; }
QSplitter::handle:horizontal { width: 5px; }
)");
}

MainWindow::MainWindow(ControlBus* bus, AudioEngine* engine,
                       TrackLibrary* library, TransitionStore* store,
                       TransitionRecorder* recorder, TransitionPlayer* player,
                       MidiEngine* midi, MasterRecorder* rec,
                       StemSeparator* stems, QWidget* parent)
    : QMainWindow(parent), bus_(bus), engine_(engine), library_(library),
      store_(store), midi_(midi), rec_(rec), stems_(stems)
{
    setWindowTitle(tr("Gravitino DJ"));
    setMinimumSize(1200, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // Row 1: Deck A | Deck B — exactly equal halves (Serato-style, no
    // center mixer column; tempo sliders sit on the outer edges).
    deckA_ = new DeckWidget(0, bus_, engine_);
    deckB_ = new DeckWidget(1, bus_, engine_);
    auto* deckRow = new QHBoxLayout;
    deckRow->setSpacing(6);
    deckRow->addWidget(deckA_, 1);
    deckRow->addWidget(deckB_, 1);
    root->addLayout(deckRow);

    // Row 2: full-width scrolling detail waveforms (A lane over B lane,
    // fixed center playhead).
    detailWave_ = new DetailWaveformView(engine_);
    root->addWidget(detailWave_);

    // Lower workspace: the transition/event area and library share a vertical
    // splitter, so the library can be pulled down when more event rows matter.
    mixer_ = new MixerWidget(bus_);
    transitionPanel_ =
        new TransitionPanel(bus_, engine_, store_, recorder, player);
    auto* workspaceSplitter = new QSplitter(Qt::Horizontal);
    workspaceSplitter->setObjectName(
        QStringLiteral("mixerTransitionSplitter"));
    workspaceSplitter->setChildrenCollapsible(false);
    workspaceSplitter->addWidget(mixer_);
    workspaceSplitter->addWidget(transitionPanel_);
    workspaceSplitter->setStretchFactor(0, 0);
    workspaceSplitter->setStretchFactor(1, 1);
    workspaceSplitter->setSizes({470, 900});
    const QByteArray workspaceState = QSettings().value(
        QStringLiteral("layout/mixerTransitionSplitter")).toByteArray();
    if (!workspaceState.isEmpty())
        workspaceSplitter->restoreState(workspaceState);
    connect(workspaceSplitter, &QSplitter::splitterMoved, this,
            [workspaceSplitter] {
                QSettings().setValue(
                    QStringLiteral("layout/mixerTransitionSplitter"),
                    workspaceSplitter->saveState());
            });

    // The load history is persisted at ~/.gravitino/history.jsonl and shown
    // in the library's History tab.
    history_ = new History(this);
    libraryWidget_ = new LibraryWidget(library_, engine_, store_, history_);
    auto* lowerSplitter = new QSplitter(Qt::Vertical);
    lowerSplitter->setObjectName(QStringLiteral("transitionLibrarySplitter"));
    lowerSplitter->setChildrenCollapsible(false);
    lowerSplitter->addWidget(workspaceSplitter);
    lowerSplitter->addWidget(libraryWidget_);
    lowerSplitter->setStretchFactor(0, 1);
    lowerSplitter->setStretchFactor(1, 2);
    lowerSplitter->setSizes({170, 280});
    const QByteArray lowerState = QSettings().value(
        QStringLiteral("layout/transitionLibrarySplitter")).toByteArray();
    if (!lowerState.isEmpty())
        lowerSplitter->restoreState(lowerState);
    connect(lowerSplitter, &QSplitter::splitterMoved, this,
            [lowerSplitter] {
                QSettings().setValue(
                    QStringLiteral("layout/transitionLibrarySplitter"),
                    lowerSplitter->saveState());
            });
    root->addWidget(lowerSplitter, 2);

    setCentralWidget(central);

    // Menus.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("Open Music Folder…"), this,
                        &MainWindow::openMusicFolder);
    QMenu* transMenu = menuBar()->addMenu(tr("&Transitions"));
    transMenu->addAction(tr("Open Transitions Folder"), this,
                         &MainWindow::openTransitionsFolder);
    QMenu* settingsMenu = menuBar()->addMenu(tr("&Settings"));
    audioOutputMenu_ = settingsMenu->addMenu(tr("Audio Output"));
    audioOutputMenu_->setToolTipsVisible(true);
    connect(audioOutputMenu_, &QMenu::aboutToShow, this,
            &MainWindow::rebuildAudioOutputMenu);
    rebuildAudioOutputMenu();
    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("About Gravitino"), this, &MainWindow::about);

    // Status-bar LEFT: master-record toggle (hidden when no recorder was
    // provided — the button is simply never created).
    if (rec_) {
        recBtn_ = new FitPushButton(tr("● REC MASTER"));
        recBtn_->setToolTip(
            tr("Record the master output to a WAV file"));
        recBtn_->setCursor(Qt::PointingHandCursor);
        statusBar()->addWidget(recBtn_); // left side
        connect(recBtn_, &QPushButton::clicked, this,
                &MainWindow::onRecClicked);
        connect(rec_, &MasterRecorder::recordingChanged, this,
                &MainWindow::onRecordingChanged);
        recTimer_ = new QTimer(this);
        recTimer_->setInterval(1000);
        connect(recTimer_, &QTimer::timeout, this, [this] {
            const int s = (int)rec_->recordedSec();
            recBtn_->setText(
                QString::asprintf("● %02d:%02d", s / 60, s % 60));
        });
        onRecordingChanged(rec_->isRecording(), rec_->currentPath());
    }

    // Status bar: MIDI indicator, sample rate, transient messages.
    pickupLabel_ = new QLabel;
    pickupLabel_->setAlignment(Qt::AlignCenter);
    pickupLabel_->setVisible(false);
    pickupLabel_->setMinimumHeight(24);
    statusBar()->addWidget(pickupLabel_, 1);
    pickupTimer_ = new QTimer(this);
    pickupTimer_->setInterval(360);
    connect(pickupTimer_, &QTimer::timeout, this, [this] {
        pickupPulse_ = !pickupPulse_;
        pickupLabel_->setStyleSheet(
            pickupPulse_
                ? QStringLiteral("background:white; color:#111318; "
                                 "font-weight:bold; padding:3px 9px;")
                : QStringLiteral("background:#343943; color:white; "
                                 "border:1px dashed white; font-weight:bold; "
                                 "padding:3px 9px;"));
        for (PickupFuzzOverlay* overlay : pickupOverlays_)
            if (overlay) overlay->setPulse(pickupPulse_);
    });
    midiLabel_ = new QLabel;
    rateLabel_ = new QLabel;
    updateAudioOutputLabel();
    statusBar()->addPermanentWidget(midiLabel_);
    statusBar()->addPermanentWidget(rateLabel_);
    onMidiConnection(midi_->controllerConnected(), midi_->controllerName());
    connect(midi_, &MidiEngine::connectionChanged, this,
            &MainWindow::onMidiConnection);
    connect(midi_, &MidiEngine::softTakeoverChanged, this,
            &MainWindow::refreshSoftTakeoverUi);
    connect(transitionPanel_,
            &TransitionPanel::hardwareTakeoverTrackingStarted, midi_,
            &MidiEngine::beginTransitionTakeoverTracking);
    connect(transitionPanel_,
            &TransitionPanel::hardwareTakeoverTrackingFinished, midi_,
            &MidiEngine::finishTransitionTakeoverTracking);
    connect(midi_, &MidiEngine::hardwareControlObserved, transitionPanel_,
            &TransitionPanel::observeTutorialHardwareControl);
    connect(midi_, &MidiEngine::connectionChanged, this,
            [this](bool connected, const QString&) {
                if (!connected || engine_->headphoneOutputAvailable()) return;
                QString error;
                engine_->switchOutputDevice(
                    engine_->outputDevicePreference(), &error);
                updateAudioOutputLabel();
            });
    connect(engine_, &AudioEngine::outputDeviceChanged, this,
            [this](const QString&, bool) { updateAudioOutputLabel(); });

    // Cross-widget wiring.
    connect(libraryWidget_, &LibraryWidget::trackLoaded, this,
            [this](int deck) { notifyTrackLoaded(deck); });
    connect(libraryWidget_, &LibraryWidget::statusMessage, this,
            [this](const QString& msg, int timeoutMs) {
                statusBar()->showMessage(msg, timeoutMs);
            });
    connect(midi_, &MidiEngine::browseMoved, libraryWidget_,
            &LibraryWidget::browseBy);
    connect(midi_, &MidiEngine::browsePressed, libraryWidget_,
            &LibraryWidget::confirmBrowseSelection);
    connect(midi_, &MidiEngine::loadRequested, libraryWidget_,
            &LibraryWidget::loadSelectedTo);
    connect(midi_, &MidiEngine::performancePadModeRequested, this,
            [this](int deck, int mode) {
                if ((deck != 0 && deck != 1) || mode < 0 ||
                    mode >= static_cast<int>(PerformancePadMode::Count))
                    return;
                deckWidget(deck)->setPerformancePadMode(
                    static_cast<PerformancePadMode>(mode));
            });
    connect(midi_, &MidiEngine::performancePadRequested, this,
            [this](int deck, int mode, int pad, bool pressed) {
                if ((deck != 0 && deck != 1) || mode < 0 ||
                    mode >= static_cast<int>(PerformancePadMode::Count))
                    return;
                deckWidget(deck)->triggerPerformancePad(
                    static_cast<PerformancePadMode>(mode), pad, pressed);
            });
    connect(midi_, &MidiEngine::hotCueClearRequested, this,
            [this](int deck, int pad) {
                if (deck == 0 || deck == 1)
                    deckWidget(deck)->clearHotCue(pad);
            });
    auto wirePadLeds = [this](DeckWidget* deck) {
        connect(deck, &DeckWidget::performancePadStateChanged, midi_,
                &MidiEngine::setPerformancePadState);
        const PerformancePadMode mode = deck->performancePadMode();
        midi_->setPerformancePadState(
            deck == deckA_ ? 0 : 1, static_cast<int>(mode),
            deck->performancePadLedMask(mode),
            deck->performancePadPressedMask());
    };
    wirePadLeds(deckA_);
    wirePadLeds(deckB_);

    auto persistPerformanceMetadata = [this](int deck) {
        if (deck != 0 && deck != 1) return;
        const TrackDataPtr changed = engine_->deck(deck).track();
        if (!changed) return;
        QString error;
        const bool persisted =
            library_->persistPerformanceMetadata(*changed, &error);

        // Both decks can share the same analyzed TrackData only by value after
        // independent loads. Mirror authored metadata immediately so their
        // pads and waveform flags cannot disagree during the current session.
        for (int other = 0; other < kNumDecks; ++other) {
            const TrackDataPtr loaded = engine_->deck(other).track();
            if (!loaded || loaded->filePath != changed->filePath) continue;
            for (int pad = 0; pad < 8; ++pad) {
                loaded->hotCues[pad] = changed->hotCues[pad];
                loaded->savedLoops[pad] = changed->savedLoops[pad];
            }
            deckWidget(other)->performanceMetadataChanged();
        }
        detailWave_->update();
        statusBar()->showMessage(
            persisted ? tr("Saved track cues and loops")
                      : tr("Cues changed for this session, but could not be saved: %1")
                            .arg(error),
            persisted ? 2500 : 6500);
    };
    connect(deckA_, &DeckWidget::trackPerformanceMetadataChanged,
            this, persistPerformanceMetadata);
    connect(deckB_, &DeckWidget::trackPerformanceMetadataChanged,
            this, persistPerformanceMetadata);

    auto applyBeatGridEdit = [this](int deck, BeatGridCommand command,
                                     double value) {
        if (deck != 0 && deck != 1)
            return;
        TrackDataPtr track = engine_->deck(deck).track();
        if (!track) {
            statusBar()->showMessage(
                tr("Load a track before editing its beat grid"), 3500);
            return;
        }

        BeatGridEditor editor(
            track->bpm, track->firstBeatSec,
            engine_->deck(deck).positionSec());
        bool changed = false;
        switch (command) {
        case BeatGridCommand::SetDownbeat:
            changed = editor.setDownbeatAt(value);
            break;
        case BeatGridCommand::Nudge:
            changed = editor.nudgeSeconds(value);
            break;
        case BeatGridCommand::HalveBpm:
            changed = editor.halveBpm();
            break;
        case BeatGridCommand::DoubleBpm:
            changed = editor.doubleBpm();
            break;
        case BeatGridCommand::SetBpm:
            changed = editor.setBpm(value);
            break;
        }
        if (!changed || !editor.hasValidGrid()) {
            statusBar()->showMessage(
                tr("Beat-grid edit is outside the supported 20–400 BPM range"),
                4500);
            return;
        }

        track->bpm = editor.bpm();
        track->firstBeatSec = editor.firstBeatSec();
        QString error;
        const bool persisted = library_->persistBeatGrid(*track, &error);

        for (int other = 0; other < kNumDecks; ++other) {
            const TrackDataPtr loaded = engine_->deck(other).track();
            if (loaded && loaded->filePath == track->filePath) {
                engine_->deck(other).updateBeatGrid(
                    track->bpm, track->firstBeatSec);
                deckWidget(other)->beatGridChanged();
            }
        }
        detailWave_->update();
        transitionPanel_->refreshMatches();
        statusBar()->showMessage(
            persisted
                ? tr("Saved beat grid: %1 BPM · anchor %2 s")
                      .arg(track->bpm, 0, 'f', 3)
                      .arg(track->firstBeatSec, 0, 'f', 3)
                : tr("Beat grid changed for this session, but could not be saved: %1")
                      .arg(error),
            persisted ? 4500 : 7000);
    };
    connect(deckA_, &DeckWidget::beatGridEditRequested, this,
            applyBeatGridEdit);
    connect(deckB_, &DeckWidget::beatGridEditRequested, this,
            applyBeatGridEdit);
    connect(bus_, &ControlBus::eventDispatched, this,
            [this](const ControlEvent& event, Origin origin) {
                if (origin != Origin::Midi ||
                    (event.id != ControlId::HeadphoneCue &&
                     event.id != ControlId::MasterCue) ||
                    event.value <= 0.5 ||
                    engine_->headphoneOutputAvailable())
                    return;
                statusBar()->showMessage(
                    tr("Headphone cue needs DDJ-FLX4 audio: connect the "
                       "controller, then reopen Settings > Audio Output"),
                    7000);
            });
    connect(transitionPanel_, &TransitionPanel::entryMarkerChanged, this,
            &MainWindow::setTransitionEntryMarker);
    connect(transitionPanel_, &TransitionPanel::cueMarkersChanged, this,
            &MainWindow::setTransitionCueMarkers);
    connect(transitionPanel_, &TransitionPanel::statusMessage, this,
            [this](const QString& msg, int timeoutMs) {
                statusBar()->showMessage(msg, timeoutMs);
            });

    // Stem separation wiring. Every StemSeparator signal carries the track
    // fingerprint; it is matched against each deck's CURRENT track, so
    // results for a track that was swapped out mid-separation are dropped
    // for that deck (the cache keeps them for the next load).
    connect(deckA_, &DeckWidget::stemsRequested, this,
            &MainWindow::onStemsRequested);
    connect(deckB_, &DeckWidget::stemsRequested, this,
            &MainWindow::onStemsRequested);
    if (stems_) {
        auto forEachMatchingDeck = [this](const QString& fingerprint,
                                          auto&& fn) {
            for (int i = 0; i < kNumDecks; ++i) {
                TrackDataPtr t = engine_->deck(i).track();
                if (t && t->fingerprint == fingerprint) fn(i);
            }
        };
        connect(stems_, &StemSeparator::progress, this,
                [this, forEachMatchingDeck](const QString& fp,
                                            const QString& stage) {
                    forEachMatchingDeck(fp, [this, &stage](int i) {
                        deckWidget(i)->setStemsInProgress(stage);
                    });
                });
        connect(stems_, &StemSeparator::stemsReady, this,
                [this, forEachMatchingDeck](const QString& fp,
                                            StemSetPtr stems) {
                    forEachMatchingDeck(fp, [this, &stems](int i) {
                        engine_->deck(i).attachStems(stems);
                        deckWidget(i)->setStemsReady();
                    });
                });
        connect(stems_, &StemSeparator::stemsFailed, this,
                [this, forEachMatchingDeck](const QString& fp,
                                            const QString& error) {
                    forEachMatchingDeck(fp, [this](int i) {
                        deckWidget(i)->setStemsIdle();
                    });
                    statusBar()->showMessage(
                        tr("Stem separation failed: %1").arg(error), 8000);
                });
    }
}

void MainWindow::refreshSoftTakeoverUi()
{
    for (PickupFuzzOverlay* overlay : pickupOverlays_)
        if (overlay) overlay->deleteLater();
    pickupOverlays_.clear();

    const std::vector<SoftTakeoverState> pending = midi_->pendingTakeovers();
    if (pending.empty()) {
        pickupTimer_->stop();
        pickupPulse_ = false;
        pickupLabel_->hide();
        return;
    }

    const auto controlText = [this](ControlId control) {
        switch (control) {
        case ControlId::Tempo:       return tr("TEMPO");
        case ControlId::Fader:       return tr("FADER");
        case ControlId::Trim:        return tr("TRIM");
        case ControlId::EqHigh:      return tr("HIGH");
        case ControlId::EqMid:       return tr("MID");
        case ControlId::EqLow:       return tr("LOW");
        case ControlId::Filter:      return tr("FILTER");
        case ControlId::Crossfader:  return tr("CROSSFADER");
        default: return QString::fromUtf8(controlName(control)).toUpper();
        }
    };
    const auto valueText = [this](ControlId control, double value) {
        if (control == ControlId::Tempo)
            return tr("%1%")
                .arg((value - 1.0) * 100.0, 0, 'f', 1);
        return tr("%1%").arg(value * 100.0, 0, 'f', 0);
    };

    QStringList instructions;
    for (const SoftTakeoverState& state : pending) {
        QWidget* target = state.control == ControlId::Tempo &&
                                  (state.deck == 0 || state.deck == 1)
                              ? deckWidget(state.deck)->controlWidget(state.control)
                              : mixer_->controlWidget(state.deck, state.control);
        if (target) {
            auto* overlay = new PickupFuzzOverlay(target);
            overlay->setPulse(pickupPulse_);
            pickupOverlays_.append(overlay);
        }

        const QString scope = state.deck == 0 ? tr("A")
                              : state.deck == 1 ? tr("B") : tr("MIXER");
        instructions.append(
            tr("%1 %2 %3 → %4")
                .arg(scope, controlText(state.control),
                     state.hardwareKnown
                         ? valueText(state.control, state.hardwareValue)
                         : tr("move it"),
                     valueText(state.control, state.targetValue)));
    }
    QString detail = instructions.mid(0, 4).join(QStringLiteral(" · "));
    if (instructions.size() > 4)
        detail += tr(" · +%1 more").arg(instructions.size() - 4);
    pickupLabel_->setText(
        tr("⚠ FLX4 INPUT FROZEN — MATCH CONTROLS: %1").arg(detail));
    pickupLabel_->show();
    pickupPulse_ = false;
    pickupTimer_->start();
}

void MainWindow::updateAudioOutputLabel()
{
    if (!rateLabel_) return;
    const QString outputName = engine_->outputDeviceName();
    const bool phones = engine_->headphoneOutputAvailable();
    const bool masterOnFlx4 = outputName.contains(
        QStringLiteral("DDJ-FLX4"), Qt::CaseInsensitive);
    rateLabel_->setText(
        outputName.isEmpty()
            ? tr("%1 Hz · NO AUDIO OUTPUT").arg(kSampleRate)
            : tr("%1 Hz · %2%3")
                  .arg(kSampleRate)
                  .arg(outputName,
                       phones ? tr(" · FLX4 PHONES") : QString()));
    rateLabel_->setToolTip(
        phones
            ? (masterOnFlx4
                   ? tr("FLX4 master and headphone outputs are active")
                   : tr("Master uses %1; headphone cue uses the FLX4")
                         .arg(outputName))
            : tr("Stereo output is active. Bluetooth is supported but may "
                 "add latency; connect the FLX4 to add headphone cue."));
}

void MainWindow::rebuildAudioOutputMenu()
{
    if (!audioOutputMenu_) return;
    audioOutputMenu_->clear();
    delete audioOutputGroup_;
    audioOutputGroup_ = new QActionGroup(audioOutputMenu_);
    audioOutputGroup_->setExclusive(true);

    const QString selected = engine_->outputDevicePreference();
    auto addChoice = [this, &selected](const QString& label,
                                       const QString& preference,
                                       const QString& toolTip = QString()) {
        QAction* action = audioOutputMenu_->addAction(label);
        action->setCheckable(true);
        action->setChecked(selected == preference);
        action->setData(preference);
        action->setToolTip(toolTip);
        audioOutputGroup_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, preference] { selectAudioOutput(preference); });
    };

    addChoice(tr("System Default"), QString(),
              tr("Follow the current macOS sound output (stereo)"));
    audioOutputMenu_->addSeparator();

    QString error;
    const QList<AudioOutputDevice> outputs =
        engine_->availableOutputDevices(&error);
    for (const AudioOutputDevice& output : outputs) {
        const QString label = output.isDefault
                                  ? tr("%1 (macOS default)").arg(output.name)
                                  : output.name;
        addChoice(label, output.name,
                  output.name.contains(QStringLiteral("Bluetooth"),
                                       Qt::CaseInsensitive)
                      ? tr("Bluetooth output may have noticeable latency")
                      : QString());
    }
    if (!error.isEmpty()) {
        QAction* unavailable = audioOutputMenu_->addAction(error);
        unavailable->setEnabled(false);
    }
}

void MainWindow::selectAudioOutput(const QString& preferredName)
{
    QString error;
    if (!engine_->switchOutputDevice(preferredName, &error)) {
        QMessageBox::warning(this, tr("Could not switch audio output"), error);
        rebuildAudioOutputMenu();
        updateAudioOutputLabel();
        return;
    }

    QSettings().setValue(QStringLiteral("audio/outputDevice"), preferredName);
    updateAudioOutputLabel();
    rebuildAudioOutputMenu();
    const QString detail = engine_->headphoneOutputAvailable()
                               ? tr("master output + FLX4 headphones")
                               : tr("stereo; connect FLX4 for headphone cue");
    statusBar()->showMessage(
        tr("Audio output: %1 (%2)")
            .arg(engine_->outputDeviceName(), detail),
        6000);
}

void MainWindow::onStemsRequested(int deck)
{
    if (!stems_ || (deck != 0 && deck != 1)) return;
    TrackDataPtr t = engine_->deck(deck).track();
    if (!t) return;
    deckWidget(deck)->setStemsInProgress(
        stems_->hasCached(*t) ? tr("loading cached stems…")
                              : tr("queued for separation…"));
    stems_->requestStems(t);
}

void MainWindow::openMusicFolder()
{
    QString start =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Music Folder"), start);
    if (dir.isEmpty()) return;
    library_->scanFolder(dir);
    statusBar()->showMessage(tr("Scanning %1…").arg(dir), 4000);
}

void MainWindow::openTransitionsFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(store_->directory()));
}

void MainWindow::about()
{
    QMessageBox::about(
        this, tr("About Gravitino"),
        tr("<b>Gravitino DJ</b><br>Record a mixing transition once, "
           "replay or teach it beat-perfectly.<br><br>"
           "Open source, Qt %1.")
            .arg(QLatin1String(qVersion())));
}

void MainWindow::onMidiConnection(bool connected, const QString& name)
{
    if (connected) {
        midiLabel_->setText(
            tr("%1 connected").arg(name.isEmpty() ? tr("controller") : name));
        midiLabel_->setStyleSheet("color:#4cd964; font-weight:bold;");
    } else {
        midiLabel_->setText(tr("no controller — plug in any time"));
        midiLabel_->setStyleSheet(
            QStringLiteral("color:%1;").arg(themeDimText().name()));
    }
}

void MainWindow::onRecClicked()
{
    if (!rec_) return;
    if (rec_->isRecording()) {
        rec_->stop();
        return;
    }
    QString err;
    if (!rec_->start(QString(), &err))
        statusBar()->showMessage(
            tr("Master recording failed: %1").arg(err), 6000);
    // Button state follows recordingChanged, not the click.
}

void MainWindow::onRecordingChanged(bool active, const QString& path)
{
    if (!recBtn_) return;
    if (active) {
        recBtn_->setText(QStringLiteral("● 00:00"));
        recBtn_->setStyleSheet(
            "QPushButton { background:#c8322e; color:white; "
            "font-weight:bold; }");
        recBtn_->setToolTip(tr("Recording master → %1 — click to stop")
                                .arg(path));
        recTimer_->start();
        statusBar()->showMessage(tr("Recording master → %1").arg(path),
                                 4000);
    } else {
        recTimer_->stop();
        recBtn_->setText(tr("● REC MASTER"));
        recBtn_->setStyleSheet(QString());
        recBtn_->setToolTip(
            tr("Record the master output to a WAV file"));
        if (!path.isEmpty())
            statusBar()->showMessage(tr("Saved recording: %1").arg(path),
                                     6000);
    }
}

void MainWindow::notifyTrackLoaded(int deck)
{
    // Log to the session history (covers both library loads and the
    // --autoload dev hook, which land here alike).
    if (history_ && (deck == 0 || deck == 1)) {
        if (TrackDataPtr t = engine_->deck(deck).track())
            history_->logLoad(deck, *t);
    }
    (deck == 0 ? deckA_ : deckB_)->trackChanged();
    // Cached stems separate for free (decode-only) — auto-request them.
    if (stems_ && (deck == 0 || deck == 1)) {
        if (TrackDataPtr t = engine_->deck(deck).track())
            if (stems_->hasCached(*t)) onStemsRequested(deck);
    }
    detailWave_->update();
    transitionPanel_->refreshMatches();
}

void MainWindow::setTransitionEntryMarker(int deck, double sec)
{
    if (deck != 0 && deck != 1) return;
    (deck == 0 ? deckA_ : deckB_)->setTransitionEntry(sec);
    detailWave_->setTransitionEntry(deck, sec);
}

void MainWindow::setTransitionCueMarkers(int deck,
                                         const QList<double>& seconds,
                                         const QStringList& labels)
{
    if (deck != 0 && deck != 1) return;
    (deck == 0 ? deckA_ : deckB_)->setTransitionCues(seconds, labels);
    detailWave_->setTransitionCues(deck, seconds, labels);
}

} // namespace gvt
