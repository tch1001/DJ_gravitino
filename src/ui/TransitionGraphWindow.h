// Separate set-planning window: songs are force-directed nodes and recorded
// transitions are directed edges with timing-aware route previews on hover.
#pragma once

#include <QMainWindow>
#include <QSlider>

namespace gvt {

class AudioEngine;
class TransitionStore;

class TransitionGraphWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit TransitionGraphWindow(TransitionStore* store, AudioEngine* engine,
                                   QWidget* parent = nullptr);

    void refreshGraph();

private:
    void refreshDeckHighlights();

    class Canvas;
    TransitionStore* store_ = nullptr;
    AudioEngine* engine_ = nullptr;
    Canvas* canvas_ = nullptr;
    QSlider* zoomSlider_ = nullptr;
};

} // namespace gvt
