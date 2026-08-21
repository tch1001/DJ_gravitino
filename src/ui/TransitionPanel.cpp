#include "TransitionPanel.h"
#include "FitButton.h"
#include "Flx4TutorialWidget.h"
#include "../performance/PerformancePads.h"
#include "../transitions/TransitionPlayerExt.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QAbstractButton>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QLineEdit>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

namespace gvt {

namespace {

class ToggleSelectionList final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        const QModelIndex clicked = indexAt(event->position().toPoint());
        if (clicked.isValid() && clicked.row() == currentRow() &&
            selectionModel()->isSelected(clicked)) {
            clearSelection();
            setCurrentRow(-1);
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }
};

std::vector<int> summarizedEventIndices(const GvtFile& file) {
    int firstCrossfader = -1;
    int lastCrossfader = -1;
    for (int i = 0; i < (int)file.events.size(); ++i) {
        if (file.events[(size_t)i].control != ControlId::Crossfader) continue;
        if (firstCrossfader < 0) firstCrossfader = i;
        lastCrossfader = i;
    }

    std::vector<int> indices;
    indices.reserve(file.events.size());
    for (int i = 0; i < (int)file.events.size(); ++i) {
        if (file.events[(size_t)i].control == ControlId::Crossfader &&
            i != firstCrossfader && i != lastCrossfader)
            continue;
        indices.push_back(i);
    }
    return indices;
}

Flx4PadMode tutorialPadMode(int recordedMode)
{
    switch (static_cast<PerformancePadMode>(recordedMode)) {
    case PerformancePadMode::HotCue:   return Flx4PadMode::HotCue;
    case PerformancePadMode::PadFx1:    return Flx4PadMode::PadFx1;
    case PerformancePadMode::BeatJump:  return Flx4PadMode::BeatJump;
    case PerformancePadMode::Sampler:
    case PerformancePadMode::SavedLoop:return Flx4PadMode::Custom;
    case PerformancePadMode::Keyboard:  return Flx4PadMode::Keyboard;
    case PerformancePadMode::PadFx2:    return Flx4PadMode::PadFx2;
    case PerformancePadMode::BeatLoop:  return Flx4PadMode::BeatLoop;
    case PerformancePadMode::KeyShift:  return Flx4PadMode::KeyShift;
    case PerformancePadMode::Count:     return Flx4PadMode::None;
    }
    return Flx4PadMode::None;
}

} // namespace

static QString qualityBadge(MatchQuality q)
{
    switch (q) {
    case MatchQuality::Fingerprint:  return QStringLiteral("● exact");
    case MatchQuality::TitleArtist:  return QStringLiteral("◐ title/artist");
    case MatchQuality::DurationOnly: return QStringLiteral("○ duration");
    case MatchQuality::None:         return QStringLiteral("✕ none");
    }
    return {};
}

TransitionPanel::TransitionPanel(ControlBus* bus, AudioEngine* engine,
                                 TransitionStore* store,
                                 TransitionRecorder* recorder,
                                 TransitionPlayer* player, QWidget* parent)
    : QWidget(parent), bus_(bus), engine_(engine), store_(store),
      recorder_(recorder), player_(player)
{
    setObjectName(QStringLiteral("transitionPanel"));
    setProperty("panel", true);
    setMinimumHeight(124);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(0);
    auto* contentSplitter = new QSplitter(Qt::Horizontal, this);
    contentSplitter->setObjectName(
        QStringLiteral("transitionContentSplitter"));
    contentSplitter->setChildrenCollapsible(false);
    root->addWidget(contentSplitter);

    // Left: matching transitions list.
    auto* leftPane = new QWidget(contentSplitter);
    tutorialLeftPane_ = leftPane;
    leftPane->setMinimumWidth(140);
    auto* leftCol = new QVBoxLayout(leftPane);
    leftCol->setContentsMargins(0, 0, 4, 0);
    auto* header = new QLabel(tr("TRANSITIONS"));
    header->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                         "letter-spacing:2px;")
                              .arg(themeText().name()));
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(3);
    headerRow->addWidget(header);
    headerRow->addStretch(1);
    renameBtn_ = new FitPushButton(tr("RENAME…"));
    deleteBtn_ = new FitPushButton(tr("DELETE…"));
    renameBtn_->setFixedHeight(18);
    deleteBtn_->setFixedHeight(18);
    headerRow->addWidget(renameBtn_);
    headerRow->addWidget(deleteBtn_);
    leftCol->addLayout(headerRow);
    list_ = new ToggleSelectionList;
    list_->setMinimumHeight(52);
    leftCol->addWidget(list_, 1);
    contentSplitter->addWidget(leftPane);

    // Center: deterministic sequence preview for the selected transition.
    auto* previewPane = new QWidget(contentSplitter);
    tutorialPreviewPane_ = previewPane;
    previewPane->setMinimumWidth(220);
    auto* previewCol = new QVBoxLayout(previewPane);
    previewCol->setContentsMargins(4, 0, 4, 0);
    auto* previewHeader = new QLabel(tr("EVENT SEQUENCE"));
    previewHeader->setStyleSheet(QStringLiteral("color:%1; font-weight:bold; "
                                                "letter-spacing:2px;")
                                     .arg(themeText().name()));
    auto* previewHeaderRow = new QHBoxLayout;
    previewHeaderRow->setSpacing(3);
    previewHeaderRow->addWidget(previewHeader);
    previewHeaderRow->addStretch(1);
    labelCueBtn_ = new FitPushButton(tr("LABEL CUE…"));
    labelCueBtn_->setFixedHeight(18);
    labelCueBtn_->setToolTip(
        tr("Add, change, or clear a waveform label at the selected event beat"));
    previewHeaderRow->addWidget(labelCueBtn_);
    previewCol->addLayout(previewHeaderRow);
    preview_ = new QTableWidget;
    preview_->setColumnCount(5);
    preview_->setHorizontalHeaderLabels(
        {tr("Beat"), tr("Target"), tr("Action"), tr("Value"), tr("Cue label")});
    preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_->setSelectionBehavior(QAbstractItemView::SelectRows);
    preview_->setSelectionMode(QAbstractItemView::SingleSelection);
    preview_->verticalHeader()->hide();
    preview_->horizontalHeader()->setStretchLastSection(true);
    preview_->setColumnWidth(0, 52);
    preview_->setColumnWidth(1, 68);
    preview_->setColumnWidth(2, 150); // includes "CUSTOM PAD 3 → play"
    preview_->setColumnWidth(3, 72);
    previewCol->addWidget(preview_, 1);
    contentSplitter->addWidget(previewPane);

    // Right: controls.
    auto* rightPane = new QWidget(contentSplitter);
    rightPane->setMinimumWidth(240);
    auto* rightCol = new QVBoxLayout(rightPane);
    rightCol->setContentsMargins(4, 0, 0, 0);
    auto* buttons = new QHBoxLayout;
    recBtn_ = new FitPushButton(tr("● REC"));
    recBtn_->setStyleSheet(
        "QPushButton { color:#e85555; font-weight:bold; }"
        "QPushButton:disabled { color:#4f545e; background:#24272e; "
        "border-color:#30343c; }");
    stopSaveBtn_ = new FitPushButton(tr("■ STOP && SAVE"));
    performBtn_ = new FitPushButton(tr("▶ PERFORM"));
    performBtn_->setStyleSheet(
        QStringLiteral("color:%1; font-weight:bold;").arg(deckAccent(0).name()));
    tutorialBtn_ = new FitPushButton(tr("TUTOR VIEW"));
    tutorialBtn_->setCheckable(true);
    tutorialBtn_->setToolTip(
        tr("Open the compact tutorial view without starting playback.\n"
           "Then use Perform or Prime for an 8-beat guided countdown."));
    tutorialBtn_->setStyleSheet(
        "QPushButton:checked { background:#f1c75b; color:#111318; "
        "font-weight:bold; }");
    abortBtn_ = new FitPushButton(tr("⏹ ABORT"));
    primeBtn_ = new FitPushButton(tr("⚡ PRIME"));
    primeBtn_->setToolTip(
        tr("Arm the selected transition without seeking: it fires\n"
           "automatically when playback reaches the marked entry beat\n"
           "(like entering a stored loop)."));
    for (auto* b : {recBtn_, stopSaveBtn_, performBtn_, primeBtn_, tutorialBtn_, abortBtn_})
        buttons->addWidget(b);
    rightCol->addLayout(buttons);

    tutorialGuideLabel_ = new QLabel(this);
    tutorialGuideLabel_->setWordWrap(true);
    tutorialGuideLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tutorialGuideLabel_->setStyleSheet(
        QStringLiteral("background:#20242b; color:%1; border:1px solid #4b5260; "
                       "border-radius:4px; padding:5px; font-weight:bold;")
            .arg(themeText().name()));
    tutorialGuideLabel_->hide();
    rightCol->addWidget(tutorialGuideLabel_);

    auto* setupRow = new QHBoxLayout;
    setupLabel_ = new QLabel;
    setupLabel_->setWordWrap(true);
    setupRow->addWidget(setupLabel_, 1);
    QSettings setupSettings;
    setupTolerances_.closeEnough = setupSettings.value(
        QStringLiteral("transitions/closeEnoughEnabled"), false).toBool();
    setupTolerances_.bpm = std::clamp(setupSettings.value(
        QStringLiteral("transitions/closeEnoughBpm"), 0.5).toDouble(),
        0.05, 10.0);
    setupTolerances_.volume = std::clamp(setupSettings.value(
        QStringLiteral("transitions/closeEnoughVolume"), 5.0).toDouble() /
        100.0, 0.015, 0.25);
    setupTolerances_.eq = std::clamp(setupSettings.value(
        QStringLiteral("transitions/closeEnoughEq"), 5.0).toDouble() /
        100.0, 0.015, 0.25);
    closeEnoughCheck_ = new QCheckBox(tr("CLOSE ENOUGH"), this);
    closeEnoughCheck_->setChecked(setupTolerances_.closeEnough);
    closeEnoughCheck_->setToolTip(
        tr("Accept configured BPM, volume, and EQ differences as ready; "
           "tracks and discrete setup states must still match exactly"));
    setupRow->addWidget(closeEnoughCheck_);
    toleranceBtn_ = new FitPushButton(tr("TOLERANCE…"), this);
    toleranceBtn_->setToolTip(
        tr("Set the accepted BPM, volume-fader, and EQ differences"));
    setupRow->addWidget(toleranceBtn_);
    applySetupBtn_ = new FitPushButton(tr("MATCH SETUP"));
    applySetupBtn_->setToolTip(
        tr("Restore the recorded pre-transition audio state across both "
           "decks and the mixer"));
    setupRow->addWidget(applySetupBtn_);
    rightCol->addLayout(setupRow);

    auto* statusRow = new QHBoxLayout;
    recIndicator_ = new QLabel;
    recIndicator_->setStyleSheet("color:#e85555; font-weight:bold;");
    statusRow->addWidget(recIndicator_);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    statusRow->addWidget(progress_, 1);
    rightCol->addLayout(statusRow);
    rightCol->addStretch(1);
    contentSplitter->addWidget(rightPane);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 3);
    contentSplitter->setStretchFactor(2, 2);
    contentSplitter->setSizes({260, 480, 360});
    const QByteArray contentState = setupSettings.value(
        QStringLiteral("layout/transitionContentSplitter")).toByteArray();
    if (!contentState.isEmpty())
        contentSplitter->restoreState(contentState);
    connect(contentSplitter, &QSplitter::splitterMoved, this,
            [contentSplitter] {
                QSettings().setValue(
                    QStringLiteral("layout/transitionContentSplitter"),
                    contentSplitter->saveState());
            });

    bannerTimer_ = new QTimer(this);
    bannerTimer_->setSingleShot(true);
    connect(bannerTimer_, &QTimer::timeout, this, [this] {
        if (banner_) banner_->hide();
    });

    connect(recBtn_, &QPushButton::clicked, this, &TransitionPanel::onRec);
    connect(stopSaveBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onStopSave);
    connect(performBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onPerform);
    connect(primeBtn_, &QPushButton::clicked, this, &TransitionPanel::onPrime);
    connect(list_, &QListWidget::currentRowChanged, this,
            [this](int) {
                const int idx = selectedMatch();
                selectedPath_ = idx >= 0 ? matches_[(size_t)idx].file->filePath
                                         : QString();
                updatePreview();
                announceEntryMarker();
                updateSetupStatus();
                updateControls();
                if (tutorialViewOpen_) refreshTutorialView();
            });
    connect(preview_, &QTableWidget::itemSelectionChanged, this,
            &TransitionPanel::updateControls);
    connect(tutorialBtn_, &QPushButton::toggled, this,
            &TransitionPanel::onTutorialViewToggled);
    connect(abortBtn_, &QPushButton::clicked, this, &TransitionPanel::onAbort);
    connect(renameBtn_, &QPushButton::clicked, this, &TransitionPanel::onRename);
    connect(deleteBtn_, &QPushButton::clicked, this, &TransitionPanel::onDelete);
    connect(labelCueBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onLabelCue);
    connect(applySetupBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onApplySetup);
    connect(closeEnoughCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        setupTolerances_.closeEnough = enabled;
        QSettings().setValue(
            QStringLiteral("transitions/closeEnoughEnabled"), enabled);
        updateSetupStatus();
        updateControls();
        emit statusMessage(
            enabled
                ? tr("Close Enough enabled: configured BPM, volume, and EQ margins are accepted")
                : tr("Close Enough disabled: strict recorded setup matching restored"),
            4000);
    });
    connect(toleranceBtn_, &QPushButton::clicked, this,
            &TransitionPanel::onEditSetupTolerance);

    connect(store_, &TransitionStore::changed, this,
            &TransitionPanel::refreshMatches);
    connect(recorder_, &TransitionRecorder::eventCaptured, this,
            &TransitionPanel::onEventCaptured);
    connect(player_, &TransitionPlayer::progressChanged, this,
            &TransitionPanel::onProgress);
    connect(player_, &TransitionPlayer::finished, this,
            &TransitionPanel::onFinished);
    connect(player_, &TransitionPlayer::tutorialPrompt, this,
            &TransitionPanel::onTutorialPrompt);
    connect(player_, &TransitionPlayer::tutorialScored, this,
            &TransitionPanel::onTutorialScored);

    // Deck transport and continuous setup values are atomics, so a light UI
    // poll keeps enablement/preflight feedback correct even at EOF or after a
    // physical controller move that does not emit a dedicated state signal.
    stateTimer_ = new QTimer(this);
    stateTimer_->setInterval(100);
    connect(stateTimer_, &QTimer::timeout, this, [this] {
        updateControls();
        updateSetupStatus();
        if (tutorialViewOpen_) {
            layoutTutorialOverlay();
            refreshTutorialLiveState();
            refreshTutorialGuideLabel();
        }
    });
    stateTimer_->start();

    refreshMatches();
}

