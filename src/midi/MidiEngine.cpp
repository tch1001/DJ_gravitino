#include <memory>

#include "MidiEngine.h"
#include "../audio/AudioEngine.h"
#include "Flx4Mapping.h"

#include <QMetaObject>
#include <QString>
#include <QTimer>

#include <rtmidi/RtMidi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace gvt {
namespace {

constexpr int kPortPollIntervalMs = 2000;

struct PortInfo {
    unsigned int index = 0;
    QString name;
};

bool isFlx4Port(const QString& name)
{
    return name.contains(QStringLiteral("DDJ-FLX4"), Qt::CaseInsensitive)
        || name.contains(QStringLiteral("FLX4"), Qt::CaseInsensitive);
}

template<typename MidiPort>
bool enumeratePorts(MidiPort& midi, std::vector<PortInfo>& ports) noexcept
{
    try {
        const unsigned int count = midi.getPortCount();
        ports.clear();
        ports.reserve(count);
        for (unsigned int index = 0; index < count; ++index) {
            ports.push_back(PortInfo {
                index, QString::fromStdString(midi.getPortName(index))});
        }
        return true;
    } catch (...) {
        ports.clear();
        return false;
    }
}

const PortInfo* firstFlx4Port(const std::vector<PortInfo>& ports)
{
    for (const auto& port : ports) {
        if (isFlx4Port(port.name)) {
            return &port;
        }
    }
    return nullptr;
}

bool containsPort(const std::vector<PortInfo>& ports, const QString& name)
{
    for (const auto& port : ports) {
        if (port.name == name) {
            return true;
        }
    }
    return false;
}

const PortInfo* preferredOutputPort(
    const std::vector<PortInfo>& ports, const QString& inputName)
{
    for (const auto& port : ports) {
        if (port.name == inputName && isFlx4Port(port.name)) {
            return &port;
        }
    }
    return firstFlx4Port(ports);
}

} // namespace

struct MidiEngine::Impl {
    Impl(MidiEngine* ownerIn, ControlBus* busIn)
        : owner(ownerIn), bus(busIn)
    {
        pollTimer.setInterval(kPortPollIntervalMs);
        pollTimer.setSingleShot(false);
        QObject::connect(&pollTimer, &QTimer::timeout, owner, [this] {
            pollPorts();
        });

        if (bus != nullptr) {
            QObject::connect(
                bus, &ControlBus::eventDispatched, owner,
                [this](const ControlEvent& event, Origin origin) {
                    observeEvent(event, origin);
                });
        }

        for (auto& state : playing) {
            state.store(false, std::memory_order_relaxed);
        }
    }

    ~Impl()
    {
        pollTimer.stop();
        if (midiIn != nullptr) {
            try {
                midiIn->cancelCallback();
            } catch (...) {
            }
        }
        closeInput();
        closeOutput();
        // Destroy the RtMidi objects NOW, while every other member is still
        // alive: their destructors tear down the CoreMIDI client, which is the
        // only thing that guarantees no callback is left running when the
        // remaining members (mapping, names, ...) are destroyed.
        try { midiIn.reset(); } catch (...) {}
        try { midiOut.reset(); } catch (...) {}
    }

    void start()
    {
        if (pollTimer.isActive()) {
            return;
        }
        pollPorts();
        pollTimer.start();
    }

    bool ensureInput() noexcept
    {
        if (midiIn != nullptr) {
            return true;
        }

        try {
            auto input = std::make_unique<RtMidiIn>(
                RtMidi::UNSPECIFIED, "Gravitino MIDI Input");
            input->ignoreTypes(true, true, true);
            input->setCallback(&Impl::midiCallback, this);
            midiIn = std::move(input);
            return true;
        } catch (...) {
            midiIn.reset();
            return false;
        }
    }

    bool ensureOutput() noexcept
    {
        if (midiOut != nullptr) {
            return true;
        }

        try {
            midiOut = std::make_unique<RtMidiOut>(
                RtMidi::UNSPECIFIED, "Gravitino MIDI Output");
            return true;
        } catch (...) {
            midiOut.reset();
            return false;
        }
    }

    void pollPorts() noexcept
    {
        if (!ensureInput()) {
            disconnectController();
            return;
        }

        std::vector<PortInfo> inputPorts;
        if (!enumeratePorts(*midiIn, inputPorts)) {
            disconnectController();
            midiIn.reset();
            return;
        }

        if (connected && !containsPort(inputPorts, connectedName)) {
            disconnectController();
        }

        if (!connected) {
            const PortInfo* port = firstFlx4Port(inputPorts);
            if (port == nullptr) {
                return;
            }

            try {
                if (midiIn->isPortOpen()) {
                    midiIn->closePort();
                }
                midiIn->openPort(port->index, "Gravitino DDJ-FLX4 Input");
                setConnected(true, port->name);
            } catch (...) {
                closeInput();
                return;
            }
        }

        pollOutputPort();
    }

