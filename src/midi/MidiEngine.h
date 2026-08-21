// PINNED INTERFACE — RtMidi wrapper + DDJ-FLX4 mapping.
#pragma once
#include <QObject>
#include <vector>
#include "../control/ControlBus.h"
#include "SoftTakeover.h"

namespace gvt {

class AudioEngine;

// Opens/monitors MIDI ports (hot-plug poll every 2 s), translates DDJ-FLX4
// messages to ControlBus events (Origin::Midi), and mirrors state back to
// controller LEDs. Non-FLX4 controllers are ignored in the MVP.
class MidiEngine : public QObject {
    Q_OBJECT
public:
    MidiEngine(ControlBus* bus, AudioEngine* engine, QObject* parent = nullptr);
    ~MidiEngine();
    void start();                    // begins hot-plug polling
    bool controllerConnected() const;
    QString controllerName() const;  // "" if none
    void setPerformancePadState(int deck, int mode,
                                unsigned int enabledMask,
                                unsigned int pressedMask);
    void beginTransitionTakeoverTracking();
    void finishTransitionTakeoverTracking();
    void cancelTransitionTakeoverTracking();
    std::vector<SoftTakeoverState> pendingTakeovers() const;
signals:
    void connectionChanged(bool connected, const QString& name);
    // FLX4 browser controls are UI commands rather than transition/audio
    // events, so they bypass ControlBus recording and are delivered here.
    void browseMoved(int rows);
    void browsePressed();
    void loadRequested(int deck);
    void performancePadModeRequested(int deck, int mode);
    void performancePadRequested(int deck, int mode, int pad, bool pressed);
    void hotCueClearRequested(int deck, int pad);
    void softTakeoverChanged();
    void hardwareControlObserved(const gvt::ControlEvent& event);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