void TransitionPanel::refreshMatches()
{
    const QString restorePath = selectedPath_;
    matches_.clear();
    list_->clear();
    preview_->setRowCount(0);

    TrackDataPtr a = engine_->deck(0).track();
    TrackDataPtr b = engine_->deck(1).track();
    if (!a || !b) {
        auto* item = new QListWidgetItem(
            tr("load a track on both decks to see matching transitions"));
        item->setFlags(Qt::NoItemFlags);
        list_->addItem(item);
        selectedPath_.clear();
        announceEntryMarker();
        updateSetupStatus();
        updateControls();
        return;
    }

    // Both orderings: A→B (fromDeck 0) and B→A (fromDeck 1).
    struct Ordering { const TrackData* from; const TrackData* to; int fromDeck; };
    const Ordering orderings[2] = {{a.get(), b.get(), 0},
                                   {b.get(), a.get(), 1}};
    for (const auto& o : orderings) {
        for (const GvtFile* f : store_->matching(*o.from, *o.to)) {
            Match m;
            m.file = f;
            m.fromDeck = o.fromDeck;
            m.quality = std::min(matchTrack(f->from, *o.from),
                                 matchTrack(f->to, *o.to));
            matches_.push_back(m);
        }
    }

    if (matches_.empty()) {
        auto* item =
            new QListWidgetItem(tr("no saved transitions match this pair"));
        item->setFlags(Qt::NoItemFlags);
        list_->addItem(item);
        selectedPath_.clear();
        announceEntryMarker();
        updateSetupStatus();
        updateControls();
        return;
    }
    for (const Match& m : matches_) {
        QString dir = m.fromDeck == 0 ? QStringLiteral("A→B")
                                      : QStringLiteral("B→A");
        QString author =
            m.file->author.isEmpty() ? tr("unknown") : m.file->author;
        list_->addItem(QStringLiteral("%1  —  %2  [%3]  %4")
                           .arg(m.file->name, author, dir,
                                qualityBadge(m.quality)));
    }
    int restoreRow = 0;
    if (!restorePath.isEmpty()) {
        for (int i = 0; i < (int)matches_.size(); ++i) {
            if (matches_[(size_t)i].file->filePath == restorePath) {
                restoreRow = i;
                break;
            }
        }
    }
    list_->setCurrentRow(restoreRow);
    // setCurrentRow does not emit when restoring the already-current row in a
    // freshly rebuilt model on every Qt version.
    selectedPath_ = matches_[(size_t)restoreRow].file->filePath;
    updatePreview();
    announceEntryMarker();
    updateSetupStatus();
    updateControls();
}

int TransitionPanel::selectedMatch() const
{
    int row = list_->currentRow();
    if (row < 0 || row >= (int)matches_.size()) return -1;
    return row;
}

void TransitionPanel::onRec()
{
    if (recorder_->isRecording()) {
        emit statusMessage(tr("Already recording"), 3000);
        return;
    }
    const bool aPlaying = engine_->deck(0).playing.load();
    const bool bPlaying = engine_->deck(1).playing.load();
    if (aPlaying == bPlaying) { // neither, or both
        emit statusMessage(
            aPlaying
                ? tr("Can't arm recording: both decks are playing — the "
                     "outgoing deck must be the only one playing")
                : tr("Can't arm recording: start the outgoing deck first"),
            5000);
        return;
    }
    int fromDeck = aPlaying ? 0 : 1;
    recorder_->start(fromDeck);
    capturedCount_ = 0;
    recIndicator_->setText(tr("REC ● 0 events (from deck %1)")
                               .arg(fromDeck == 0 ? QStringLiteral("A")
                                                  : QStringLiteral("B")));
    emit statusMessage(tr("Recording transition…"), 3000);
    updateControls();
}

void TransitionPanel::onEventCaptured(int count)
{
    recIndicator_->setText(tr("REC ● %1 events").arg(count));
    capturedCount_ = count;
    updateControls();
}

void TransitionPanel::onStopSave()
{
    if (!recorder_->isRecording()) {
        emit statusMessage(tr("Not recording"), 3000);
        return;
    }
    GvtFile f = recorder_->finish();
    capturedCount_ = 0;
    recIndicator_->clear();
    updateControls();

    bool ok = false;
    QString name = QInputDialog::getText(
        this, tr("Save transition"), tr("Transition name:"),
        QLineEdit::Normal,
        f.name.isEmpty() ? tr("My Transition") : f.name, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        emit statusMessage(tr("Recording discarded"), 3000);
        return;
    }
    f.name = name.trimmed();
    QString error;
    QString path = store_->save(f, &error);
    if (path.isEmpty())
        emit statusMessage(tr("Save failed: %1").arg(error), 6000);
    else
        emit statusMessage(tr("Saved %1").arg(path), 5000);
    // Store emits changed() -> refreshMatches().
    updateControls();
}

void TransitionPanel::announceEntryMarker()
{
    const int idx = selectedMatch();
    if (idx < 0) {
        emit entryMarkerChanged(0, -1.0);
        emit entryMarkerChanged(1, -1.0);
        emit cueMarkersChanged(0, {}, {});
        emit cueMarkersChanged(1, {}, {});
        return;
    }
    const Match& m = matches_[(size_t)idx];
    double sec = -1.0;
    if (TrackDataPtr t = engine_->deck(m.fromDeck).track())
        sec = t->secAtBeat(m.file->anchorFromBeat);
    emit entryMarkerChanged(m.fromDeck, sec);
    emit entryMarkerChanged(m.fromDeck == 0 ? 1 : 0, -1.0);

    // Build a compact set of significant moments. Internal crossfader
    // checkpoints stay in the replay, but only its endpoints become automatic
    // markers. Explicit user labels always win.
    std::map<double, QString> cues;
    cues[0.0] = tr("Transition start");
    const std::vector<int> summary = summarizedEventIndices(*m.file);
    int firstCrossfader = -1, lastCrossfader = -1;
    for (int i : summary) {
        if (m.file->events[(size_t)i].control != ControlId::Crossfader) continue;
        if (firstCrossfader < 0) firstCrossfader = i;
        lastCrossfader = i;
    }
    for (int i : summary) {
        const GvtEvent& event = m.file->events[(size_t)i];
        QString label = automaticCueLabel(event);
        if (event.control == ControlId::Crossfader && firstCrossfader != lastCrossfader)
            label = i == firstCrossfader ? tr("Crossfade start")
                                          : tr("Crossfade end");
        if (!label.isEmpty() && cues.find(event.beat) == cues.end())
            cues[event.beat] = label;
    }
    for (const GvtCue& cue : m.file->cues)
        if (!cue.label.trimmed().isEmpty()) cues[cue.beat] = cue.label.trimmed();

    const int toDeck = m.fromDeck == 0 ? 1 : 0;
    for (int physicalDeck : {m.fromDeck, toDeck}) {
        QList<double> seconds;
        QStringList labels;
        if (TrackDataPtr track = engine_->deck(physicalDeck).track()) {
            const double anchor = physicalDeck == m.fromDeck
                                      ? m.file->anchorFromBeat
                                      : m.file->anchorToBeat;
            for (const auto& [relativeBeat, label] : cues) {
                seconds.append(track->secAtBeat(anchor + relativeBeat));
                labels.append(label);
            }
        }
        emit cueMarkersChanged(physicalDeck, seconds, labels);
    }
}

