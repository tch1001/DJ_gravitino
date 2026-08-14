// PINNED INTERFACE — RtMidi wrapper + DDJ-FLX4 mapping.
#pragma once
#include <QObject>
#include "../control/ControlBus.h"

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
signals:
    void connectionChanged(bool connected, const QString& name);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
