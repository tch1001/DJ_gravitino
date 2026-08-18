#pragma once
#include <QWidget>
#include "../control/ControlBus.h"

class QDial;
class QLabel;
class QSlider;

namespace gvt {

// Compact mixer: each channel follows the FLX4's vertical TRIM, HI, MID, LOW,
// FILTER order, with mirrored channel faders flanking the crossfader. This
// minimizes the mixer's preferred width beside the transition workspace. User actions
// dispatch onto the ControlBus (Origin::Ui);
// bus events from MIDI/Replay are mirrored back into the controls without
// re-dispatching.
class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(ControlBus* bus, QWidget* parent = nullptr);
    QWidget* controlWidget(DeckId deck, ControlId control) const;

protected:
    // Double-click on a FILTER dial re-centers it to 0.5 (off).
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onBusEvent(const gvt::ControlEvent& e, gvt::Origin origin);

private:
    struct Strip {
        QDial* trim = nullptr;
        QDial* eqHigh = nullptr;
        QDial* eqMid = nullptr;
        QDial* eqLow = nullptr;
        QDial* filter = nullptr;
        QLabel* filterLabel = nullptr;
        QSlider* fader = nullptr;
    };
    QWidget* buildStrip(int deck);
    void wireDial(QDial* d, int deck, ControlId id, double initial);
    void updateFilterLabel(int deck, double value);

    ControlBus* bus_;
    Strip strips_[2];
    QSlider* crossfader_ = nullptr;
};

} // namespace gvt