QString TransitionPanel::automaticCueLabel(const GvtEvent& event) const
{
    switch (event.control) {
    case ControlId::TempoSync: return tr("Align beats");
    case ControlId::Play:
        return event.role == Role::ToDeck ? tr("Bring in track")
                                          : tr("Start outgoing");
    case ControlId::Crossfader: return tr("Crossfade");
    case ControlId::EqLow:
    case ControlId::EqMid:
    case ControlId::EqHigh: return tr("EQ change");
    case ControlId::Filter: return tr("Filter move");
    case ControlId::Fader: return tr("Channel level");
    case ControlId::FxOn:
    case ControlId::FxType: return tr("FX change");
    case ControlId::StemVocals:
    case ControlId::StemMelody:
    case ControlId::StemBass:
    case ControlId::StemDrums: return tr("Stem change");
    case ControlId::LoopIn:
    case ControlId::LoopOut:
    case ControlId::LoopAuto:
    case ControlId::LoopExit: return tr("Loop move");
    case ControlId::Stop:
        return event.role == Role::FromDeck ? tr("Exit outgoing")
                                            : tr("Stop incoming");
    default: return {};
    }
}

QString TransitionPanel::cueLabelAt(const GvtFile& file, double beat) const
{
    for (const GvtCue& cue : file.cues)
        if (std::fabs(cue.beat - beat) < 0.0005) return cue.label;
    return {};
}

void TransitionPanel::updatePreview()
{
    preview_->clearContents();
    preview_->setRowCount(0);
    const int idx = selectedMatch();
    if (idx < 0) return;
    const GvtFile& file = *matches_[(size_t)idx].file;
    const std::vector<int> summary = summarizedEventIndices(file);
    preview_->setRowCount((int)summary.size());

    int firstCrossfader = -1, lastCrossfader = -1;
    for (int i : summary) {
        if (file.events[(size_t)i].control != ControlId::Crossfader) continue;
        if (firstCrossfader < 0) firstCrossfader = i;
        lastCrossfader = i;
    }

    auto targetText = [this](Role role) {
        switch (role) {
        case Role::FromDeck: return tr("Outgoing");
        case Role::ToDeck: return tr("Incoming");
        case Role::Mixer: return tr("Mixer");
        }
        return QString();
    };
    auto curveText = [this](Curve curve) {
        switch (curve) {
        case Curve::Step: return QString();
        case Curve::Linear: return tr("linear");
        case Curve::SCurve: return tr("S-curve");
        }
        return QString();
    };

    for (int row = 0; row < (int)summary.size(); ++row) {
        const int eventIndex = summary[(size_t)row];
        const GvtEvent& event = file.events[(size_t)eventIndex];
        auto* beatItem = new QTableWidgetItem(
            QStringLiteral("+%1").arg(event.beat, 0, 'f', 3));
        beatItem->setData(Qt::UserRole, eventIndex);
        preview_->setItem(row, 0, beatItem);
        preview_->setItem(row, 1, new QTableWidgetItem(targetText(event.role)));

        QString action = QString::fromUtf8(controlName(event.control));
        action.replace(QLatin1Char('_'), QLatin1Char(' '));
        if (event.control == ControlId::Crossfader) {
            action = firstCrossfader == lastCrossfader
                         ? tr("crossfade")
                         : (eventIndex == firstCrossfader
                                ? tr("crossfade start")
                                : tr("crossfade end"));
        }
        if (event.gestureControl >= ControlId::PerformancePad1 &&
            event.gestureControl <= ControlId::PerformancePad8 &&
            event.gesturePadMode >= 0 &&
            event.gesturePadMode <
                static_cast<int>(PerformancePadMode::Count)) {
            const int pad = static_cast<int>(event.gestureControl) -
                            static_cast<int>(ControlId::PerformancePad1) + 1;
            const auto mode =
                static_cast<PerformancePadMode>(event.gesturePadMode);
            action = tr("%1 PAD %2 → %3")
                         .arg(QLatin1String(performancePadModeLabel(mode)))
                         .arg(pad)
                         .arg(action);
        }
        preview_->setItem(row, 2, new QTableWidgetItem(action));

        QString value;
        if (controlIsTrigger(event.control)) {
            value = event.value >= 0.5 ? tr("press") : tr("release");
        } else if (event.control == ControlId::Tempo) {
            value = QStringLiteral("×%1").arg(event.value, 0, 'f', 3);
        } else if (event.control == ControlId::BeatJump ||
                   event.control == ControlId::LoopAuto ||
                   event.control == ControlId::FxBeats) {
            value = tr("%1 beats").arg(event.value, 0, 'f', 2);
        } else if (event.control == ControlId::FxType) {
            const int type = std::clamp((int)std::lround(event.value), 0, 2);
            value = QStringList {tr("echo"), tr("reverb"), tr("flanger")}.at(type);
        } else if (event.control == ControlId::Quantize) {
            value = event.value >= 0.5 ? tr("On") : tr("Off");
        } else {
            value = QStringLiteral("%1%").arg(event.value * 100.0, 0, 'f', 0);
        }
        const QString curve = curveText(event.curve);
        if (!curve.isEmpty()) value += QStringLiteral(" · ") + curve;
        preview_->setItem(row, 3, new QTableWidgetItem(value));

        QString label = cueLabelAt(file, event.beat);
        if (label.isEmpty()) label = automaticCueLabel(event);
        preview_->setItem(row, 4, new QTableWidgetItem(label));
    }
    if (!summary.empty()) preview_->selectRow(0);
}

bool TransitionPanel::setupMatches(const Match& match,
                                   QStringList* differences,
                                   bool honorCloseEnough) const
{
    QStringList local;
    const bool complete = match.file->initialComplete;
    TransitionSetupTolerances tolerance = setupTolerances_;
    if (!honorCloseEnough)
        tolerance.closeEnough = false;
    const auto compare = [&local, this, &tolerance](
                             const QString& name, double actual,
                             double wanted,
                             SetupToleranceField field =
                                 SetupToleranceField::Other,
                             double strictTolerance = 0.015) {
        if (!transitionSetupValueMatches(
                actual, wanted, field, strictTolerance, tolerance))
            local.append(tr("%1 %2 → %3")
                             .arg(name)
                             .arg(actual, 0, 'f', 2)
                             .arg(wanted, 0, 'f', 2));
    };
    const auto compareDeck = [&](bool fromRole) {
        const int physical = fromRole ? match.fromDeck : 1 - match.fromDeck;
        const QString deckName = physical == 0 ? QStringLiteral("A")
                                                : QStringLiteral("B");
        const Deck& deck = engine_->deck(physical);
        const GvtInitialState& expected = fromRole ? match.file->initialFrom
                                                    : match.file->initialTo;
        const TrackDataPtr track = deck.track();
        if (!track) {
            local.append(tr("deck %1 track missing").arg(deckName));
            return;
        }
        if (!expected.captured) {
            if (fromRole && match.file->masterBpm > 0.0)
                compare(tr("%1 BPM").arg(deckName), deck.effectiveBpm(),
                        match.file->masterBpm, SetupToleranceField::Bpm, 0.05);
            return;
        }

        const double wantedRatio = expectedTempoRatio(match, fromRole);
        compare(tr("%1 BPM").arg(deckName), deck.effectiveBpm(),
                track->bpm * wantedRatio, SetupToleranceField::Bpm, 0.05);
        compare(tr("%1 fader").arg(deckName), deck.fader.load(), expected.fader,
                SetupToleranceField::Volume);
        compare(tr("%1 low").arg(deckName), deck.eqLow.load(), expected.eqLow,
                SetupToleranceField::Eq);
        compare(tr("%1 mid").arg(deckName), deck.eqMid.load(), expected.eqMid,
                SetupToleranceField::Eq);
        compare(tr("%1 high").arg(deckName), deck.eqHigh.load(), expected.eqHigh,
                SetupToleranceField::Eq);
        compare(tr("%1 filter").arg(deckName), deck.filter.load(), expected.filter);
        if (!complete) return;

        if (expected.quantizeCaptured &&
            deck.quantizeHotCues.load() != expected.quantize)
            local.append(tr("%1 Quantize on/off").arg(deckName));

        if (deck.fxType.load() != expected.fxType)
            local.append(tr("%1 FX type").arg(deckName));
        if (deck.fxOn.load() != expected.fxOn)
            local.append(tr("%1 FX on/off").arg(deckName));
        compare(tr("%1 FX wet").arg(deckName), deck.fxWet.load(), expected.fxWet);
        compare(tr("%1 FX beats").arg(deckName), deck.fxBeats.load(),
                expected.fxBeats, SetupToleranceField::Other, 0.01);
        compare(tr("%1 vocals").arg(deckName), deck.stemVocals.load(),
                expected.stemVocals);
        compare(tr("%1 melody").arg(deckName), deck.stemMelody.load(),
                expected.stemMelody);
        compare(tr("%1 bass").arg(deckName), deck.stemBass.load(),
                expected.stemBass);
        compare(tr("%1 drums").arg(deckName), deck.stemDrums.load(),
                expected.stemDrums);
        if (deck.loopActive.load() != expected.loopActive)
            local.append(tr("%1 loop on/off").arg(deckName));
        if (expected.loopActive) {
            compare(tr("%1 loop start").arg(deckName),
                    track->beatAtSec(deck.loopStartSec.load()),
                    expected.loopStartBeat, SetupToleranceField::Other, 0.02);
            compare(tr("%1 loop end").arg(deckName),
                    track->beatAtSec(deck.loopEndSec.load()),
                    expected.loopEndBeat, SetupToleranceField::Other, 0.02);
        }
        compare(tr("%1 cue").arg(deckName),
                track->beatAtSec(deck.cuePointSec.load()),
                expected.cueBeat, SetupToleranceField::Other, 0.02);
    };

    compareDeck(true);
    if (complete) compareDeck(false);
    if (complete && match.file->initialMixerCaptured) {
        const double wanted = match.fromDeck == 0
                                  ? match.file->initialCrossfader
                                  : 1.0 - match.file->initialCrossfader;
        compare(tr("crossfader"), engine_->crossfader.load(), wanted,
                SetupToleranceField::Volume);
    }
    if (differences) *differences = local;
    return local.isEmpty();
}

double TransitionPanel::expectedTempoRatio(const Match& match,
                                           bool fromRole) const
{
    const int physical = fromRole ? match.fromDeck : 1 - match.fromDeck;
    const TrackDataPtr loaded = engine_->deck(physical).track();
    const GvtInitialState& state = fromRole ? match.file->initialFrom
                                            : match.file->initialTo;
    const GvtTrackRef& recorded = fromRole ? match.file->from : match.file->to;
    if (!loaded || loaded->bpm <= 0.0) return state.tempoRatio;
    const double effective = fromRole && match.file->masterBpm > 0.0
                                 ? match.file->masterBpm
                                 : recorded.bpm * state.tempoRatio;
    return effective > 0.0 ? effective / loaded->bpm : state.tempoRatio;
}

