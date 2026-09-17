#include "MainWindow.h"

#include "DeckWidget.h"
#include "DetailWaveformView.h"
#include "FitButton.h"
#include "LibraryWidget.h"
#include "MixerWidget.h"
#include "SoftTakeoverOverlay.h"
#include "Theme.h"
#include "TransitionPanel.h"
#include "TransitionEditor.h"
#include "SetRenderWindow.h"
#include "../analysis/BeatGridEditor.h"
#include "../analysis/StemSeparator.h"
#include "../audio/MasterRecorder.h"
#include "../library/History.h"

#include <QDesktopServices>
#include <QActionGroup>
#include <QCheckBox>
#include <QSplitter>
#include <QTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace gvt {


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

    // Row 1: Deck A | compact FLX4 mixer | Deck B.  Keeping the mixer beside
    // the deck controls (and below their overview-waveform baseline) leaves the
    // transition/event workspace the full application width.
    deckA_ = new DeckWidget(0, bus_, engine_);
    deckB_ = new DeckWidget(1, bus_, engine_);
    mixer_ = new MixerWidget(bus_);
    auto* hardwareHost = new QWidget(mixer_);
    auto* hardwareLayout = new QVBoxLayout(hardwareHost);
    hardwareLayout->setContentsMargins(2, 1, 2, 1);
    hardwareLayout->setSpacing(1);
    hardwareSyncLabel_ = new QLabel(tr("HW UNKNOWN"), hardwareHost);
    hardwareSyncLabel_->setObjectName(QStringLiteral("hardwareSyncStatus"));
    hardwareSyncLabel_->setAlignment(Qt::AlignCenter);
    hardwareSyncLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:9px; font-weight:bold;")
            .arg(themeDimText().name()));
    hardwareLayout->addWidget(hardwareSyncLabel_);
    auto* hardwareButtons = new QHBoxLayout;
    hardwareButtons->setContentsMargins(0, 0, 0, 0);
    hardwareButtons->setSpacing(3);
    showHardwareCheck_ = new QCheckBox(tr("SHOW HW"), hardwareHost);
    showHardwareCheck_->setObjectName(QStringLiteral("showHardwareState"));
    showHardwareCheck_->setFixedHeight(18);
    showHardwareCheck_->setToolTip(
        tr("Overlay the last physical positions reported by the controller"));
    freezeHardwareCheck_ = new QCheckBox(tr("FREEZE HW"), hardwareHost);
    freezeHardwareCheck_->setObjectName(QStringLiteral("freezeHardwareInput"));
    freezeHardwareCheck_->setFixedHeight(18);
    freezeHardwareCheck_->setToolTip(
        tr("Keep knobs and absolute faders from changing software; buttons remain live"));
    hardwareButtons->addWidget(showHardwareCheck_);
    hardwareButtons->addWidget(freezeHardwareCheck_);
    hardwareLayout->addLayout(hardwareButtons);
    getHardwareStateBtn_ = new FitPushButton(
        tr("GET HW STATE"), hardwareHost);
    getHardwareStateBtn_->setObjectName(
        QStringLiteral("getHardwareState"));
    getHardwareStateBtn_->setFixedHeight(18);
    getHardwareStateBtn_->setToolTip(
        tr("Refresh the controller connection and compare the latest reported "
           "physical positions. Move an unknown knob or fader once to capture it."));
    hardwareLayout->addWidget(getHardwareStateBtn_);
    mixer_->setTopWidget(hardwareHost);
    auto* deckRow = new QHBoxLayout;
    deckRow->setSpacing(6);
    deckRow->addWidget(deckA_, 1);
    deckRow->addWidget(mixer_, 0, Qt::AlignTop);
    deckRow->addWidget(deckB_, 1);
    root->addLayout(deckRow);

    // Row 2: full-width scrolling detail waveforms (A lane over B lane,
    // fixed center playhead).
    detailWave_ = new DetailWaveformView(engine_);
    root->addWidget(detailWave_);

    // Lower workspace: Tutor opens only here, to the left of the three panels
    // it is allowed to narrow (transition controls, event sequence, library).
    // Nothing above the detailed waveform participates in this splitter.
    transitionPanel_ =
        new TransitionPanel(bus_, engine_, store_, recorder, player);

    // The load history is persisted at ~/.gravitino/history.jsonl and shown
    // in the library's History tab.
    history_ = new History(this);
    libraryWidget_ = new LibraryWidget(library_, engine_, store_, history_);
    lowerWorkspaceSplitter_ = new QSplitter(Qt::Horizontal);
    lowerWorkspaceSplitter_->setObjectName(
        QStringLiteral("tutorialWorkspaceSplitter"));
    lowerWorkspaceSplitter_->setChildrenCollapsible(false);

    tutorialRegion_ = new QWidget(lowerWorkspaceSplitter_);
    tutorialRegion_->setObjectName(QStringLiteral("tutorialBoardRegion"));
    tutorialRegion_->setProperty("panel", true);
    tutorialRegion_->setMinimumSize(500, 260);
    tutorialRegion_->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Expanding);
    lowerWorkspaceSplitter_->addWidget(tutorialRegion_);
    tutorialRegion_->hide();

    auto* lowerRightHost = new QWidget(lowerWorkspaceSplitter_);
    lowerRightHost->setMinimumWidth(470);
    auto* lowerRight = new QVBoxLayout(lowerRightHost);
    lowerRight->setContentsMargins(0, 0, 0, 0);
    lowerRight->setSpacing(2);

    lowerSplitter_ = new QSplitter(Qt::Vertical, lowerRightHost);
    lowerSplitter_->setObjectName(QStringLiteral("transitionLibrarySplitter"));
    lowerSplitter_->setChildrenCollapsible(false);
    lowerSplitter_->addWidget(transitionPanel_);
    lowerSplitter_->addWidget(libraryWidget_);
    lowerSplitter_->setStretchFactor(0, 1);
    lowerSplitter_->setStretchFactor(1, 2);
    lowerSplitter_->setSizes({220, 280});
    const QByteArray lowerState = QSettings().value(
        QStringLiteral("layout/transitionLibrarySplitter")).toByteArray();
    if (!lowerState.isEmpty())
        lowerSplitter_->restoreState(lowerState);
    connect(lowerSplitter_, &QSplitter::splitterMoved, this,
            [this] {
                if (!libraryWidget_->isVisible()) return;
                QSettings().setValue(
                    QStringLiteral("layout/transitionLibrarySplitter"),
                    lowerSplitter_->saveState());
    });
    lowerRight->addWidget(lowerSplitter_, 1);

    lowerWorkspaceSplitter_->addWidget(lowerRightHost);
    lowerWorkspaceSplitter_->setStretchFactor(0, 3);
    lowerWorkspaceSplitter_->setStretchFactor(1, 2);
    lowerWorkspaceSplitter_->setSizes({650, 550});
    const QByteArray tutorWorkspaceState = QSettings().value(
        QStringLiteral("layout/tutorialWorkspaceSplitter")).toByteArray();
    if (!tutorWorkspaceState.isEmpty())
        lowerWorkspaceSplitter_->restoreState(tutorWorkspaceState);
    connect(lowerWorkspaceSplitter_, &QSplitter::splitterMoved, this,
            [this] {
                if (!tutorialRegion_->isVisible()) return;
                QSettings().setValue(
                    QStringLiteral("layout/tutorialWorkspaceSplitter"),
                    lowerWorkspaceSplitter_->saveState());
            });
    root->addWidget(lowerWorkspaceSplitter_, 2);
    transitionPanel_->setTutorialOverlayAnchor(tutorialRegion_);

    setCentralWidget(central);

    // Menus.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("Open Music Folder…"), this,
                        &MainWindow::openMusicFolder);
    QMenu* transMenu = menuBar()->addMenu(tr("&Transitions"));
    transMenu->addAction(tr("Open Transitions Folder"), this,
                         &MainWindow::openTransitionsFolder);
    QAction* newTransitionAction = transMenu->addAction(tr("New Transition…"));
    QAction* renderSetAction = transMenu->addAction(tr("Record Set to WAV…"));
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
        recBtn_->setObjectName(QStringLiteral("masterRecordButton"));
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

    // Keep the library toggle in the same bottom status strip as REC MASTER.
    // It is added to the permanent right-hand controls below so transient
    // status messages can never cover it.
    libraryToggleBtn_ = new FitPushButton(tr("HIDE LIBRARY ▾"));
    libraryToggleBtn_->setObjectName(QStringLiteral("libraryVisibilityToggle"));
    libraryToggleBtn_->setFixedHeight(20);
    libraryToggleBtn_->setToolTip(
        tr("Show or hide the library without changing the deck or waveform layout"));
    const bool libraryVisible = QSettings().value(
        QStringLiteral("layout/libraryVisible"), true).toBool();
    libraryWidget_->setVisible(libraryVisible);
    libraryToggleBtn_->setText(libraryVisible ? tr("HIDE LIBRARY ▾")
                                             : tr("SHOW LIBRARY ▴"));
    connect(libraryToggleBtn_, &QPushButton::clicked, this, [this] {
        const bool show = !libraryWidget_->isVisible();
        if (!show) {
            QSettings().setValue(
                QStringLiteral("layout/transitionLibrarySplitter"),
                lowerSplitter_->saveState());
        }
        libraryWidget_->setVisible(show);
        if (show) {
            const QByteArray state = QSettings().value(
                QStringLiteral("layout/transitionLibrarySplitter"))
                                         .toByteArray();
            if (!state.isEmpty()) lowerSplitter_->restoreState(state);
        }
        libraryToggleBtn_->setText(show ? tr("HIDE LIBRARY ▾")
                                        : tr("SHOW LIBRARY ▴"));
        QSettings().setValue(QStringLiteral("layout/libraryVisible"), show);
    });

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
    setupMismatchTimer_ = new QTimer(this);
    setupMismatchTimer_->setInterval(420);
    connect(setupMismatchTimer_, &QTimer::timeout, this, [this] {
        setupMismatchPulse_ = !setupMismatchPulse_;
        for (SetupMismatchOverlay* overlay : setupMismatchOverlays_)
            if (overlay) overlay->setPulse(setupMismatchPulse_);
    });
    midiLabel_ = new QLabel;
    midiLabel_->setObjectName(QStringLiteral("midiConnectionStatus"));
    rateLabel_ = new QLabel;
    updateAudioOutputLabel();
    // Permanent widgets survive QStatusBar::showMessage(). Keep the library
    // action immediately to the left of the controller connection status.
    statusBar()->addPermanentWidget(libraryToggleBtn_);
    statusBar()->addPermanentWidget(midiLabel_);
    statusBar()->addPermanentWidget(rateLabel_);
    onMidiConnection(midi_->controllerConnected(), midi_->controllerName());
    connect(midi_, &MidiEngine::connectionChanged, this,
            &MainWindow::onMidiConnection);
    connect(midi_, &MidiEngine::softTakeoverChanged, this,
            &MainWindow::refreshSoftTakeoverUi);
    connect(midi_, &MidiEngine::hardwareStateChanged, this,
            &MainWindow::refreshHardwareStateUi);
    connect(midi_, &MidiEngine::hardwareInputFrozenChanged,
            transitionPanel_, &TransitionPanel::setHardwareInputFrozen);
    connect(midi_, &MidiEngine::hardwareInputFrozenChanged, this,
            [this](bool frozen) {
                QSignalBlocker block(freezeHardwareCheck_);
                freezeHardwareCheck_->setChecked(frozen);
                refreshHardwareStateUi();
            });
    connect(showHardwareCheck_, &QCheckBox::toggled, this,
            [this] { refreshHardwareStateUi(); });
    connect(getHardwareStateBtn_, &QPushButton::clicked, this, [this] {
        midi_->refreshHardwareState();
        if (!midi_->controllerConnected()) {
            statusBar()->showMessage(
                tr("No controller connected — hardware state is unknown"),
                5000);
            return;
        }
        const std::vector<SoftTakeoverState> states =
            midi_->hardwareControlStates();
        const int known = static_cast<int>(std::count_if(
            states.begin(), states.end(),
            [](const SoftTakeoverState& state) {
                return state.hardwareKnown;
            }));
        if (known < static_cast<int>(states.size())) {
            statusBar()->showMessage(
                tr("Hardware state: %1/%2 controls known — move each unknown "
                   "knob or fader once")
                    .arg(known)
                    .arg(states.size()),
                6500);
        } else {
            const int mismatched = static_cast<int>(std::count_if(
                states.begin(), states.end(),
                [](const SoftTakeoverState& state) {
                    return std::fabs(state.hardwareValue -
                                     state.targetValue) >
                           SoftTakeover::tolerance(state.control);
                }));
            statusBar()->showMessage(
                mismatched == 0
                    ? tr("Hardware state captured: all controls match software")
                    : tr("Hardware state captured: %1 control(s) differ")
                          .arg(mismatched),
                5000);
        }
    });
    connect(freezeHardwareCheck_, &QCheckBox::toggled, midi_,
            &MidiEngine::setHardwareInputFrozen);
    connect(transitionPanel_,
            &TransitionPanel::hardwareTakeoverTrackingStarted, midi_,
            &MidiEngine::beginTransitionTakeoverTracking);
    connect(transitionPanel_,
            &TransitionPanel::hardwareTakeoverTrackingFinished, midi_,
            &MidiEngine::finishTransitionTakeoverTracking);
    connect(transitionPanel_,
            &TransitionPanel::hardwareTakeoverTrackingCancelled, midi_,
            &MidiEngine::cancelTransitionTakeoverTracking);
    connect(transitionPanel_,
            &TransitionPanel::setupMismatchControlsChanged, this,
            &MainWindow::refreshSetupMismatchUi);
    connect(bus_, &ControlBus::eventDispatched, this,
            [this](const ControlEvent& event, Origin) {
                // The authored target is unchanged, but its position on the
                // fader moves when the range changes (from either UI or MIDI).
                if (event.id == ControlId::TempoRange)
                    refreshSetupMismatchUi(setupMismatchControls_);
            });
    connect(transitionPanel_, &TransitionPanel::tutorialTargetsChanged, this,
            &MainWindow::refreshTutorialTargetUi);
    connect(transitionPanel_, &TransitionPanel::tutorialViewChanged, this,
            [this](bool open) {
                freezeHardwareCheck_->setEnabled(
                    hardwareControlsAvailable_ && !open);
            });
    connect(midi_, &MidiEngine::hardwareControlObserved, transitionPanel_,
            &TransitionPanel::observeTutorialHardwareControl);
    connect(midi_, &MidiEngine::connectionChanged, this,
            [this](bool connected, const QString&) {
                if (!connected || engine_->headphoneOutputAvailable()) return;
                engine_->refreshOutputDevices();
                updateAudioOutputLabel();
            });
    // AudioEngine owns hot-plug recovery for every output, independent of
    // whether a MIDI controller is connected.
    connect(engine_, &AudioEngine::outputDeviceChanged, this,
            [this](const QString&, bool) { updateAudioOutputLabel(); });
    transitionPanel_->setHardwareInputFrozen(midi_->hardwareInputFrozen());
    refreshHardwareStateUi();

    // Cross-widget wiring.
    store_->setSongCatalog(library_->songCatalog());
    const auto openSetRenderer = [this] {
        if (!setRenderWindow_) setRenderWindow_ = new SetRenderWindow(library_,store_,stems_,this);
        setRenderWindow_->show(); setRenderWindow_->raise(); setRenderWindow_->activateWindow();
    };
    connect(renderSetAction,&QAction::triggered,this,openSetRenderer);
    connect(libraryWidget_,&LibraryWidget::setRenderRequested,this,openSetRenderer);
    transitionEditor_ = new TransitionEditorWindow(
        engine_, library_, store_, recorder, player, rec_, stems_, this);
    connect(newTransitionAction, &QAction::triggered, transitionEditor_,
            [this] { transitionEditor_->createTransition(); });
    connect(libraryWidget_, &LibraryWidget::newTransitionRequested,
            transitionEditor_, [this] { transitionEditor_->createTransition(); });
    connect(libraryWidget_, &LibraryWidget::transitionSelected,
            transitionPanel_, &TransitionPanel::selectTransitionFile);
    connect(libraryWidget_, &LibraryWidget::transitionEditRequested,
            this, [this](const QString& path) {
                const auto found = std::find_if(
                    store_->all().begin(), store_->all().end(),
                    [&path](const GvtFile& file) { return file.filePath == path; });
                if (found != store_->all().end()) transitionEditor_->openTransition(*found);
            });
    connect(transitionPanel_, &TransitionPanel::transitionEditRequested,
            this, [this](const QString& path) {
                const auto found = std::find_if(
                    store_->all().begin(), store_->all().end(),
                    [&path](const GvtFile& file) { return file.filePath == path; });
                if (found != store_->all().end()) transitionEditor_->openTransition(*found);
            });
    connect(transitionEditor_, &TransitionEditorWindow::transitionSaved,
            transitionPanel_, [this](const QString& path) {
                transitionPanel_->refreshMatches();
                transitionPanel_->selectTransitionFile(path);
            });
    connect(transitionEditor_, &TransitionEditorWindow::statusMessage,
            this, [this](const QString& message, int timeout) {
                statusBar()->showMessage(message, timeout);
            });
    connect(transitionEditor_, &TransitionEditorWindow::previewStateChanged,
            this, [this](bool active) {
                if (centralWidget()) centralWidget()->setEnabled(!active);
                menuBar()->setEnabled(!active);
                if (recBtn_) recBtn_->setEnabled(!active);
                if (active)
                    statusBar()->showMessage(
                        tr("Transition Editor preview owns MASTER; the live workspace is locked."));
                else
                    statusBar()->clearMessage();
            });
    connect(transitionPanel_, &TransitionPanel::transitionEditingEnabled,
            libraryWidget_, &LibraryWidget::setTransitionEditingEnabled);
    connect(transitionPanel_, &TransitionPanel::temporaryCueBankChanged, this,
            [this](int deck, const QList<double>& startSeconds,
                   const QList<double>& endSeconds,
                   const QStringList& labels, const QStringList& colors) {
                if (deck != 0 && deck != 1) return;
                DeckWidget* widget = deckWidget(deck);
                if (startSeconds.isEmpty()) {
                    widget->clearTemporaryTransitionCues();
                    return;
                }
                std::array<double, 8> slotStarts {
                    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
                std::array<double, 8> slotEnds {
                    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
                for (int i = 0; i < 8 && i < startSeconds.size(); ++i)
                    slotStarts[static_cast<std::size_t>(i)] =
                        startSeconds.at(i);
                for (int i = 0; i < 8 && i < endSeconds.size(); ++i)
                    slotEnds[static_cast<std::size_t>(i)] = endSeconds.at(i);
                widget->setTemporaryTransitionCues(
                    slotStarts, slotEnds, labels, colors);
            });
    libraryWidget_->setTransitionEditingEnabled(true);
    connect(transitionPanel_, &TransitionPanel::tutorialViewChanged, this,
            [this](bool open) {
                tutorialRegion_->setVisible(open);
                if (open) {
                    const QByteArray state = QSettings().value(
                        QStringLiteral("layout/tutorialWorkspaceSplitter"))
                                                 .toByteArray();
                    if (!state.isEmpty())
                        lowerWorkspaceSplitter_->restoreState(state);
                    else
                        lowerWorkspaceSplitter_->setSizes({650, 550});
                }
            });
    connect(libraryWidget_, &LibraryWidget::trackLoaded, this,
            [this](int deck) { notifyTrackLoaded(deck); });
    connect(store_, &TransitionStore::changed, this,
            [this] { library_->rebuildTransitionGraph(*store_); });
    connect(library_, &TrackLibrary::trackReady, this,
            [this](int) { library_->rebuildTransitionGraph(*store_); });
    library_->rebuildTransitionGraph(*store_);
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
    connect(transitionPanel_,
            &TransitionPanel::tutorialPerformancePadRequested, this,
            [this](int deck, int mode, int pad, bool pressed) {
                if ((deck != 0 && deck != 1) || pad < 0 || pad >= 8 ||
                    mode < 0 ||
                    mode >= static_cast<int>(PerformancePadMode::Count)) {
                    return;
                }
                deckWidget(deck)->triggerPerformancePad(
                    static_cast<PerformancePadMode>(mode), pad, pressed);
            });
    connect(midi_, &MidiEngine::hotCueClearRequested, this,
            [this](int deck, int pad) {
                if (deck == 0 || deck == 1)
                    deckWidget(deck)->requestHotCueClear(pad);
            });
    const auto guardHotCueRemoval = [this](int deck, int pad) {
        if ((deck != 0 && deck != 1) || pad < 0 || pad >= 8)
            return;
        const TrackDataPtr track = engine_->deck(deck).track();
        if (!track || track->hotCues[pad] < 0.0)
            return;

        QStringList dependentTransitions;
        const ControlId cueControl = static_cast<ControlId>(
            static_cast<int>(ControlId::HotCue1) + pad);
        for (const GvtFile& file : store_->all()) {
            if (file.sourceFormat == TransitionSourceFormat::PortableYaml)
                continue; // portable cues live in the isolated CUSTOM bank
            const auto roleUsesCue = [&](Role role) {
                return std::any_of(
                    file.events.begin(), file.events.end(),
                    [role, cueControl](const GvtEvent& event) {
                        return event.role == role &&
                               event.control == cueControl;
                    });
            };
            const bool fromDependency =
                isReliableTrackMatch(matchTrack(file.from, *track)) &&
                (hotCueBeatIsMapped(
                     file.fromHotCueBeats[static_cast<std::size_t>(pad)]) ||
                 roleUsesCue(Role::FromDeck));
            const bool toDependency =
                isReliableTrackMatch(matchTrack(file.to, *track)) &&
                (hotCueBeatIsMapped(
                     file.toHotCueBeats[static_cast<std::size_t>(pad)]) ||
                 roleUsesCue(Role::ToDeck));
            if ((fromDependency || toDependency) &&
                !dependentTransitions.contains(file.name)) {
                dependentTransitions.append(
                    file.name.isEmpty() ? tr("Unnamed transition") : file.name);
            }
        }

        if (!dependentTransitions.isEmpty()) {
            QMessageBox warning(
                QMessageBox::Warning, tr("Hot cue used by transitions"),
                tr("HOT CUE %1 on Deck %2 is required by:\n\n• %3\n\n"
                   "Removing it can make Perform and Tutorial jump to the "
                   "wrong place. Remove it anyway?")
                    .arg(pad + 1)
                    .arg(deck == 0 ? QStringLiteral("A")
                                   : QStringLiteral("B"))
                    .arg(dependentTransitions.mid(0, 8).join(
                        QStringLiteral("\n• "))),
                QMessageBox::Cancel | QMessageBox::Yes, this);
            warning.setDefaultButton(QMessageBox::Cancel);
            warning.button(QMessageBox::Yes)->setText(tr("REMOVE HOT CUE"));
            if (warning.exec() != QMessageBox::Yes)
                return;
        }
        deckWidget(deck)->clearHotCue(pad);
    };
    connect(deckA_, &DeckWidget::hotCueRemovalRequested, this,
            guardHotCueRemoval);
    connect(deckB_, &DeckWidget::hotCueRemovalRequested, this,
            guardHotCueRemoval);
    auto wirePadLeds = [this](DeckWidget* deck) {
        connect(deck, &DeckWidget::performancePadStateChanged, midi_,
                &MidiEngine::setPerformancePadState);
        connect(deck, &DeckWidget::performancePadStateChanged,
                transitionPanel_,
                &TransitionPanel::observePerformancePadState);
        const PerformancePadMode mode = deck->performancePadMode();
        const int deckIndex = deck == deckA_ ? 0 : 1;
        const unsigned int enabled = deck->performancePadLedMask(mode);
        const unsigned int pressed = deck->performancePadPressedMask();
        midi_->setPerformancePadState(deckIndex, static_cast<int>(mode),
                                      enabled, pressed);
        transitionPanel_->observePerformancePadState(
            deckIndex, static_cast<int>(mode), enabled, pressed);
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
    connect(transitionPanel_, &TransitionPanel::stemPreparationRequested,
            this, &MainWindow::onStemsRequested);
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
    transitionPanel_->setHardwareTakeovers(pending);
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
        QWidget* target = controlTargetWidget(
            {state.deck, state.control, state.targetValue});
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
        tr("⚠ MATCH FOR PICKUP — KNOBS/FADERS ONLY: %1").arg(detail));
    pickupLabel_->show();
    pickupPulse_ = false;
    pickupTimer_->start();
}

QWidget* MainWindow::controlTargetWidget(const ControlEvent& event) const
{
    if (event.deck == 0 || event.deck == 1) {
        if (QWidget* deckControl =
                (event.deck == 0 ? deckA_ : deckB_)->controlWidget(event.id))
            return deckControl;
    }
    return mixer_->controlWidget(event.deck, event.id);
}

double MainWindow::controlDisplayFraction(const ControlEvent& event) const
{
    if (event.deck == 0 || event.deck == 1)
        return (event.deck == 0 ? deckA_ : deckB_)
            ->controlDisplayFraction(event.id, event.value);
    return std::clamp(event.value, 0.0, 1.0);
}

double MainWindow::softwareControlValue(const ControlEvent& event) const
{
    if (event.id == ControlId::Crossfader)
        return engine_->crossfader.load();
    if (event.deck < 0 || event.deck >= kNumDecks) return 0.0;
    const Deck& deck = engine_->deck(event.deck);
    switch (event.id) {
    case ControlId::Tempo: return deck.tempoRatio.load();
    case ControlId::Fader: return deck.fader.load();
    case ControlId::Trim: return deck.trim.load();
    case ControlId::EqLow: return deck.eqLow.load();
    case ControlId::EqMid: return deck.eqMid.load();
    case ControlId::EqHigh: return deck.eqHigh.load();
    case ControlId::Filter: return deck.filter.load();
    case ControlId::Quantize: return deck.quantizeHotCues.load() ? 1.0 : 0.0;
    case ControlId::FxType: return static_cast<double>(deck.fxType.load());
    case ControlId::FxOn: return deck.fxOn.load() ? 1.0 : 0.0;
    case ControlId::FxWet: return deck.fxWet.load();
    case ControlId::FxBeats: return deck.fxBeats.load();
    case ControlId::StemVocals: return deck.stemVocals.load();
    case ControlId::StemMelody: return deck.stemMelody.load();
    case ControlId::StemBass: return deck.stemBass.load();
    case ControlId::StemDrums: return deck.stemDrums.load();
    default: return 0.0;
    }
}

QString MainWindow::controlValueText(ControlId control, double value) const
{
    switch (control) {
    case ControlId::Tempo:
        return tr("×%1 (%2%3%)")
            .arg(value, 0, 'f', 3)
            .arg(value >= 1.0 ? QStringLiteral("+") : QString())
            .arg((value - 1.0) * 100.0, 0, 'f', 1);
    case ControlId::FxType:
        return QStringList {tr("Echo"), tr("Reverb"), tr("Flanger")}
            .value(std::clamp(static_cast<int>(std::lround(value)), 0, 2));
    case ControlId::FxOn:
    case ControlId::Quantize:
        return value >= 0.5 ? tr("On") : tr("Off");
    default:
        return tr("%1%").arg(value * 100.0, 0, 'f', 1);
    }
}

void MainWindow::refreshTutorialTargetUi(
    const QList<ControlEvent>& controls)
{
    for (TutorialTargetOverlay* overlay : tutorialTargetOverlays_)
        if (overlay) overlay->deleteLater();
    tutorialTargetOverlays_.clear();
    for (const ControlEvent& targetValue : controls) {
        if (targetValue.id == ControlId::Crossfader) continue;
        QWidget* target = controlTargetWidget(targetValue);
        if (!target) continue;
        const double tolerance = targetValue.id == ControlId::Tempo
                                     ? 0.002
                                 : (targetValue.id == ControlId::FxType ||
                                    targetValue.id == ControlId::FxOn ||
                                    targetValue.id == ControlId::Quantize)
                                     ? 0.001
                                     : 0.04;
        const bool mismatch = std::fabs(
            softwareControlValue(targetValue) - targetValue.value) > tolerance;
        auto* overlay = new TutorialTargetOverlay(
            target, controlDisplayFraction(targetValue), mismatch,
            tr("Tutorial target: %1")
                .arg(controlValueText(targetValue.id, targetValue.value)));
        tutorialTargetOverlays_.append(overlay);
    }
}

void MainWindow::refreshHardwareStateUi()
{
    for (HardwareGhostOverlay* overlay : hardwareGhostOverlays_)
        if (overlay) overlay->deleteLater();
    hardwareGhostOverlays_.clear();

    if (!midi_->controllerConnected()) {
        const bool frozen = midi_->hardwareInputFrozen();
        hardwareSyncLabel_->setText(
            frozen ? tr("HW FROZEN · NO CONTROLLER") : tr("HW UNKNOWN"));
        hardwareSyncLabel_->setStyleSheet(
            QStringLiteral("color:%1; font-size:9px; font-weight:bold;")
                .arg(frozen ? QStringLiteral("#67c8ff")
                            : themeDimText().name()));
        hardwareSyncLabel_->setToolTip(
            frozen ? tr("Absolute hardware controls will remain frozen after reconnection")
                   : tr("Connect a controller to compare its physical controls with software"));
        return;
    }

    const std::vector<SoftTakeoverState> states =
        midi_->hardwareControlStates();
    int known = 0;
    int mismatched = 0;
    QStringList detail;
    for (const SoftTakeoverState& state : states) {
        if (state.hardwareKnown) {
            ++known;
            if (std::fabs(state.hardwareValue - state.targetValue) >
                SoftTakeover::tolerance(state.control))
                ++mismatched;
        }
        const QString scope = state.deck == 0 ? QStringLiteral("A")
                              : state.deck == 1 ? QStringLiteral("B")
                                                : tr("MIX");
        if (!state.hardwareKnown)
            detail.append(tr("%1 %2 unknown")
                              .arg(scope,
                                   QString::fromLatin1(controlName(state.control))));
        else if (std::fabs(state.hardwareValue - state.targetValue) >
                 SoftTakeover::tolerance(state.control))
            detail.append(tr("%1 %2 HW %3 / SW %4")
                              .arg(scope,
                                   QString::fromLatin1(controlName(state.control)))
                              .arg(controlValueText(state.control,
                                                    state.hardwareValue),
                                   controlValueText(state.control,
                                                    state.targetValue)));

        if (showHardwareCheck_->isChecked()) {
            ControlEvent physical {state.deck, state.control,
                                   state.hardwareValue};
            QWidget* target = controlTargetWidget(physical);
            if (!target) continue;
            auto* overlay = new HardwareGhostOverlay(
                target, controlDisplayFraction(physical), state.hardwareKnown,
                state.hardwareKnown
                    ? tr("Hardware: %1")
                          .arg(controlValueText(state.control,
                                                state.hardwareValue))
                    : tr("Hardware position unknown"));
            hardwareGhostOverlays_.append(overlay);
        }
    }

    QString color;
    if (midi_->hardwareInputFrozen()) {
        hardwareSyncLabel_->setText(
            tr("HW FROZEN · %1 DIFFER").arg(mismatched));
        color = QStringLiteral("#67c8ff");
    } else if (known < static_cast<int>(states.size())) {
        hardwareSyncLabel_->setText(
            tr("HW PARTIAL %1/%2").arg(known).arg(states.size()));
        color = QStringLiteral("#aab1bd");
    } else if (mismatched > 0) {
        hardwareSyncLabel_->setText(
            tr("HW MISMATCH · %1").arg(mismatched));
        color = QStringLiteral("#e8a835");
    } else {
        hardwareSyncLabel_->setText(tr("HW ↔ SW SYNC"));
        color = QStringLiteral("#4cd964");
    }
    hardwareSyncLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-size:9px; font-weight:bold;")
            .arg(color));
    hardwareSyncLabel_->setToolTip(
        detail.isEmpty()
            ? tr("All reported physical controls match software")
            : detail.mid(0, 10).join(QStringLiteral("\n")));
}

