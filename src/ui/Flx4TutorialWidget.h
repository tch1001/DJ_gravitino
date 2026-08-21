#pragma once

#include "../control/ControlBus.h"
#include "../midi/Flx4TutorialMap.h"
#include "../midi/SoftTakeover.h"

#include <QColor>
#include <QRectF>
#include <QWidget>
#include <array>
#include <optional>
#include <vector>

namespace gvt {

struct Flx4TutorialLiveState {
    std::array<double, 2> tempo {1.0, 1.0};
    std::array<double, 2> fader {1.0, 1.0};
    std::array<double, 2> trim {0.5, 0.5};
    std::array<double, 2> eqHigh {0.5, 0.5};
    std::array<double, 2> eqMid {0.5, 0.5};
    std::array<double, 2> eqLow {0.5, 0.5};
    std::array<double, 2> filter {0.5, 0.5};
    std::array<double, 2> level {};
    std::array<bool, 2> playing {};
    std::array<bool, 2> cueSet {};
    std::array<bool, 2> loopActive {};
    std::array<bool, 2> channelCue {};
    std::array<bool, 2> quantize {true, true};
    std::array<bool, 2> fxOn {};
    std::array<int, 2> padMode {};
    std::array<unsigned int, 2> padEnabledMask {};
    std::array<unsigned int, 2> padPressedMask {};
    bool masterCue = false;
    double headphoneMix = 0.0;
    double crossfader = 0.5;
    double fxWet = 0.5;
};

// Full-surface DDJ-FLX4 teaching overlay. The recorded action's physical
// control pulses on the diagram; clicking that highlighted control performs
// the exact expected event, while physical MIDI input continues to work.
class Flx4TutorialWidget final : public QWidget {
    Q_OBJECT
public:
    explicit Flx4TutorialWidget(QWidget* parent = nullptr);

    void setWaiting(const QString& transitionName, const QString& warning);
    void setExpected(const ControlEvent& event, const QString& instruction,
                     const QString& detail, double beatsAhead,
                     bool activationEnabled, const QString& warning,
                     const std::optional<Flx4TutorialMapping>& mapping,
                     int gesturePadMode = -1);
    void updateCountdown(double beatsAhead);
    void setFeedback(const QString& text, const QColor& color);
    void clearExpected();
    void setHardwareValue(const ControlEvent& event);
    void setLiveState(const Flx4TutorialLiveState& state);
    void setTakeovers(const std::vector<SoftTakeoverState>& states);

signals:
    void controlActivated(const gvt::ControlEvent& event);
    void performancePadActivated(int deck, int mode, int pad, bool pressed);
    void abortRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QRectF targetRect(const Flx4TutorialMapping& mapping, int deck) const;
    QRectF fxAssignmentRect(int deck) const;
    QPointF toDesign(const QPointF& point) const;
    QRectF fittedDesignRect() const;

    std::optional<ControlEvent> expected_;
    std::optional<Flx4TutorialMapping> mapping_;
    QString transitionName_;
    QString instruction_;
    QString detail_;
    QString warning_;
    QString feedback_;
    QColor feedbackColor_;
    double beatsAhead_ = 0.0;
    bool activationEnabled_ = false;
    int gesturePadMode_ = -1;
    bool pulse_ = false;
    int animationPhase_ = 0;
    std::optional<double> hardwareValue_;
    Flx4TutorialLiveState live_;
    std::vector<SoftTakeoverState> takeovers_;
};

} // namespace gvt