QStringList TransitionPanel::primeReadinessIssues(const Match& match) const
{
    QStringList issues;
    setupMatches(match, &issues);

    const Deck& outgoing = engine_->deck(match.fromDeck);
    const double currentBeat = outgoing.beatPosition();
    if (currentBeat > match.file->anchorFromBeat + 0.05)
        issues.append(tr("entry beat has already passed"));
    if (!outgoing.playing.load() &&
        currentBeat >= match.file->anchorFromBeat - 0.05)
        issues.append(tr("paused outgoing deck must be before the entry beat"));
    if (outgoing.loopActive.load()) {
        const TrackDataPtr track = outgoing.track();
        if (track) {
            const double start = track->beatAtSec(outgoing.loopStartSec.load());
            const double end = track->beatAtSec(outgoing.loopEndSec.load());
            if (match.file->anchorFromBeat < start ||
                match.file->anchorFromBeat >= end)
                issues.append(tr("outgoing loop cannot reach the entry beat"));
        }
    }

    const int incomingIndex = 1 - match.fromDeck;
    const Deck& incoming = engine_->deck(incomingIndex);
    if (incoming.playing.load())
        issues.append(tr("incoming deck must be stopped"));
    const double expectedBeat = match.file->initialComplete &&
                                        match.file->initialTo.captured
                                    ? match.file->initialTo.positionBeat
                                    : match.file->anchorToBeat;
    if (std::fabs(incoming.beatPosition() - expectedBeat) > 0.05)
        issues.append(tr("incoming cue position is not prepared"));
    return issues;
}

void TransitionPanel::updateSetupStatus()
{
    const int idx = selectedMatch();
    if (idx < 0) {
        setupLabel_->setText(tr("Select a transition to inspect its setup"));
        setupLabel_->setStyleSheet(
            QStringLiteral("color:%1;").arg(themeDimText().name()));
        return;
    }

    const Match& match = matches_[(size_t)idx];
    QStringList differences;
    const bool ready = setupMatches(match, &differences);
    const bool strictlyReady = ready && setupTolerances_.closeEnough
        ? setupMatches(match, nullptr, false) : ready;
    const QString acceptedSuffix = ready && !strictlyReady
        ? tr(" (close enough: ±%1 BPM, ±%2% volume, ±%3% EQ)")
              .arg(setupTolerances_.bpm, 0, 'f', 2)
              .arg(setupTolerances_.volume * 100.0, 0, 'f', 1)
              .arg(setupTolerances_.eq * 100.0, 0, 'f', 1)
        : QString();
    if (!match.file->initialFrom.captured) {
        QString text = ready
                           ? tr("Outgoing setup: BPM ready%1; EQ was not stored in this older transition")
                                 .arg(acceptedSuffix)
                           : tr("Outgoing setup: %1; EQ was not stored in this older transition")
                                 .arg(differences.join(QStringLiteral(", ")));
        setupLabel_->setText(text);
        setupLabel_->setStyleSheet("color:#e8a835;");
        return;
    }
    QString summary = differences.mid(0, 4).join(QStringLiteral(", "));
    if (differences.size() > 4)
        summary += tr(", +%1 more").arg(differences.size() - 4);
    setupLabel_->setText(
        ready ? (match.file->initialComplete
                     ? tr("Full pre-transition state: ready%1").arg(acceptedSuffix)
                     : tr("Outgoing setup: ready%1 (legacy partial snapshot)")
                           .arg(acceptedSuffix))
              : (match.file->initialComplete
                     ? tr("Pre-transition state: %1").arg(summary)
                     : tr("Outgoing setup: %1").arg(summary)));
    setupLabel_->setStyleSheet(ready ? "color:#4cd964;" : "color:#e8a835;");
}

void TransitionPanel::updateControls()
{
    const bool recording = recorder_->isRecording();
    const bool replaying = player_->isActive();
    const bool busy = recording || replaying;
    const bool selected = selectedMatch() >= 0;
    const bool tracksLoaded = engine_->deck(0).track() && engine_->deck(1).track();
    const bool aPlaying = engine_->deck(0).playing.load();
    const bool bPlaying = engine_->deck(1).playing.load();
    const bool oneOutgoing = aPlaying != bPlaying;

    recBtn_->setEnabled(!busy && tracksLoaded && oneOutgoing);
    stopSaveBtn_->setEnabled(recording && capturedCount_ > 0);
    performBtn_->setEnabled(selected && !busy);
    bool primeTimingReady = false;
    if (selected && !busy) {
        const Match& match = matches_[(size_t)selectedMatch()];
        const Deck& outgoing = engine_->deck(match.fromDeck);
        const double beat = outgoing.beatPosition();
        primeTimingReady = outgoing.playing.load()
                               ? beat <= match.file->anchorFromBeat + 0.05
                               : beat < match.file->anchorFromBeat - 0.05;
    }
    primeBtn_->setEnabled(selected && !busy && primeTimingReady);
    // The checked TUTOR VIEW remains closable during a guided run. It cannot be
    // opened on top of an unrelated automatic Perform or while recording.
    tutorialBtn_->setEnabled(
        selected && !recording && (!replaying || tutorialActive_));
    abortBtn_->setEnabled(busy);
    renameBtn_->setEnabled(selected && !busy);
    deleteBtn_->setEnabled(selected && !busy);
    const bool eventSelected = preview_->currentRow() >= 0;
    labelCueBtn_->setEnabled(selected && eventSelected && !busy);

    bool setupReady = true;
    if (selected) setupReady = setupMatches(matches_[(size_t)selectedMatch()]);
    applySetupBtn_->setEnabled(selected && !busy && !setupReady);
    closeEnoughCheck_->setEnabled(!busy);
    toleranceBtn_->setEnabled(!busy);
    list_->setEnabled(!busy);
    preview_->setEnabled(!busy);

    if (busy) {
        recBtn_->setToolTip(tr("Finish or abort the current transition first"));
    } else if (!tracksLoaded) {
        recBtn_->setToolTip(tr("Load both decks before recording a transition"));
    } else if (!oneOutgoing) {
        recBtn_->setToolTip(tr("Start only the outgoing deck before recording"));
    } else {
        recBtn_->setToolTip(tr("Record a transition from the currently playing deck"));
    }
    stopSaveBtn_->setToolTip(
        recording && capturedCount_ == 0
            ? tr("Make at least one transition move, or use Abort to discard")
            : tr("Stop recording and save the captured transition"));
    if (!selected)
        primeBtn_->setToolTip(tr("Select a transition first"));
    else if (!primeTimingReady && !busy)
        primeBtn_->setToolTip(
            tr("Prime requires the outgoing deck to be before the entry marker"));
    else
        primeBtn_->setToolTip(
            tr("Verify and restore the complete pre-transition state, then arm "
               "the transition at its entry marker"));
}

void TransitionPanel::applyInitialSetup(const Match& match, bool announce,
                                        bool prepareFromTransport,
                                        bool prepareToTransport)
{
    const auto applyDeck = [&](bool fromRole, bool prepareTransport) {
        const int deckIndex = fromRole ? match.fromDeck : 1 - match.fromDeck;
        Deck& deck = engine_->deck(deckIndex);
        const GvtInitialState& setup = fromRole ? match.file->initialFrom
                                                : match.file->initialTo;
        if (!setup.captured) return;
        if (prepareTransport)
            bus_->dispatch({deckIndex, ControlId::Stop, 1.0}, Origin::System);
        const std::pair<ControlId, double> values[] = {
            {ControlId::Tempo, expectedTempoRatio(match, fromRole)},
            {ControlId::Fader, setup.fader},
            {ControlId::EqLow, setup.eqLow},
            {ControlId::EqMid, setup.eqMid},
            {ControlId::EqHigh, setup.eqHigh},
            {ControlId::Filter, setup.filter},
        };
        for (const auto& [control, value] : values)
            bus_->dispatch({deckIndex, control, value}, Origin::System);
        if (!match.file->initialComplete) return;
        const std::pair<ControlId, double> extended[] = {
            {ControlId::FxType, (double)setup.fxType},
            {ControlId::FxOn, setup.fxOn ? 1.0 : 0.0},
            {ControlId::FxWet, setup.fxWet},
            {ControlId::FxBeats, setup.fxBeats},
            {ControlId::StemVocals, setup.stemVocals},
            {ControlId::StemMelody, setup.stemMelody},
            {ControlId::StemBass, setup.stemBass},
            {ControlId::StemDrums, setup.stemDrums},
        };
        for (const auto& [control, value] : extended)
            bus_->dispatch({deckIndex, control, value}, Origin::System);
        if (setup.quantizeCaptured)
            bus_->dispatch({deckIndex, ControlId::Quantize,
                            setup.quantize ? 1.0 : 0.0}, Origin::System);

        if (TrackDataPtr track = deck.track()) {
            deck.cuePointSec.store(track->secAtBeat(setup.cueBeat));
            deck.loopStartSec.store(track->secAtBeat(setup.loopStartBeat));
            deck.loopEndSec.store(track->secAtBeat(setup.loopEndBeat));
            deck.loopActive.store(setup.loopActive &&
                                  setup.loopEndBeat > setup.loopStartBeat);
            if (prepareTransport)
                deck.seekSec(track->secAtBeat(setup.positionBeat));
        }
    };

    applyDeck(true, prepareFromTransport);
    if (match.file->initialComplete)
        applyDeck(false, prepareToTransport);
    else if (prepareToTransport) {
        const int incoming = 1 - match.fromDeck;
        bus_->dispatch({incoming, ControlId::Stop, 1.0}, Origin::System);
        if (TrackDataPtr track = engine_->deck(incoming).track())
            engine_->deck(incoming).seekSec(
                track->secAtBeat(match.file->anchorToBeat));
    }
    if (match.file->initialComplete && match.file->initialMixerCaptured) {
        const double physical = match.fromDeck == 0
                                    ? match.file->initialCrossfader
                                    : 1.0 - match.file->initialCrossfader;
        bus_->dispatch({kNoDeck, ControlId::Crossfader, physical},
                       Origin::System);
    } else if (!match.file->initialFrom.captured) {
        const int outgoing = match.fromDeck;
        if (TrackDataPtr track = engine_->deck(outgoing).track();
            track && track->bpm > 0.0 && match.file->masterBpm > 0.0)
            bus_->dispatch({outgoing, ControlId::Tempo,
                            match.file->masterBpm / track->bpm},
                           Origin::System);
    }
    updateSetupStatus();
    updateControls();
    if (announce)
        emit statusMessage(match.file->initialComplete
                               ? tr("Matched both decks and mixer to the recorded pre-state")
                               : tr("Matched the available legacy outgoing setup"),
                           4000);
}

void TransitionPanel::onApplySetup()
{
    const int idx = selectedMatch();
    if (idx >= 0) applyInitialSetup(matches_[(size_t)idx], true);
}

