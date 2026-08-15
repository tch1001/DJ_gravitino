#pragma once
#include <QWidget>
#include "../control/ControlBus.h"
#include "../audio/AudioEngine.h"

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace gvt {

// Compact whole-track overview waveform (~44 px): peak bars, played tint,
// playhead, beatgrid ticks, numbered hotcue flags in slot colors, cue point
// and transition-entry markers. Click to seek.
class WaveformView : public QWidget {
    Q_OBJECT
public:
    WaveformView(int deckIndex, Deck* deck, QWidget* parent = nullptr);
    void setTransitionEntry(double sec); // sec < 0 = none
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    int deckIndex_;
    Deck* deck_;
    double transitionEntrySec_ = -1.0;
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

    int deckIndex_;
    ControlBus* bus_;
    AudioEngine* engine_;

    WaveformView* waveform_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* artistLabel_ = nullptr;
    QLabel* bpmLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* tempoLabel_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QPushButton* cueBtn_ = nullptr;
    QPushButton* syncBtn_ = nullptr;
    QSlider* tempoSlider_ = nullptr;
    QPushButton* hotcueBtns_[8] = {};

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

    QTimer* timer_ = nullptr;
};

} // namespace gvt
