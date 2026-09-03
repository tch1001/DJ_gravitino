#pragma once
#include <QList>
#include <QStringList>
#include <QWidget>
#include <array>
#include "../control/ControlBus.h"
#include "../audio/AudioEngine.h"
#include "../performance/PerformancePads.h"

class QComboBox;
class QCheckBox;
class QDial;
class QLabel;
class QPoint;
class QPointF;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;

namespace gvt {

enum class BeatGridCommand : int {
    SetDownbeat,
    Nudge,
    HalveBpm,
    DoubleBpm,
    SetBpm,
};

// Compact whole-track overview waveform (~44 px): peak bars, played tint,
// playhead, numbered hotcue flags in slot colors, cue point and
// transition-entry markers. The beat grid belongs to the detailed waveform;
// click this overview to seek.
class WaveformView : public QWidget {
    Q_OBJECT
public:
    WaveformView(int deckIndex, Deck* deck, QWidget* parent = nullptr);
    void setTransitionEntry(double sec); // sec < 0 = none
    void setTransitionCues(const QList<double>& seconds,
                           const QStringList& labels);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    int deckIndex_;
    Deck* deck_;
    double transitionEntrySec_ = -1.0;
    QList<double> transitionCueSecs_;
    QStringList transitionCueLabels_;
};

// Mouse-operable platter. Its marker follows the track position at a vinyl
// 33⅓ RPM visual rate; dragging dispatches the same touch/scratch controls as
// the hardware platter so the engine remains the single source of behavior.
class JogWheelWidget : public QWidget {
public:
    JogWheelWidget(int deckIndex, ControlBus* bus,
                   QWidget* parent = nullptr);
    void setPositionSec(double positionSec, bool trackAvailable);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    double pointerAngle(const QPointF& position) const;
    void applyDrag(const QPointF& position);
    void dispatch(ControlId id, double value);

    int deckIndex_ = 0;
    ControlBus* bus_ = nullptr;
    double rotationDegrees_ = 0.0;
    double lastPointerAngle_ = 0.0;
    bool trackAvailable_ = false;
    bool dragging_ = false;
};

// One deck panel (Serato-style): overview waveform, track info lines,
// PLAY/CUE/SYNC transport, 2x4 hot-cue pads, and a narrow vertical tempo
// slider on the outer edge (left for deck A, right for deck B).
// All user actions go through the ControlBus (Origin::Ui) except the
// documented direct-API calls (seek, load, hotcue clear).
class DeckWidget : public QWidget {
    Q_OBJECT
public:
    DeckWidget(int deckIndex, ControlBus* bus, AudioEngine* engine,
               QWidget* parent = nullptr);

    // Relay from MainWindow::setTransitionEntryMarker; sec < 0 = none.
    void setTransitionEntry(double sec);
    void setTransitionCues(const QList<double>& seconds,
                           const QStringList& labels);
    void setTemporaryTransitionCues(
        const std::array<double, 8>& startSeconds,
        const std::array<double, 8>& endSeconds,
        const QStringList& labels, const QStringList& colors);
    void clearTemporaryTransitionCues();

    // Selects the virtual performance-pad layer. Hardware integration can
    // call this later without duplicating the pad execution/configuration UI.
    void setPerformancePadMode(PerformancePadMode mode);
    PerformancePadMode performancePadMode() const { return padMode_; }
    void triggerPerformancePad(PerformancePadMode mode, int pad, bool pressed);
    // Routes all removal paths through MainWindow so transitions that depend
    // on this pad can be identified before the cue is actually cleared.
    void requestHotCueClear(int pad);
    void clearHotCue(int pad);
    unsigned int performancePadLedMask(PerformancePadMode mode) const;
    unsigned int performancePadPressedMask() const;
    void beatGridChanged();
    void performanceMetadataChanged();
    QWidget* controlWidget(ControlId control) const;

    // Stems row state machine (driven by MainWindow from StemSeparator
    // signals). Idle: [STEMS] request button armed, pads disabled.
    // InProgress: button disabled, `stage` text shown. Ready: pads enabled,
    // mirroring the deck's stem* atomics on the 30 Hz refresh.
    void setStemsIdle();
    void setStemsInProgress(const QString& stage);
    void setStemsReady();

signals:
    // [STEMS] pressed — MainWindow kicks StemSeparator for this deck's track.
    void stemsRequested(int deck);
    void performancePadStateChanged(int deck, int mode,
                                    unsigned int enabledMask,
                                    unsigned int pressedMask);
    void trackPerformanceMetadataChanged(int deck);
    void beatGridEditRequested(int deck, gvt::BeatGridCommand command,
                               double value);
    void hotCueRemovalRequested(int deck, int pad);

public slots:
    void trackChanged();   // call after a track (un)load to refresh labels

private slots:
    void onBusEvent(const gvt::ControlEvent& e, gvt::Origin origin);
    void refresh();        // ~30 Hz UI poll
    void onTempoSlider(int value);

private:
    void dispatch(ControlId id, double value = 1.0);
    void syncHotCueButtons();
    void syncLoopButtons();
    void loadPerformancePadSettings();
    void savePerformancePadAssignment(PerformancePadMode mode, int pad);
    void savePerformancePadMode();
    void syncPerformancePadUi();
    void handlePerformancePad(int pad, bool pressed);
    void dispatchPerformancePadGesture(PerformancePadMode mode, int pad);
    void configurePerformancePad(int pad, const QPoint& position);
    void beginPadFx(int pad, const PerformancePadAssignment& assignment);
    void endPadFx(int pad);
    void showPadFeedback(const QString& text);
    void updateTempoRangeUi();
    bool canDragHotCueToPlay(int pad) const;
    void updateHotCuePlayDropTarget(int pad, const QPoint& globalPosition);
    void finishHotCuePlayDrag(int pad, const QPoint& globalPosition);
    void setPlayDropTargetVisible(bool visible);