    void pollOutputPort() noexcept
    {
        if (!connected || !ensureOutput()) {
            return;
        }

        std::vector<PortInfo> outputPorts;
        if (!enumeratePorts(*midiOut, outputPorts)) {
            closeOutput();
            midiOut.reset();
            return;
        }

        if (midiOut->isPortOpen()
            && !containsPort(outputPorts, connectedOutputName)) {
            closeOutput();
        }

        if (midiOut->isPortOpen()) {
            return;
        }

        const PortInfo* port = preferredOutputPort(outputPorts, connectedName);
        if (port == nullptr) {
            return;
        }

        try {
            midiOut->openPort(port->index, "Gravitino DDJ-FLX4 Output");
            connectedOutputName = port->name;
            syncLedState();
        } catch (...) {
            closeOutput();
        }
    }

    void disconnectController() noexcept
    {
        closeInput();
        closeOutput();
        setConnected(false, {});
    }

    void closeInput() noexcept
    {
        if (midiIn == nullptr) {
            return;
        }
        try {
            if (midiIn->isPortOpen()) {
                midiIn->closePort();
            }
        } catch (...) {
        }
    }

    void closeOutput() noexcept
    {
        connectedOutputName.clear();
        if (midiOut == nullptr) {
            return;
        }
        try {
            if (midiOut->isPortOpen()) {
                midiOut->closePort();
            }
        } catch (...) {
        }
    }

    void setConnected(bool newConnected, QString newName)
    {
        if (connected == newConnected && connectedName == newName) {
            return;
        }
        connected = newConnected;
        connectedName = std::move(newName);
        emit owner->connectionChanged(connected, connectedName);
    }

    static void midiCallback(
        double, std::vector<unsigned char>* message, void* userData) noexcept
    {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || message == nullptr) {
            return;
        }

