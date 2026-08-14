#pragma once
#include <QWidget>
#include "../control/ControlBus.h"
#include "../audio/AudioEngine.h"

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace gvt {

// Custom-painted track overview: peak bars, played tint, playhead, beatgrid
// ticks, numbered hotcue flags. Click to seek.
class WaveformView : public QWidget {
    Q_OBJECT
public:
    WaveformView(int deckIndex, Deck* deck, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    int deckIndex_;
    Deck* deck_;
};

// One deck: waveform, track labels, transport, sync, tempo slider, hotcues.
// All user actions go through the ControlBus (Origin::Ui) except the
// documented direct-API calls (seek, load, hotcue set/jump).
class DeckWidget : public QWidget {
    Q_OBJECT
public:
    DeckWidget(int deckIndex, ControlBus* bus, AudioEngine* engine,
               QWidget* parent = nullptr);

public slots:
    void trackChanged();   // call after a track (un)load to refresh labels

private slots:
    void onBusEvent(const gvt::ControlEvent& e, gvt::Origin origin);
    void refresh();        // ~30 Hz UI poll
    void onTempoSlider(int value);
    void onHotCueClicked(int i);

private:
    void dispatch(ControlId id, double value = 1.0);
    void syncHotCueButtons();

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
    QTimer* timer_ = nullptr;
};

} // namespace gvt
