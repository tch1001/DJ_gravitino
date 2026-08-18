#pragma once

#include "../control/ControlBus.h"
#include "../midi/Flx4TutorialMap.h"

#include <QColor>
#include <QRectF>
#include <QWidget>
#include <optional>

namespace gvt {

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
                     bool activationEnabled, const QString& warning);
    void updateCountdown(double beatsAhead);
    void setFeedback(const QString& text, const QColor& color);
    void clearExpected();
    void setHardwareValue(const ControlEvent& event);

signals:
    void controlActivated(const gvt::ControlEvent& event);
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
    bool pulse_ = false;
    int animationPhase_ = 0;
    std::optional<double> hardwareValue_;
};

} // namespace gvt