        try {
            auto event = self->mapping.parse(*message);
            if (event.has_value()) {
                self->postMidiEvent(*event);
            }
        } catch (...) {
            // No exception may escape onto CoreMIDI's callback thread.
        }
    }

    void postMidiEvent(ControlEvent event) noexcept
    {
        if (event.deck >= 0 && event.deck < 2
            && event.id == ControlId::Play && event.value > 0.0) {
            // Toggle against the ENGINE's play state, not our cache — the
            // deck stops itself at end-of-track without a bus event, and a
            // stale cache would turn the next Play press into Stop.
            const bool wasPlaying =
                engine != nullptr &&
                engine->deck(event.deck).playing.load(std::memory_order_relaxed);
            playing[static_cast<std::size_t>(event.deck)].store(
                !wasPlaying, std::memory_order_relaxed);
            event.id = wasPlaying ? ControlId::Stop : ControlId::Play;
        }

        ControlBus* const targetBus = bus;
        if (targetBus == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(
            targetBus,
            [targetBus, event] {
                targetBus->dispatch(event, Origin::Midi);
            },
            Qt::QueuedConnection);
    }

    void observeEvent(const ControlEvent& event, Origin origin)
    {
        if (event.deck < 0 || event.deck > 1) {
            return;
        }

        const auto deck = static_cast<std::size_t>(event.deck);
        const bool mirrorToController = origin != Origin::Midi;

        if (event.id == ControlId::Play && event.value > 0.0) {
            playing[deck].store(true, std::memory_order_relaxed);
            if (mirrorToController) {
                sendLed(event.deck, ControlId::Play, true);
            }
            return;
        }

        if (event.id == ControlId::Stop && event.value > 0.0) {
            playing[deck].store(false, std::memory_order_relaxed);
            if (mirrorToController) {
                sendLed(event.deck, ControlId::Play, false);
            }
            return;
        }

        if (event.id == ControlId::Cue) {
            cue[deck] = engine != nullptr &&
                engine->deck(event.deck).cuePointSec.load(
                    std::memory_order_relaxed) >= 0.0;
            if (mirrorToController) {
                sendLed(event.deck, ControlId::Cue, cue[deck]);
            }
            return;
        }

        const auto idValue = static_cast<unsigned int>(event.id);
        const auto firstHotCue = static_cast<unsigned int>(ControlId::HotCue1);
        const auto lastHotCue = static_cast<unsigned int>(ControlId::HotCue8);
        if (idValue < firstHotCue || idValue > lastHotCue) {
            return;
        }
        if (event.value < 0.5)
            return;

        const auto pad = static_cast<std::size_t>(idValue - firstHotCue);
        const TrackDataPtr currentTrack = engine != nullptr
            ? engine->deck(event.deck).track()
            : TrackDataPtr {};
        hotCue[deck][pad] = currentTrack
            ? currentTrack->hotCues[pad] >= 0.0
            : true;
        if (mirrorToController) {
            sendLed(event.deck, event.id, hotCue[deck][pad]);
        }
    }

    void sendLed(DeckId deck, ControlId id, bool on) noexcept
    {
        if (midiOut == nullptr || !midiOut->isPortOpen()) {
            return;
        }

        const auto message = Flx4Mapping::ledMessage(deck, id, on);
        if (!message.has_value()) {
            return;
        }

        try {
            midiOut->sendMessage(message->data(), message->size());
        } catch (...) {
            closeOutput();
        }
    }

    // Cached deck state can drift from the engine (deck stops itself at EOF,
    // tracks load outside the bus, and cue/hot-cue LEDs represent stored
    // points rather than momentary button state).
    // Poll the engine truth and push LED deltas.
    void reconcileTransportLeds() noexcept
    {
        if (engine == nullptr) {
            return;
        }
        for (DeckId deck = 0; deck < 2; ++deck) {
            const auto index = static_cast<std::size_t>(deck);
            const bool actual =
                engine->deck(deck).playing.load(std::memory_order_relaxed);
            if (playing[index].load(std::memory_order_relaxed) != actual) {
                playing[index].store(actual, std::memory_order_relaxed);
                if (connected) {
                    sendLed(deck, ControlId::Play, actual);
                }
            }

            const bool actualCue =
                engine->deck(deck).cuePointSec.load(
                    std::memory_order_relaxed) >= 0.0;
            if (cue[index] != actualCue) {
                cue[index] = actualCue;
                if (connected)
                    sendLed(deck, ControlId::Cue, actualCue);
            }

            const TrackDataPtr currentTrack = engine->deck(deck).track();
            for (std::size_t pad = 0; pad < hotCue[index].size(); ++pad) {
                const bool actualHotCue = currentTrack
                    ? currentTrack->hotCues[pad] >= 0.0
                    : false;
                if (hotCue[index][pad] == actualHotCue)
                    continue;

                hotCue[index][pad] = actualHotCue;
                if (connected) {
                    const auto id = static_cast<ControlId>(
                        static_cast<unsigned int>(ControlId::HotCue1) + pad);
                    sendLed(deck, id, actualHotCue);
                }
            }
        }
    }

    void syncLedState() noexcept
    {
        for (DeckId deck = 0; deck < 2; ++deck) {
            const auto index = static_cast<std::size_t>(deck);
            if (engine != nullptr) {
                playing[index].store(
                    engine->deck(deck).playing.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                cue[index] = engine->deck(deck).cuePointSec.load(
                    std::memory_order_relaxed) >= 0.0;

                const TrackDataPtr currentTrack = engine->deck(deck).track();
                for (std::size_t pad = 0; pad < hotCue[index].size(); ++pad) {
                    hotCue[index][pad] = currentTrack
                        ? currentTrack->hotCues[pad] >= 0.0
                        : false;
                }
            }

            sendLed(
                deck, ControlId::Play,
                playing[index].load(std::memory_order_relaxed));
            sendLed(deck, ControlId::Cue, cue[index]);

            for (std::size_t pad = 0; pad < hotCue[index].size(); ++pad) {
                const auto id = static_cast<ControlId>(
                    static_cast<unsigned int>(ControlId::HotCue1) + pad);
                sendLed(deck, id, hotCue[index][pad]);
            }
        }
    }

    MidiEngine* owner = nullptr;
    ControlBus* bus = nullptr;
    AudioEngine* engine = nullptr;
    QTimer pollTimer;
    std::unique_ptr<RtMidiIn> midiIn;
    std::unique_ptr<RtMidiOut> midiOut;
    Flx4Mapping mapping;
    bool connected = false;
    QString connectedName;
    QString connectedOutputName;
    std::array<std::atomic_bool, 2> playing {};
    std::array<bool, 2> cue {};
    std::array<std::array<bool, 8>, 2> hotCue {};
};

MidiEngine::MidiEngine(
    ControlBus* bus, AudioEngine* engine, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, bus))
{
    impl_->engine = engine;
    auto* ledTimer = new QTimer(this);
    ledTimer->setInterval(250);
    connect(ledTimer, &QTimer::timeout, this,
            [this] { impl_->reconcileTransportLeds(); });
    ledTimer->start();
}

MidiEngine::~MidiEngine() = default;

void MidiEngine::start()
{
    impl_->start();
}

bool MidiEngine::controllerConnected() const
{
    return impl_->connected;
}

QString MidiEngine::controllerName() const
{
    return impl_->connectedName;
}

} // namespace gvt