void MainWindow::refreshSetupMismatchUi(
    const QList<ControlEvent>& controls)
{
    QList<double> fractions;
    fractions.reserve(controls.size());
    for (const ControlEvent& control : controls)
        fractions.append(controlDisplayFraction(control));
    const auto sameControls = [&] {
        if (controls.size() != setupMismatchControls_.size()) return false;
        if (fractions != setupMismatchFractions_) return false;
        for (qsizetype index = 0; index < controls.size(); ++index) {
            if (controls[index].deck != setupMismatchControls_[index].deck ||
                controls[index].id != setupMismatchControls_[index].id ||
                std::fabs(controls[index].value -
                          setupMismatchControls_[index].value) > 1.0e-9)
                return false;
        }
        return true;
    };
    if (sameControls()) return;

    for (SetupMismatchOverlay* overlay : setupMismatchOverlays_)
        if (overlay) overlay->deleteLater();
    setupMismatchOverlays_.clear();
    setupMismatchControls_ = controls;
    setupMismatchFractions_ = fractions;

    if (controls.isEmpty()) {
        setupMismatchTimer_->stop();
        setupMismatchPulse_ = false;
        return;
    }

    for (const ControlEvent& mismatch : controls) {
        QWidget* target = controlTargetWidget(mismatch);
        if (!target) continue;
        auto* overlay = new SetupMismatchOverlay(
            target, controlDisplayFraction(mismatch),
            tr("PRIME target: %1")
                .arg(controlValueText(mismatch.id, mismatch.value)));
        overlay->setPulse(setupMismatchPulse_);
        setupMismatchOverlays_.append(overlay);
    }
    if (!setupMismatchOverlays_.isEmpty())
        setupMismatchTimer_->start();
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
        outputName.isEmpty()
            ? tr("Waiting for audio output “%1” to reconnect. Choose another output in Settings > Audio Output.")
                  .arg(engine_->outputDevicePreference().isEmpty()
                      ? tr("System Default") : engine_->outputDevicePreference())
        : phones
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

    audioOutputMenu_->addSeparator();
    QAction* testPhones = audioOutputMenu_->addAction(
        tr("Test FLX4 headphones (2 seconds)"));
    testPhones->setEnabled(engine_->headphoneOutputAvailable());
    testPhones->setToolTip(
        tr("Plays a quiet test tone only through FLX4 outputs 3/4"));
    connect(testPhones, &QAction::triggered, this, [this] {
        engine_->startHeadphoneTest();
        statusBar()->showMessage(
            tr("Playing FLX4 headphone test · raise HEADPHONES LEVEL"),
            3000);
    });
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
    hardwareControlsAvailable_ = connected;
    showHardwareCheck_->setEnabled(connected);
    getHardwareStateBtn_->setEnabled(connected);
    freezeHardwareCheck_->setEnabled(
        connected && !transitionPanel_->tutorialViewOpen());
    if (connected) {
        midiLabel_->setText(
            tr("%1 connected").arg(name.isEmpty() ? tr("controller") : name));
        midiLabel_->setStyleSheet("color:#4cd964; font-weight:bold;");
    } else {
        midiLabel_->setText(tr("no controller — plug in any time"));
        midiLabel_->setStyleSheet(
            QStringLiteral("color:%1;").arg(themeDimText().name()));
    }
    refreshHardwareStateUi();
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