void TransitionPanel::onEditSetupTolerance()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Close Enough tolerances"));
    auto* root = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        tr("When CLOSE ENOUGH is enabled, these are the maximum accepted "
           "differences from the recorded pre-transition state. Tracks, "
           "transport, loops, and discrete FX states still match strictly."),
        &dialog);
    explanation->setWordWrap(true);
    root->addWidget(explanation);

    auto* form = new QFormLayout;
    auto* bpm = new QDoubleSpinBox(&dialog);
    bpm->setRange(0.05, 10.0);
    bpm->setDecimals(2);
    bpm->setSingleStep(0.1);
    bpm->setSuffix(tr(" BPM"));
    bpm->setValue(setupTolerances_.bpm);
    form->addRow(tr("BPM difference:"), bpm);

    auto* volume = new QDoubleSpinBox(&dialog);
    volume->setRange(1.5, 25.0);
    volume->setDecimals(1);
    volume->setSingleStep(0.5);
    volume->setSuffix(tr("%"));
    volume->setValue(setupTolerances_.volume * 100.0);
    form->addRow(tr("Volume/fader difference:"), volume);

    auto* eq = new QDoubleSpinBox(&dialog);
    eq->setRange(1.5, 25.0);
    eq->setDecimals(1);
    eq->setSingleStep(0.5);
    eq->setSuffix(tr("%"));
    eq->setValue(setupTolerances_.eq * 100.0);
    form->addRow(tr("LOW/MID/HIGH EQ difference:"), eq);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    setupTolerances_.bpm = bpm->value();
    setupTolerances_.volume = volume->value() / 100.0;
    setupTolerances_.eq = eq->value() / 100.0;
    QSettings settings;
    settings.setValue(QStringLiteral("transitions/closeEnoughBpm"),
                      setupTolerances_.bpm);
    settings.setValue(QStringLiteral("transitions/closeEnoughVolume"),
                      setupTolerances_.volume * 100.0);
    settings.setValue(QStringLiteral("transitions/closeEnoughEq"),
                      setupTolerances_.eq * 100.0);
    updateSetupStatus();
    updateControls();
    emit statusMessage(
        tr("Close Enough tolerances saved: ±%1 BPM, ±%2% volume, ±%3% EQ")
            .arg(setupTolerances_.bpm, 0, 'f', 2)
            .arg(setupTolerances_.volume * 100.0, 0, 'f', 1)
            .arg(setupTolerances_.eq * 100.0, 0, 'f', 1),
        5000);
}

void TransitionPanel::onRename()
{
    const int idx = selectedMatch();
    if (idx < 0) return;
    const GvtFile file = *matches_[(size_t)idx].file;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename transition"), tr("New transition name:"),
        QLineEdit::Normal, file.name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == file.name) return;
    QString error;
    const QString path = store_->renameTransition(file, name, &error);
    if (path.isEmpty()) {
        emit statusMessage(tr("Rename failed: %1").arg(error), 6000);
        return;
    }
    selectedPath_ = path;
    refreshMatches();
    emit statusMessage(tr("Renamed transition to “%1”").arg(name), 4000);
}