    int deckIndex_;
    ControlBus* bus_;
    AudioEngine* engine_;

    WaveformView* waveform_ = nullptr;
    JogWheelWidget* jogWheel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* artistLabel_ = nullptr;
    QLabel* bpmLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* tempoLabel_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QPushButton* cueBtn_ = nullptr;
    QPushButton* syncBtn_ = nullptr;
    QPushButton* quantizeBtn_ = nullptr;
    QCheckBox* preservePitchCheck_ = nullptr;
    QToolButton* gridBtn_ = nullptr;
    QToolButton* tempoRangeBtn_ = nullptr;
    QSlider* tempoSlider_ = nullptr;
    QPushButton* hotcueBtns_[8] = {};

    // FLX4-style performance pad section. The hardware sampler bank is exposed
    // as CUSTOM and can own per-track captured loops or assigned audio in one
    // visible layer. The four SHIFT modes live in shiftedModesBtn_'s menu.
    // SavedLoop remains an internal settings/API compatibility alias.
    static constexpr int kPerformanceModeCount =
        static_cast<int>(PerformancePadMode::Count);
    PerformancePadMode padMode_ = PerformancePadMode::HotCue;
    QPushButton* normalModeBtns_[4] = {};
    QToolButton* shiftedModesBtn_ = nullptr;
    QLabel* padStatusLabel_ = nullptr;
    std::array<std::array<PerformancePadAssignment, kPerformancePadCount>,
               kPerformanceModeCount> padAssignments_ {};
    std::array<PerformancePadMode, kPerformancePadCount> pressedPadModes_ {};
    std::array<bool, kPerformancePadCount> padIsPressed_ {};
    std::array<bool, kPerformancePadCount> padReleasePending_ {};
    bool temporaryTransitionCuesActive_ = false;
    std::array<double, kPerformancePadCount> temporaryTransitionCueSecs_ {
        -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    std::array<double, kPerformancePadCount> temporaryTransitionLoopEndSecs_ {
        -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    QStringList temporaryTransitionCueLabels_;
    QStringList temporaryTransitionCueColors_;
    int padFeedbackSerial_ = 0;
    bool playDropTargetVisible_ = false;

    struct PadFxSnapshot {
        bool valid = false;
        int pad = -1;
        int type = 0;
        bool on = false;
        double wet = 0.5;
        double beats = 0.5;
    } padFxSnapshot_;

    // Loop / beat-jump row (below the hot cues).
    static constexpr double kAutoLoopBeats[5] = {0.5, 1, 2, 4, 8};
    QPushButton* autoLoopBtns_[5] = {};
    QPushButton* loopInBtn_ = nullptr;
    QPushButton* loopOutBtn_ = nullptr;
    QPushButton* loopExitBtn_ = nullptr;
    // Cached loop-highlight state so refresh() only restyles on change:
    // -2 = none, -1 = manual/non-standard length, 0..4 = kAutoLoopBeats idx.
    int shownLoopLenIdx_ = -2;
    bool shownLoopIn_ = false, shownLoopActive_ = false;

    // FX strip (below the loop/jump row): type combo, ON toggle, WET dial,
    // BEATS halve/double. Mirrors the deck's fx* atomics in refresh().
    void syncFxControls();
    QComboBox* fxTypeCombo_ = nullptr;
    QPushButton* fxOnBtn_ = nullptr;
    QDial* fxWetDial_ = nullptr;
    QLabel* fxBeatsLabel_ = nullptr;
    int shownFxOn_ = -1; // -1 = unstyled yet, else 0/1

    // STEMS row (below the FX row): [STEMS] request button, four checkable
    // pads (vocal/melody/bass/drums, Serato colors), status label.
    enum class StemsState { Idle, InProgress, Ready };
    void syncStemPads();
    StemsState stemsState_ = StemsState::Idle;
    QPushButton* stemsBtn_ = nullptr;
    QPushButton* stemPads_[4] = {};
    QLabel* stemsStatusLabel_ = nullptr;

    QTimer* timer_ = nullptr;
    double lastWaveformPos_ = -1.0;
};

} // namespace gvt
