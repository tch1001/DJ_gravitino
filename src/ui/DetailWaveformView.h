#pragma once
#include <QList>
#include <QStringList>
#include <QWidget>
#include "../audio/AudioEngine.h"

class QTimer;
class QToolButton;

namespace gvt {

// Serato-style center waveform: two horizontally-scrolling zoomed lanes
// (deck A on top, deck B below). The playhead is a fixed vertical line at
// the horizontal center of the widget; the waveform scrolls beneath it.
// Band-colored rendering from TrackData::overviewLow/Mid/High (falls back
// to gray overviewPeaks), beatgrid ticks, numbered hot-cue flags in slot
// colors, cue point marker, and an orange transition-entry marker.
// Click in a lane to seek that deck. Scroll wheel / +- buttons zoom
// (4..30 s visible, default 8 s).
class DetailWaveformView : public QWidget {
    Q_OBJECT
public:
    explicit DetailWaveformView(AudioEngine* engine,
                                QWidget* parent = nullptr);

    // sec < 0 clears the marker for that deck.
    void setTransitionEntry(int deck, double sec);
    void setTransitionCues(int deck, const QList<double>& seconds,
                           const QStringList& labels);

public slots:
    void zoomIn();
    void zoomOut();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void drawLane(QPainter& p, const QRect& r, int deck);
    void setWindowSec(double sec);
    QRect laneRect(int deck) const;

    AudioEngine* engine_;
    double windowSec_ = 8.0;              // seconds visible across the width
    double transitionEntrySec_[2] = {-1.0, -1.0};
    QList<double> transitionCueSecs_[2];
    QStringList transitionCueLabels_[2];
    double lastPaintPos_[2] = {-1.0, -1.0};
    double lastTempoRatio_[2] = {-1.0, -1.0};
    const void* lastTrack_[2] = {nullptr, nullptr};
    // Last-painted loop state, so loop edits repaint even while paused.
    double lastLoopStart_[2] = {-2.0, -2.0};
    double lastLoopEnd_[2] = {-2.0, -2.0};
    bool lastLoopActive_[2] = {false, false};
    // Mouse scratch state. The waveform is treated like a strip being grabbed:
    // dragging it right moves the playhead earlier, and left moves it later.
    int scratchDeck_ = -1;
    double scratchPressX_ = 0.0;
    double scratchAnchorSec_ = 0.0;
    double scratchLastSec_ = 0.0;
    bool scratchMoved_ = false;
    const void* scratchTrack_ = nullptr;
    QTimer* timer_ = nullptr;
    QToolButton* zoomInBtn_ = nullptr;
    QToolButton* zoomOutBtn_ = nullptr;
};

} // namespace gvt