void TransitionPanel::onDelete()
{
    const int idx = selectedMatch();
    if (idx < 0) return;
    const GvtFile file = *matches_[(size_t)idx].file;
    if (QMessageBox::question(
            this, tr("Delete transition"),
            tr("Delete “%1”? This removes its .gvt file.").arg(file.name),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    QString error;
    selectedPath_.clear();
    if (!store_->deleteTransition(file, &error)) {
        emit statusMessage(tr("Delete failed: %1").arg(error), 6000);
        return;
    }
    emit statusMessage(tr("Deleted transition “%1”").arg(file.name), 4000);
}

void TransitionPanel::onLabelCue()
{
    const int idx = selectedMatch();
    const int row = preview_->currentRow();
    if (idx < 0 || row < 0) return;
    GvtFile file = *matches_[(size_t)idx].file;
    QTableWidgetItem* beatItem = preview_->item(row, 0);
    if (!beatItem) return;
    const int eventIndex = beatItem->data(Qt::UserRole).toInt();
    if (eventIndex < 0 || eventIndex >= (int)file.events.size()) return;
    const GvtEvent& event = file.events[(size_t)eventIndex];

    QString current = cueLabelAt(file, event.beat);
    if (current.isEmpty()) current = automaticCueLabel(event);
    bool ok = false;
    QString label = QInputDialog::getText(
        this, tr("Label transition cue"),
        tr("Waveform label at beat +%1 (leave empty to clear):")
            .arg(event.beat, 0, 'f', 3),
        QLineEdit::Normal, current, &ok).trimmed();
    if (!ok) return;
    if (label.contains(QLatin1Char('#')) || label.contains(QLatin1Char(';')) ||
        label.contains(QLatin1Char('\n')) || label.contains(QLatin1Char('\r'))) {
        emit statusMessage(tr("Cue labels cannot contain #, ;, or line breaks"),
                           5000);
        return;
    }

    file.cues.erase(
        std::remove_if(file.cues.begin(), file.cues.end(),
                       [beat = event.beat](const GvtCue& cue) {
                           return std::fabs(cue.beat - beat) < 0.0005;
                       }),
        file.cues.end());
    if (!label.isEmpty()) file.cues.push_back({event.beat, label});
    std::stable_sort(file.cues.begin(), file.cues.end(),
                     [](const GvtCue& a, const GvtCue& b) {
                         return a.beat < b.beat;
                     });

    QString error;
    selectedPath_ = file.filePath;
    if (!store_->update(file, &error)) {
        emit statusMessage(tr("Could not save cue label: %1").arg(error), 6000);
        return;
    }
    emit statusMessage(label.isEmpty() ? tr("Cleared transition cue label")
                                       : tr("Labeled cue “%1”").arg(label),
                       4000);
}

void TransitionPanel::onPerform()
{
    startReplay(tutorialViewOpen_ ? PlayerMode::Tutorial
                                  : PlayerMode::Perform,
                /*prime=*/false);
}

void TransitionPanel::onPrime()
{
    startReplay(tutorialViewOpen_ ? PlayerMode::Tutorial
                                  : PlayerMode::Perform,
                /*prime=*/true);
}

void TransitionPanel::startReplay(PlayerMode mode, bool prime)
{
    int idx = selectedMatch();
    if (idx < 0) {
        emit statusMessage(tr("Select a transition first"), 3000);
        return;
    }
    const Match& m = matches_[(size_t)idx];
    const bool incomingInitiallyPlaying =
        m.file->initialComplete && m.file->initialTo.captured &&
        m.file->initialTo.playing;
    const bool hasIncomingStartEvent = std::any_of(
        m.file->events.begin(), m.file->events.end(), [](const GvtEvent& event) {
            if (event.role != Role::ToDeck || event.value < 0.5)
                return false;
            return event.control == ControlId::Play ||
                   (event.control >= ControlId::HotCue1 &&
                    event.control <= ControlId::HotCue8) ||
                   (event.control >= ControlId::SavedLoop1 &&
                    event.control <= ControlId::SavedLoop8);
        });
    if (mode == PlayerMode::Perform && !incomingInitiallyPlaying &&
        !hasIncomingStartEvent) {
        emit statusMessage(
            tr("Can't perform “%1” accurately: the recording has no incoming "
               "PLAY, hot-cue, or CUSTOM-loop event. Re-record it with the current controls.")
                .arg(m.file->name),
            9000);
        return;
    }
    if (mode == PlayerMode::Tutorial) {
        const QStringList warnings = tutorialWarnings(m);
        if (!warnings.isEmpty()) {
            QMessageBox warningBox(
                QMessageBox::Warning, tr("Tutorial mapping warning"),
                tr("This recording cannot be reproduced exactly on the "
                   "current FLX4 setup:\n\n• %1\n\nYou can start anyway; "
                   "unavailable controls will be shown in amber.")
                    .arg(warnings.mid(0, 8).join(QStringLiteral("\n• "))),
                QMessageBox::Cancel | QMessageBox::Ok, this);
            warningBox.setDefaultButton(QMessageBox::Cancel);
            warningBox.button(QMessageBox::Ok)->setText(tr("START ANYWAY"));
            if (warningBox.exec() != QMessageBox::Ok) return;
        }
    }
    transitionPlayerSetMode(player_, mode);

    if (mode == PlayerMode::Perform) {
        takeoverTrackingActive_ = true;
        emit hardwareTakeoverTrackingStarted();
    }

    if (prime) {
        // PRIME leaves the outgoing song's live position alone, but restores
        // every recorded audible parameter and fully prepares the incoming
        // deck's transport before validating the result.
        applyInitialSetup(m, false, /*prepareFromTransport=*/false,
                          /*prepareToTransport=*/true);
        const QStringList issues = primeReadinessIssues(m);
        if (!issues.isEmpty()) {
            if (takeoverTrackingActive_) {
                takeoverTrackingActive_ = false;
                emit hardwareTakeoverTrackingFinished();
            }
            emit statusMessage(tr("Can't prime: %1")
                                   .arg(issues.mid(0, 4).join(QStringLiteral(", "))),
                               7000);
            updateControls();
            return;
        }
    } else {
        // PERFORM/TUTORIAL reconstruct the recorded pre-state while stopped.
        // Arm first, then start the outgoing deck, so beat-zero events cannot
        // be missed between the seek and scheduler activation.
        applyInitialSetup(m, false, /*prepareFromTransport=*/true,
                          /*prepareToTransport=*/true);
        if (TrackDataPtr track = engine_->deck(m.fromDeck).track()) {
            // Guided Perform starts eight beats before the entry whenever the
            // track has that much runway. This makes button countdowns useful
            // even for a recorded action at transition beat zero.
            const double startBeat = mode == PlayerMode::Tutorial
                                         ? std::max(0.0,
                                             m.file->anchorFromBeat - 8.0)
                                         : m.file->anchorFromBeat;
            engine_->deck(m.fromDeck).seekSec(
                track->secAtBeat(startBeat));
        }
    }

    QString error;
    if (!player_->arm(*m.file, m.fromDeck, /*startNow=*/false, &error)) {
        if (takeoverTrackingActive_) {
            takeoverTrackingActive_ = false;
            emit hardwareTakeoverTrackingFinished();
        }
        emit statusMessage(tr("Can't start: %1").arg(error), 6000);
        return;
    }
    tutorialActive_ = mode == PlayerMode::Tutorial;
    tutorialFromDeck_ = m.fromDeck;
    tutorialBeatsIn_ = 0.0;
    tutorialPrompts_.clear();
    if (tutorialActive_) {
        tutorialViewOpen_ = true;
        {
            QSignalBlocker block(tutorialBtn_);
            tutorialBtn_->setChecked(true);
        }
        ensureTutorialOverlay();
        const QStringList warnings = tutorialWarnings(m);
        tutorialGuideInstruction_ =
            tr("Get ready — the next FLX4 control will light up");
        tutorialGuideDetail_ =
            tr("Use the physical controller or click the highlighted virtual control.");
        tutorialGuideWarning_ = warnings.isEmpty()
            ? QString()
            : tr("%1 mapping warning(s); unavailable steps appear in amber.")
                  .arg(warnings.size());
        tutorialGuideFeedback_.clear();
        tutorialGuideLabel_->show();
        tutorialOverlay_->setWaiting(
            m.file->name,
            warnings.isEmpty()
                ? QString()
                : tr("%1 mapping warning(s); unavailable steps will appear in amber")
                      .arg(warnings.size()));
        layoutTutorialOverlay();
        tutorialOverlay_->show();
        tutorialOverlay_->raise();
        tutorialOverlay_->setFocus(Qt::OtherFocusReason);
        refreshTutorialLiveState();
        refreshTutorialGuideLabel();
    } else if (!tutorialViewOpen_) {
        closeTutorialOverlay();
    }
    if (!prime)
        bus_->dispatch({m.fromDeck, ControlId::Play, 1.0}, Origin::System);
    updateControls();
    if (prime)
        emit statusMessage(
            (mode == PlayerMode::Tutorial
                 ? tr("Tutorial primed: \"%1\" — guidance begins before deck %2 reaches beat %3")
                 : tr("Primed \"%1\" — full pre-state verified; fires when deck %2 reaches beat %3"))
                .arg(m.file->name)
                .arg(m.fromDeck == 0 ? QStringLiteral("A") : QStringLiteral("B"))
                .arg(m.file->anchorFromBeat, 0, 'f', 1),
            6000);
    else
        emit statusMessage(mode == PlayerMode::Tutorial
                               ? tr("Tutorial: \"%1\" — follow the prompts!").arg(m.file->name)
                               : tr("Performing \"%1\"…").arg(m.file->name),
                           4000);
}

void TransitionPanel::onAbort()
{
    if (player_->isActive()) {
        player_->abort();
        emit statusMessage(tr("Transition aborted"), 3000);
    }
    if (recorder_->isRecording()) {
        recorder_->cancel();
        capturedCount_ = 0;
        recIndicator_->clear();
        emit statusMessage(tr("Recording cancelled"), 3000);
    }
    progress_->setValue(0);
    if (banner_) banner_->hide();
    finishTutorialRun();
    updateControls();
}

void TransitionPanel::onProgress(double beatsIn, double beatsTotal)
{
    tutorialBeatsIn_ = beatsIn;
    if (tutorialActive_) {
        showNextTutorialPrompt();
        refreshTutorialGuideLabel();
    }
    if (takeoverTrackingActive_ && beatsTotal > 0.0 &&
        beatsIn >= beatsTotal) {
        // Arm pickup as soon as the final automatic event lands. Waiting for
        // the player's one-beat completion grace would leave a short window
        // where a mismatched hardware control could overwrite replay state.
        takeoverTrackingActive_ = false;
        emit hardwareTakeoverTrackingFinished();
    }
    if (beatsTotal <= 0.0) { progress_->setValue(0); return; }
    progress_->setValue(
        (int)std::lround(std::clamp(beatsIn / beatsTotal, 0.0, 1.0) * 1000.0));
}

void TransitionPanel::onFinished(bool completed)
{
    if (takeoverTrackingActive_) {
        takeoverTrackingActive_ = false;
        emit hardwareTakeoverTrackingFinished();
    }
    progress_->setValue(completed ? 1000 : 0);
    if (banner_) banner_->hide();
    finishTutorialRun();
    updateControls();
    emit statusMessage(completed ? tr("Transition complete")
                                 : tr("Transition stopped"),
                       3000);
}

void TransitionPanel::observeTutorialHardwareControl(
    const ControlEvent& event)
{
    if (tutorialViewOpen_ && tutorialOverlay_)
        tutorialOverlay_->setHardwareValue(event);
}

void TransitionPanel::observePerformancePadState(
    int deck, int mode, unsigned int enabledMask, unsigned int pressedMask)
{
    if (deck < 0 || deck >= 2)
        return;
    tutorialPadMode_[static_cast<std::size_t>(deck)] = mode;
    tutorialPadEnabledMask_[static_cast<std::size_t>(deck)] = enabledMask;
    tutorialPadPressedMask_[static_cast<std::size_t>(deck)] = pressedMask;
    if (tutorialViewOpen_)
        refreshTutorialLiveState();
}

void TransitionPanel::setHardwareTakeovers(
    const std::vector<SoftTakeoverState>& states)
{
    tutorialTakeovers_ = states;
    if (tutorialOverlay_)
        tutorialOverlay_->setTakeovers(tutorialTakeovers_);
    refreshTutorialGuideLabel();
}

void TransitionPanel::showBanner(const QString& text, const QColor& color,
                                 int timeoutMs)
{
    QWidget* host = window();
    if (!host) return;
    if (!banner_) {
        banner_ = new QLabel(host);
        banner_->setAlignment(Qt::AlignCenter);
        banner_->setWordWrap(true);
        banner_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    banner_->setStyleSheet(
        QStringLiteral("background:rgba(22,24,29,200); color:%1; "
                       "font-size:26px; font-weight:bold; border-radius:12px; "
                       "border:2px solid %1; padding:18px;")
            .arg(color.name()));
    banner_->setText(text);
    banner_->adjustSize();
    int w = std::max(banner_->width(), host->width() * 2 / 3);
    banner_->resize(w, banner_->heightForWidth(w) > 0
                           ? banner_->heightForWidth(w) + 36
                           : banner_->height());
    banner_->move((host->width() - banner_->width()) / 2,
                  host->height() / 6);
    banner_->raise();
    banner_->show();
    bannerTimer_->start(timeoutMs);
}

QString TransitionPanel::tutorialWarningForEvent(
    const Match& match, const GvtEvent& event) const
{
    QString control = QString::fromUtf8(controlName(event.control));
    control.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!tutorialMappingForEvent(event))
        return tr("%1 has no exact FLX4 control mapping").arg(control);

    if (event.gestureControl >= ControlId::PerformancePad1 &&
        event.gestureControl <= ControlId::PerformancePad8 &&
        (event.gesturePadMode == static_cast<int>(PerformancePadMode::Sampler) ||
         event.gesturePadMode == static_cast<int>(PerformancePadMode::SavedLoop))) {
        if (event.role == Role::Mixer)
            return tr("CUSTOM pad gesture has no deck target");
        const int pad = static_cast<int>(event.gestureControl) -
                        static_cast<int>(ControlId::PerformancePad1);
        const int deck = event.role == Role::FromDeck
                             ? match.fromDeck : 1 - match.fromDeck;
        const TrackDataPtr track = engine_->deck(deck).track();
        if (!track || !track->savedLoops[pad].isSet()) {
            return tr("Deck %1 CUSTOM pad %2 has no captured loop")
                .arg(deck == 0 ? QStringLiteral("A") : QStringLiteral("B"))
                .arg(pad + 1);
        }
    }

    if (event.control < ControlId::HotCue1 ||
        event.control > ControlId::HotCue8)
        return {};
    if (event.role == Role::Mixer)
        return tr("%1 has no deck target").arg(control);

    const int pad = static_cast<int>(event.control) -
                    static_cast<int>(ControlId::HotCue1);
    const bool fromRole = event.role == Role::FromDeck;
    const int deck = fromRole ? match.fromDeck : 1 - match.fromDeck;
    const QString deckName = deck == 0 ? QStringLiteral("A")
                                       : QStringLiteral("B");
    const TrackDataPtr track = engine_->deck(deck).track();
    if (!track || !std::isfinite(track->hotCues[pad]) ||
        track->hotCues[pad] < 0.0) {
        return tr("Deck %1 HOT CUE %2 is not assigned")
            .arg(deckName).arg(pad + 1);
    }

    const auto& recorded = fromRole ? match.file->fromHotCueBeats
                                    : match.file->toHotCueBeats;
    const double expectedBeat = recorded[static_cast<std::size_t>(pad)];
    if (!hotCueBeatIsMapped(expectedBeat)) {
        return tr("Deck %1 HOT CUE %2 is assigned, but this older recording "
                  "does not store its expected position")
            .arg(deckName).arg(pad + 1);
    }

    const double actualBeat = track->beatAtSec(track->hotCues[pad]);
    if (!std::isfinite(actualBeat) ||
        std::fabs(actualBeat - expectedBeat) > 0.05) {
        return tr("Deck %1 HOT CUE %2 is at beat %3; recording expects %4")
            .arg(deckName)
            .arg(pad + 1)
            .arg(actualBeat, 0, 'f', 2)
            .arg(expectedBeat, 0, 'f', 2);
    }
    return {};
}

QStringList TransitionPanel::tutorialWarnings(const Match& match) const
{
    QStringList warnings;
    QSet<QString> seen;
    for (const GvtEvent& event : match.file->events) {
        const QString warning = tutorialWarningForEvent(match, event);
        if (warning.isEmpty() || seen.contains(warning)) continue;
        seen.insert(warning);
        warnings.append(warning);
    }
    return warnings;
}

bool TransitionPanel::tutorialEventCanActivate(
    const Match& match, const GvtEvent& event) const
{
    if (!tutorialMappingForEvent(event)) return false;
    if (event.gestureControl >= ControlId::PerformancePad1 &&
        event.gestureControl <= ControlId::PerformancePad8 &&
        (event.gesturePadMode == static_cast<int>(PerformancePadMode::Sampler) ||
         event.gesturePadMode == static_cast<int>(PerformancePadMode::SavedLoop))) {
        if (event.role == Role::Mixer) return false;
        const int pad = static_cast<int>(event.gestureControl) -
                        static_cast<int>(ControlId::PerformancePad1);
        const int deck = event.role == Role::FromDeck
                             ? match.fromDeck : 1 - match.fromDeck;
        const TrackDataPtr track = engine_->deck(deck).track();
        if (!track || !track->savedLoops[pad].isSet()) return false;
    }
    if (event.control < ControlId::HotCue1 ||
        event.control > ControlId::HotCue8)
        return true;
    if (event.role == Role::Mixer) return false;

    const int pad = static_cast<int>(event.control) -
                    static_cast<int>(ControlId::HotCue1);
    const bool fromRole = event.role == Role::FromDeck;
    const int deck = fromRole ? match.fromDeck : 1 - match.fromDeck;
    const TrackDataPtr track = engine_->deck(deck).track();
    if (!track || !std::isfinite(track->hotCues[pad]) ||
        track->hotCues[pad] < 0.0)
        return false;

    const auto& recorded = fromRole ? match.file->fromHotCueBeats
                                    : match.file->toHotCueBeats;
    const double expectedBeat = recorded[static_cast<std::size_t>(pad)];
    // A legacy transition is allowed after warning when the pad exists; a
    // known mismatch is disabled because clicking it would teach the wrong cue.
    return !hotCueBeatIsMapped(expectedBeat) ||
           std::fabs(track->beatAtSec(track->hotCues[pad]) - expectedBeat) <= 0.05;
}

std::optional<Flx4TutorialMapping>
TransitionPanel::tutorialMappingForEvent(const GvtEvent& event) const
{
    if (event.gestureControl >= ControlId::PerformancePad1 &&
        event.gestureControl <= ControlId::PerformancePad8) {
        const int pad = static_cast<int>(event.gestureControl) -
                        static_cast<int>(ControlId::PerformancePad1);
        const Flx4PadMode mode = tutorialPadMode(event.gesturePadMode);
        if (mode == Flx4PadMode::None) return std::nullopt;
        return Flx4TutorialMapping {Flx4SurfaceControl::PerformancePad,
                                    pad, mode, false};
    }
    return flx4TutorialMapping(event.control, event.value);
}

ControlEvent TransitionPanel::tutorialPhysicalEvent(const GvtEvent& event) const
{
    ControlEvent physical;
    physical.deck = event.role == Role::Mixer
                        ? kNoDeck
                        : (event.role == Role::FromDeck
                               ? tutorialFromDeck_ : 1 - tutorialFromDeck_);
    physical.id = event.control;
    physical.value = event.control == ControlId::Crossfader &&
                             tutorialFromDeck_ == 1
                         ? 1.0 - event.value
                         : event.value;
    return physical;
}

QString TransitionPanel::tutorialInstruction(
    const GvtEvent& event, const ControlEvent& physical) const
{
    const QString deck = physical.deck == 0 ? tr("Deck A")
                         : physical.deck == 1 ? tr("Deck B") : tr("mixer");
    const int hotCue = static_cast<int>(event.control) -
                       static_cast<int>(ControlId::HotCue1) + 1;
    if (event.gestureControl >= ControlId::PerformancePad1 &&
        event.gestureControl <= ControlId::PerformancePad8 &&
        event.gesturePadMode >= 0 &&
        event.gesturePadMode < static_cast<int>(PerformancePadMode::Count)) {
        const int pad = static_cast<int>(event.gestureControl) -
                        static_cast<int>(ControlId::PerformancePad1) + 1;
        const auto mode = static_cast<PerformancePadMode>(event.gesturePadMode);
        const QString modeName =
            QLatin1String(performancePadModeLabel(mode));
        const bool release =
            (event.control == ControlId::Cue ||
             (event.control >= ControlId::HotCue1 &&
              event.control <= ControlId::HotCue8) ||
             (event.control >= ControlId::SavedLoop1 &&
              event.control <= ControlId::SavedLoop8)) &&
            event.value < 0.5;
        if (release)
            return tr("Release %1 pad %2 on %3")
                .arg(modeName).arg(pad).arg(deck);
        if (event.gesturePadMode >=
                static_cast<int>(PerformancePadMode::Keyboard) &&
            event.gesturePadMode <=
                static_cast<int>(PerformancePadMode::KeyShift)) {
            const QStringList baseButtons {
                tr("HOT CUE"), tr("PAD FX1"), tr("BEAT JUMP"), tr("CUSTOM")};
            const int shiftedIndex = event.gesturePadMode -
                static_cast<int>(PerformancePadMode::Keyboard);
            return tr("Hold SHIFT + %1 for %2, then press pad %3 on %4")
                .arg(baseButtons.value(shiftedIndex), modeName)
                .arg(pad).arg(deck);
        }
        return tr("Select %1, then press pad %2 on %3")
            .arg(modeName).arg(pad).arg(deck);
    }
    switch (event.control) {
    case ControlId::Play: return tr("Press PLAY/PAUSE on %1").arg(deck);
    case ControlId::Stop: return tr("Press PLAY/PAUSE to stop %1").arg(deck);
    case ControlId::Cue:
        return event.value >= 0.5 ? tr("Hold CUE on %1").arg(deck)
                                  : tr("Release CUE on %1").arg(deck);
    case ControlId::Load: return tr("Press LOAD on %1").arg(deck);
    case ControlId::TempoSync: return tr("Press BEAT SYNC on %1").arg(deck);
    case ControlId::HotCue1: case ControlId::HotCue2:
    case ControlId::HotCue3: case ControlId::HotCue4:
    case ControlId::HotCue5: case ControlId::HotCue6:
    case ControlId::HotCue7: case ControlId::HotCue8:
        return event.value >= 0.5
                   ? tr("Hold HOT CUE %1 on %2").arg(hotCue).arg(deck)
                   : tr("Release HOT CUE %1 on %2").arg(hotCue).arg(deck);
    case ControlId::SavedLoop1: case ControlId::SavedLoop2:
    case ControlId::SavedLoop3: case ControlId::SavedLoop4:
    case ControlId::SavedLoop5: case ControlId::SavedLoop6:
    case ControlId::SavedLoop7: case ControlId::SavedLoop8: {
        const int pad = static_cast<int>(event.control) -
                        static_cast<int>(ControlId::SavedLoop1) + 1;
        return event.value >= 0.5
                   ? tr("Hold CUSTOM loop %1 on %2").arg(pad).arg(deck)
                   : tr("Release CUSTOM loop %1 on %2").arg(pad).arg(deck);
    }
    case ControlId::LoopIn: return tr("Press LOOP IN on %1").arg(deck);
    case ControlId::LoopOut: return tr("Press LOOP OUT on %1").arg(deck);
    case ControlId::LoopExit: return tr("Press 4 BEAT/EXIT on %1").arg(deck);
    case ControlId::LoopHalve: return tr("Press LOOP 1/2 on %1").arg(deck);
    case ControlId::LoopDouble: return tr("Press LOOP 2× on %1").arg(deck);
    case ControlId::LoopAuto:
        return tr("Press 4 BEAT/EXIT on %1").arg(deck);
    case ControlId::BeatJump:
        return tr("Select BEAT JUMP mode, then jump %1 beats on %2")
            .arg(event.value, 0, 'f', 0).arg(deck);
    case ControlId::Tempo:
        return tr("Move %1 TEMPO to ×%2").arg(deck).arg(event.value, 0, 'f', 3);
    case ControlId::Fader:
        return tr("Move %1 channel fader to %2%").arg(deck)
            .arg(event.value * 100.0, 0, 'f', 0);
    case ControlId::Trim:
        return tr("Turn %1 TRIM to %2%").arg(deck)
            .arg(event.value * 100.0, 0, 'f', 0);
    case ControlId::EqLow: case ControlId::EqMid: case ControlId::EqHigh: {
        const QString band = event.control == ControlId::EqLow ? tr("LOW")
                             : event.control == ControlId::EqMid ? tr("MID")
                                                                 : tr("HIGH");
        return tr("Turn %1 %2 EQ to %3%").arg(deck, band)
            .arg(event.value * 100.0, 0, 'f', 0);
    }
    case ControlId::Filter:
        return tr("Turn %1 FILTER to %2%").arg(deck)
            .arg(event.value * 100.0, 0, 'f', 0);
    case ControlId::Crossfader:
        return tr("Move CROSSFADER to %1%").arg(
            physical.value * 100.0, 0, 'f', 0);
    case ControlId::HeadphoneCue:
        return tr("Press channel CUE on %1").arg(deck);
    case ControlId::MasterCue: return tr("Press MASTER CUE");
    case ControlId::HeadphoneMix:
        return tr("Turn HEADPHONES MIX to %1%").arg(
            event.value * 100.0, 0, 'f', 0);
    case ControlId::Quantize:
        return tr("Press SHIFT + channel CUE on %1").arg(deck);
    case ControlId::Jog:
        return tr("Nudge the jog-wheel rim on %1").arg(deck);
    case ControlId::PlatterScratch:
        return tr("Scratch the top platter on %1").arg(deck);
    case ControlId::PlatterTouch:
        return event.value >= 0.5
                   ? tr("Touch and hold the top platter on %1").arg(deck)
                   : tr("Release the top platter on %1").arg(deck);
    case ControlId::FxType: {
        const QString type = QStringList {tr("ECHO"), tr("REVERB"), tr("FLANGER")}
                                 .value(std::clamp((int)std::lround(event.value), 0, 2));
        return tr("Assign BEAT FX to %1 and select %2").arg(deck, type);
    }
    case ControlId::FxOn:
        return tr("Assign BEAT FX to %1 and turn FX %2")
            .arg(deck, event.value > 0.5 ? tr("ON") : tr("OFF"));
    case ControlId::FxWet:
        return tr("Set BEAT FX LEVEL/DEPTH for %1 to %2%").arg(deck)
            .arg(event.value * 100.0, 0, 'f', 0);
    case ControlId::FxBeats:
        return tr("Set BEAT FX timing for %1 to %2 beats").arg(deck)
            .arg(event.value, 0, 'f', 2);
    case ControlId::StemVocals: case ControlId::StemMelody:
    case ControlId::StemBass: case ControlId::StemDrums:
    case ControlId::BrowseSelect: case ControlId::BrowseNavigate:
    case ControlId::PerformancePadMode:
    case ControlId::PerformancePad1: case ControlId::PerformancePad2:
    case ControlId::PerformancePad3: case ControlId::PerformancePad4:
    case ControlId::PerformancePad5: case ControlId::PerformancePad6:
    case ControlId::PerformancePad7: case ControlId::PerformancePad8:
    case ControlId::Count:
        break;
    }
    QString name = QString::fromUtf8(controlName(event.control));
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return tr("No FLX4 gesture for %1").arg(name);
}

QString TransitionPanel::tutorialDetail(const GvtEvent& event) const
{
    QStringList parts;
    parts.append(event.role == Role::Mixer
                     ? tr("Mixer event")
                     : (event.role == Role::FromDeck ? tr("Outgoing track")
                                                     : tr("Incoming track")));
    const int idx = selectedMatch();
    if (idx >= 0) {
        QString label = cueLabelAt(*matches_[static_cast<std::size_t>(idx)].file,
                                   event.beat);
        if (label.isEmpty()) label = automaticCueLabel(event);
        if (!label.isEmpty()) parts.append(label);
    }
    if (event.gestureControl >= ControlId::PerformancePad1 &&
        event.gestureControl <= ControlId::PerformancePad8) {
        parts.append(tr("recorded button gesture preserved"));
    }
    parts.append(tr("physical FLX4 or highlighted virtual control"));
    return parts.join(QStringLiteral(" · "));
}

void TransitionPanel::ensureTutorialOverlay()
{
    if (tutorialOverlay_) return;
    QWidget* host = window();
    if (!host) return;
    tutorialOverlay_ = new Flx4TutorialWidget(host);
    connect(tutorialOverlay_, &Flx4TutorialWidget::controlActivated, this,
            [this](const ControlEvent& event) {
                bus_->dispatch(event, Origin::Ui);
            });
    connect(tutorialOverlay_, &Flx4TutorialWidget::performancePadActivated,
            this, &TransitionPanel::tutorialPerformancePadRequested);
    connect(tutorialOverlay_, &Flx4TutorialWidget::abortRequested, this,
            [this] {
                // The overlay's ×/Esc closes the view. If a guided transition
                // is running, abort it first rather than leaving invisible
                // tutorial scoring active in the background.
                tutorialBtn_->setChecked(false);
            });
    tutorialOverlay_->setTakeovers(tutorialTakeovers_);
    refreshTutorialLiveState();
}

void TransitionPanel::layoutTutorialOverlay()
{
    if (!tutorialOverlay_) return;
    QWidget* host = window();
    if (!host) return;
    if (!tutorialLeftPane_ || !tutorialPreviewPane_) return;

    // Cover exactly the transition list + event sequence horizontally. Extend
    // from their top through the library below, leaving the right-hand
    // Perform/Prime controls visible and usable as the tutorial-mode controls.
    const QPoint topLeft = tutorialLeftPane_->mapTo(host, QPoint(0, 0));
    const QPoint previewRight = tutorialPreviewPane_->mapTo(
        host, QPoint(tutorialPreviewPane_->width(), 0));
    const int bottom = std::max(topLeft.y() + 260, host->height() - 30);
    QRect geometry(topLeft.x(), topLeft.y(),
                   std::max(420, previewRight.x() - topLeft.x()),
                   bottom - topLeft.y());
    geometry = geometry.intersected(host->rect().adjusted(4, 4, -4, -4));
    tutorialOverlay_->setGeometry(geometry);
    tutorialOverlay_->raise();
}

void TransitionPanel::onTutorialViewToggled(bool open)
{
    if (open) {
        if (player_->isActive() && !tutorialActive_) {
            QSignalBlocker block(tutorialBtn_);
            tutorialBtn_->setChecked(false);
            emit statusMessage(
                tr("Finish or abort the current Perform before opening Tutorial view"),
                4000);
            return;
        }
        tutorialViewOpen_ = true;
        tutorialGuideLabel_->show();
        refreshTutorialView();
        emit statusMessage(
            tr("Tutorial view open — use Perform for an 8-beat count-in or Prime to arm guidance"),
            5000);
    } else {
        tutorialViewOpen_ = false;
        if (tutorialActive_ && player_->isActive())
            player_->abort();
        closeTutorialOverlay();
    }
    updateControls();
}

void TransitionPanel::refreshTutorialView()
{
    if (!tutorialViewOpen_) return;
    ensureTutorialOverlay();
    if (!tutorialOverlay_) return;

    const int idx = selectedMatch();
    if (idx < 0) {
        tutorialGuideInstruction_ = tr("Select a transition");
        tutorialGuideDetail_ =
            tr("The full FLX4 surface mirrors live controls and LEDs here.");
        tutorialGuideWarning_ =
            tr("No transition is selected for guided Perform or Prime.");
        tutorialOverlay_->setWaiting(
            tr("No transition selected"),
            tr("Select a transition to prepare guided Perform or Prime"));
    } else {
        const Match& match = matches_[static_cast<std::size_t>(idx)];
        const QStringList warnings = tutorialWarnings(match);
        tutorialGuideInstruction_ =
            tr("Ready for guided Perform or Prime: %1").arg(match.file->name);
        tutorialGuideDetail_ =
            tr("The virtual FLX4 mirrors the live deck state. The next required control will pulse.");
        tutorialGuideWarning_ = warnings.isEmpty()
            ? QString()
            : tr("%1 mapping warning(s); unavailable steps appear in amber.")
                  .arg(warnings.size());
        tutorialOverlay_->setWaiting(
            match.file->name,
            warnings.isEmpty()
                ? QString()
                : tr("%1 mapping warning(s); unavailable steps will appear in amber")
                      .arg(warnings.size()));
    }
    tutorialGuideFeedback_.clear();
    refreshTutorialLiveState();
    refreshTutorialGuideLabel();
    layoutTutorialOverlay();
    tutorialOverlay_->show();
    tutorialOverlay_->raise();
}

void TransitionPanel::finishTutorialRun()
{
    tutorialActive_ = false;
    tutorialPrompts_.clear();
    if (tutorialViewOpen_)
        refreshTutorialView();
    else if (tutorialOverlay_)
        tutorialOverlay_->hide();
}

void TransitionPanel::closeTutorialOverlay()
{
    tutorialActive_ = false;
    tutorialViewOpen_ = false;
    tutorialPrompts_.clear();
    if (tutorialBtn_) {
        QSignalBlocker block(tutorialBtn_);
        tutorialBtn_->setChecked(false);
    }
    if (tutorialGuideLabel_) tutorialGuideLabel_->hide();
    if (tutorialOverlay_) tutorialOverlay_->hide();
}

void TransitionPanel::refreshTutorialLiveState()
{
    if (!tutorialOverlay_)
        return;

    Flx4TutorialLiveState state;
    for (int deck = 0; deck < 2; ++deck) {
        const Deck& source = engine_->deck(deck);
        state.tempo[deck] = source.tempoRatio.load();
        state.fader[deck] = source.fader.load();
        state.trim[deck] = source.trim.load();
        state.eqHigh[deck] = source.eqHigh.load();
        state.eqMid[deck] = source.eqMid.load();
        state.eqLow[deck] = source.eqLow.load();
        state.filter[deck] = source.filter.load();
        state.level[deck] = source.channelLevel.load();
        state.playing[deck] = source.playing.load();
        state.cueSet[deck] = source.cuePointSec.load() >= 0.0;
        state.loopActive[deck] = source.loopActive.load();
        state.channelCue[deck] = engine_->headphoneCue[deck].load();
        state.quantize[deck] = source.quantizeHotCues.load();
        state.fxOn[deck] = source.fxOn.load();
        state.padMode[deck] = tutorialPadMode_[deck];
        state.padEnabledMask[deck] = tutorialPadEnabledMask_[deck];
        state.padPressedMask[deck] = tutorialPadPressedMask_[deck];
    }
    state.masterCue = engine_->masterCue.load();
    state.headphoneMix = engine_->headphoneMix.load();
    state.crossfader = engine_->crossfader.load();
    state.fxWet = state.fxOn[1] ? engine_->deck(1).fxWet.load()
                                : engine_->deck(0).fxWet.load();
    tutorialOverlay_->setLiveState(state);
    tutorialOverlay_->setTakeovers(tutorialTakeovers_);
}

void TransitionPanel::refreshTutorialGuideLabel()
{
    if (!tutorialGuideLabel_ || !tutorialViewOpen_) {
        if (tutorialGuideLabel_) tutorialGuideLabel_->hide();
        return;
    }

    QStringList lines;
    QString first = tutorialGuideInstruction_.isEmpty()
        ? tr("FLX4 Tutorial view") : tutorialGuideInstruction_;
    if (tutorialActive_ && !tutorialPrompts_.empty()) {
        const double remaining =
            tutorialPrompts_.front().beat - tutorialBeatsIn_;
        first += remaining > 0.05
            ? tr("  ·  IN %1 BEATS").arg(std::max(0.0, remaining), 0, 'f', 1)
            : tr("  ·  NOW");
    }
    lines.append(first);
    if (!tutorialGuideDetail_.isEmpty())
        lines.append(tutorialGuideDetail_);
    if (!tutorialTakeovers_.empty()) {
        lines.append(tr("⚠ Reset the %1 highlighted FLX4 control(s). They stay monitored until every control is correct at the same time.")
                         .arg(tutorialTakeovers_.size()));
    } else if (!tutorialGuideWarning_.isEmpty()) {
        lines.append(QStringLiteral("⚠ ") + tutorialGuideWarning_);
    }
    if (!tutorialGuideFeedback_.isEmpty())
        lines.append(tutorialGuideFeedback_);

    const QColor border = !tutorialTakeovers_.empty() ||
                                  !tutorialGuideWarning_.isEmpty()
                              ? QColor(0xe8, 0xa8, 0x35)
                              : (!tutorialGuideFeedback_.isEmpty()
                                     ? tutorialGuideFeedbackColor_
                                     : transitionEntryColor());
    tutorialGuideLabel_->setStyleSheet(
        QStringLiteral("background:#20242b; color:%1; border:1px solid %2; "
                       "border-radius:4px; padding:5px; font-weight:bold;")
            .arg(themeText().name(), border.name()));
    tutorialGuideLabel_->setText(lines.join(QLatin1Char('\n')));
    tutorialGuideLabel_->show();
}

void TransitionPanel::showNextTutorialPrompt()
{
    if (!tutorialActive_ || !tutorialOverlay_)
        return;
    if (tutorialPrompts_.empty()) {
        tutorialOverlay_->clearExpected();
        tutorialGuideInstruction_ =
            tr("Good — watch for the next highlighted control");
        tutorialGuideDetail_ =
            tr("Keep the outgoing track running and follow the recorded sequence.");
        tutorialGuideWarning_.clear();
        refreshTutorialGuideLabel();
        return;
    }
    const GvtEvent& event = tutorialPrompts_.front();
    const int idx = selectedMatch();
    if (idx < 0) return;
    const Match& match = matches_[static_cast<std::size_t>(idx)];
    const ControlEvent physical = tutorialPhysicalEvent(event);
    tutorialGuideInstruction_ = tutorialInstruction(event, physical);
    tutorialGuideDetail_ = tutorialDetail(event);
    tutorialGuideWarning_ = tutorialWarningForEvent(match, event);
    tutorialOverlay_->setExpected(
        physical, tutorialGuideInstruction_, tutorialGuideDetail_,
        event.beat - tutorialBeatsIn_,
        tutorialEventCanActivate(match, event),
        tutorialGuideWarning_,
        tutorialMappingForEvent(event), event.gesturePadMode);
    refreshTutorialGuideLabel();
}

void TransitionPanel::onTutorialPrompt(const GvtEvent& e, double beatsAhead)
{
    // The prompt is emitted before the same timer tick's progress signal, so
    // seed the countdown from its authoritative lead instead of flashing NOW.
    tutorialBeatsIn_ = e.beat - beatsAhead;
    const auto sameEvent = [&e](const GvtEvent& queued) {
        return queued.role == e.role && queued.control == e.control &&
               std::fabs(queued.beat - e.beat) < 0.0005 &&
               std::fabs(queued.value - e.value) < 0.0005;
    };
    if (std::none_of(tutorialPrompts_.begin(), tutorialPrompts_.end(), sameEvent)) {
        tutorialPrompts_.push_back(e);
        std::stable_sort(tutorialPrompts_.begin(), tutorialPrompts_.end(),
                         [](const GvtEvent& a, const GvtEvent& b) {
                             return a.beat < b.beat;
                         });
    }
    showNextTutorialPrompt();
}

void TransitionPanel::onTutorialScored(const GvtEvent& e, double beatError,
                                       double valueError)
{
    const double abs = std::fabs(beatError);
    QColor color;
    QString verdict;
    if (abs < 0.25 && valueError < 0.08) {
        color = QColor(0x4c, 0xd9, 0x64); verdict = tr("nice!");
    } else if (abs < 1.0 && valueError < 0.2) {
        color = QColor(0xe8, 0xa8, 0x35); verdict = tr("close");
    }
    else { color = QColor(0xe8, 0x55, 0x55); verdict = tr("off beat"); }
    const auto sameEvent = [&e](const GvtEvent& queued) {
        return queued.role == e.role && queued.control == e.control &&
               std::fabs(queued.beat - e.beat) < 0.0005 &&
               std::fabs(queued.value - e.value) < 0.0005;
    };
    tutorialPrompts_.erase(
        std::remove_if(tutorialPrompts_.begin(), tutorialPrompts_.end(), sameEvent),
        tutorialPrompts_.end());
    if (tutorialOverlay_) {
        tutorialGuideFeedback_ =
            QStringLiteral("%1 %2%3 beats · %4")
                .arg(QString::fromUtf8(controlName(e.control)),
                     beatError >= 0 ? QStringLiteral("+")
                                    : QStringLiteral("−"),
                     QString::number(abs, 'f', 2), verdict);
        tutorialGuideFeedbackColor_ = color;
        tutorialOverlay_->setFeedback(tutorialGuideFeedback_, color);
        showNextTutorialPrompt();
    }
}

} // namespace gvt
