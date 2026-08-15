#pragma once
#include <QWidget>
#include "../control/ControlBus.h"

class QDial;
class QSlider;

namespace gvt {

// Compact horizontal mixer strip (<= ~110 px tall): per channel inline
// TRIM + HI/MID/LOW knobs and a small vertical fader, with the crossfader
// in the center. User actions dispatch onto the ControlBus (Origin::Ui);
// bus events from MIDI/Replay are mirrored back into the controls without
// re-dispatching.
class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(ControlBus* bus, QWidget* parent = nullptr);

private slots:
    void onBusEvent(const gvt::ControlEvent& e, gvt::Origin origin);

private:
    struct Strip {
        QDial* trim = nullptr;
        QDial* eqHigh = nullptr;
        QDial* eqMid = nullptr;
        QDial* eqLow = nullptr;
        QSlider* fader = nullptr;
    };
    QWidget* buildStrip(int deck);
    void wireDial(QDial* d, int deck, ControlId id, double initial);

    ControlBus* bus_;
    Strip strips_[2];
    QSlider* crossfader_ = nullptr;
};

} // namespace gvt
