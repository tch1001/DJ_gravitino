#include <memory>

#include "MidiEngine.h"
#include "../audio/AudioEngine.h"
#include "Flx4Mapping.h"
#include "SoftTakeover.h"
#include "../performance/PerformancePads.h"

#include <QMetaObject>
#include <QString>
#include <QTimer>

#include <rtmidi/RtMidi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <set>
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
        if (!connected) {
            takeover.clearHardware();
            takeoverTracking = false;
            takeoverTouched.clear();
            takeoverStartValues.clear();
            takeoverFrozen.store(false, std::memory_order_release);
            emit owner->softTakeoverChanged();
        }
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
        // After automatic replay, every FLX4 action is frozen except moving
        // the absolute controls needed to pick up Gravitino's final state.
        if (takeoverFrozen.load(std::memory_order_acquire) &&
            !SoftTakeover::supports(event)) {
            return;
        }

        if (event.id == ControlId::PerformancePadMode) {
            const int mode = static_cast<int>(std::lround(event.value));
            if (event.deck < 0 || event.deck >= 2 || mode < 0 ||
                mode >= static_cast<int>(PerformancePadMode::Count)) {
                return;
            }
            // Mode buttons are genuine controller input. Update the routing
            // latch immediately on the MIDI callback thread so a pad pressed
            // in the same gesture cannot race the queued GUI repaint.
            activePadMode[static_cast<std::size_t>(event.deck)].store(
                mode, std::memory_order_release);
            hardwarePadMode[static_cast<std::size_t>(event.deck)].store(
                mode, std::memory_order_release);
            MidiEngine* const targetOwner = owner;
            QMetaObject::invokeMethod(
                targetOwner,
                [targetOwner, deck = event.deck, mode] {
                    emit targetOwner->performancePadModeRequested(deck, mode);
                },
                Qt::QueuedConnection);
            return;
        }

        const auto eventId = static_cast<unsigned int>(event.id);
        const auto firstPad =
            static_cast<unsigned int>(ControlId::PerformancePad1);
        const auto lastPad =
            static_cast<unsigned int>(ControlId::PerformancePad8);
        if (eventId >= firstPad && eventId <= lastPad) {
            if (event.deck < 0 || event.deck >= 2)
                return;
            const int pad = static_cast<int>(eventId - firstPad);
            MidiEngine* const targetOwner = owner;
            int encoded = static_cast<int>(std::lround(
                std::fabs(event.value)));
            const bool shifted =
                encoded > Flx4Mapping::kShiftedPadEncodingOffset;
            if (shifted)
                encoded -= Flx4Mapping::kShiftedPadEncodingOffset;
            const int reportedMode = encoded - 1;
            if (reportedMode < 0 ||
                reportedMode >= static_cast<int>(PerformancePadMode::Count)) {
                return;
            }
            hardwarePadMode[static_cast<std::size_t>(event.deck)].store(
                reportedMode, std::memory_order_release);

            const int mode = activePadMode[
                static_cast<std::size_t>(event.deck)].load(
                    std::memory_order_acquire);
            if (mode < 0 ||
                mode >= static_cast<int>(PerformancePadMode::Count)) {
                return;
            }
            const bool pressed = event.value > 0.0;
            if (shifted && mode == static_cast<int>(PerformancePadMode::HotCue)) {
                // SHIFT deletes only when the host-selected layer is HOT CUE.
                // A stale hardware HOT CUE latch must never delete a cue while
                // Gravitino says this pad is PAD FX, BEAT JUMP, etc.
                if (!pressed)
                    return;
                QMetaObject::invokeMethod(
                    targetOwner,
                    [targetOwner, deck = event.deck, pad] {
                        emit targetOwner->hotCueClearRequested(deck, pad);
                    },
                    Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(
                targetOwner,
                [targetOwner, deck = event.deck, mode, pad, pressed] {
                    emit targetOwner->performancePadRequested(
                        deck, mode, pad, pressed);
                },
                Qt::QueuedConnection);
            return;
        }

        if (event.id == ControlId::BrowseNavigate ||
            event.id == ControlId::BrowseSelect ||
            event.id == ControlId::Load) {
            // Library navigation/loading must not become part of a recorded
            // transition. Forward the physical UI command on the GUI thread
            // instead of publishing it as an audio ControlBus event.
            MidiEngine* const targetOwner = owner;
            ControlBus* const targetBus = bus;
            QMetaObject::invokeMethod(
                targetOwner,
                [targetOwner, targetBus, event] {
                    if (event.id == ControlId::BrowseNavigate) {
                        emit targetOwner->browseMoved(
                            static_cast<int>(event.value));
                    } else if (event.id == ControlId::BrowseSelect) {
                        emit targetOwner->browsePressed();
                    } else {
                        emit targetOwner->loadRequested(event.deck);
                        // LOAD remains a tutorial-scored control. Dispatch it
                        // after the UI handler has attempted the guarded load;
                        // AudioEngine treats it as a no-op transport command.
                        if (targetBus)
                            targetBus->dispatch(event, Origin::Midi);
                    }
                },
                Qt::QueuedConnection);
            return;
        }

        if (event.deck == kNoDeck &&
            (event.id == ControlId::FxType ||
             event.id == ControlId::FxOn ||
             event.id == ControlId::FxWet ||
             event.id == ControlId::FxBeats)) {
            postSharedFxEvent(event);
            return;
        }

        if (event.deck >= 0 && event.deck < 2
            && event.id == ControlId::Play && event.value > 0.0) {
            // Toggle against the ENGINE's play state, not our cache — the
            // deck stops itself at end-of-track without a bus event, and a
            // stale cache would turn the next Play press into Stop.
            const bool takingOverPreview = engine != nullptr &&
                engine->deck(event.deck).previewActive();
            const bool wasPlaying =
                engine != nullptr &&
                engine->deck(event.deck).playing.load(std::memory_order_relaxed);
            playing[static_cast<std::size_t>(event.deck)].store(
                takingOverPreview || !wasPlaying, std::memory_order_relaxed);
            event.id = wasPlaying && !takingOverPreview
                           ? ControlId::Stop : ControlId::Play;
        }

        if (event.deck >= 0 && event.deck < 2 &&
            event.id == ControlId::HeadphoneCue && event.value > 0.0) {
            const bool enabled = engine != nullptr &&
                engine->headphoneCue[event.deck].load(
                    std::memory_order_relaxed);
            event.value = enabled ? 0.0 : 1.0;
        }

        if (event.deck == kNoDeck && event.id == ControlId::MasterCue &&
            event.value > 0.0) {
            const bool enabled = engine != nullptr &&
                engine->masterCue.load(std::memory_order_relaxed);
            event.value = enabled ? 0.0 : 1.0;
        }

        if (event.deck >= 0 && event.deck < 2 &&
            event.id == ControlId::Quantize && event.value > 0.0 &&
            engine != nullptr) {
            event.value = engine->deck(event.deck).quantizeHotCues.load(
                              std::memory_order_relaxed)
                ? 0.0 : 1.0;
        }

        if (event.deck >= 0 && event.deck < 2 && engine != nullptr) {
            // The FLX4's physical IN/1/2X and OUT/2X buttons always send the
            // ordinary LOOP IN/OUT notes. Their meaning is contextual: set
            // manual bounds while no loop is active, or resize the current
            // loop when it is active.
            event = Flx4Mapping::resolveLoopButtonAction(
                event, engine->deck(event.deck).loopActive.load(
                           std::memory_order_relaxed));
        }

        if (event.deck >= 0 && event.deck < 2 &&
            event.id == ControlId::LoopAuto && engine != nullptr &&
            engine->deck(event.deck).loopActive.load(
                std::memory_order_relaxed)) {
            // The FLX4 exposes one combined 4 BEAT/EXIT button. Its note is
            // state-independent, so resolve the action against engine truth.
            event.id = ControlId::LoopExit;
            event.value = 1.0;
        }

        ControlBus* const targetBus = bus;
        if (targetBus == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(
            owner,
            [this, event] {
                handleMidiEventOnGui(event);
            },
            Qt::QueuedConnection);
    }

    void handleMidiEventOnGui(const ControlEvent& event)
    {
        emit owner->hardwareControlObserved(event);
        bool changed = false;
        const bool accepted = takeover.acceptHardware(event, &changed);
        if (changed) {
            takeoverFrozen.store(takeover.active(), std::memory_order_release);
            emit owner->softTakeoverChanged();
        }
        if (accepted && bus != nullptr)
            bus->dispatch(event, Origin::Midi);
    }

    void postSharedFxEvent(const ControlEvent& event) noexcept
    {
        if (bus == nullptr || engine == nullptr)
            return;

        const std::array<bool, 2> assigned = mapping.fxAssignedDecks();
        std::array<ControlEvent, 2> events {};
        std::size_t eventCount = 0;
        for (DeckId deck = 0; deck < 2; ++deck) {
            if (!assigned[static_cast<std::size_t>(deck)])
                continue;

            Deck& target = engine->deck(deck);
            double value = event.value;
            switch (event.id) {
            case ControlId::FxOn:
                value = target.fxOn.load(std::memory_order_relaxed)
                    ? 0.0 : 1.0;
                break;
            case ControlId::FxBeats:
                value = std::clamp(
                    target.fxBeats.load(std::memory_order_relaxed) *
                        event.value,
                    0.25, 4.0);
                break;
            case ControlId::FxType: {
                const int direction = event.value < 0.0 ? -1 : 1;
                const int current = std::clamp(
                    target.fxType.load(std::memory_order_relaxed), 0, 2);
                value = static_cast<double>((current + direction + 3) % 3);
                break;
            }
            case ControlId::FxWet:
                if (!std::isfinite(value))
                    continue;
                value = std::clamp(value, 0.0, 1.0);
                break;
            default:
                return;
            }
            events[eventCount++] = ControlEvent {deck, event.id, value};
        }

        if (eventCount == 0)
            return;
        ControlBus* const targetBus = bus;
        QMetaObject::invokeMethod(
            targetBus,
            [targetBus, events, eventCount] {
                for (std::size_t index = 0; index < eventCount; ++index)
                    targetBus->dispatch(events[index], Origin::Midi);
            },
            Qt::QueuedConnection);
    }

    void observeEvent(const ControlEvent& event, Origin origin)
    {
        if (takeoverTracking && origin != Origin::Midi &&
            SoftTakeover::supports(event)) {
            takeoverTouched.insert(
                {event.deck, static_cast<unsigned int>(event.id)});
        } else if (!takeoverTracking && takeover.active() &&
                   origin != Origin::Midi &&
                   SoftTakeover::supports(event) &&
                   takeover.retarget(event)) {
            takeoverFrozen.store(takeover.active(), std::memory_order_release);
            emit owner->softTakeoverChanged();
        }

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

        if (event.id == ControlId::HeadphoneCue) {
            const bool actual = engine != nullptr &&
                engine->headphoneCue[event.deck].load(
                    std::memory_order_relaxed);
            headphoneCue[deck] = actual;
            // The FLX4 channel-CUE LED is host-controlled, so acknowledge
            // physical MIDI presses as well as UI/system-origin changes.
            sendLed(event.deck, ControlId::HeadphoneCue, actual);
            return;
        }

        if (event.id == ControlId::Quantize) {
            const bool actual = engine != nullptr &&
                engine->deck(event.deck).quantizeHotCues.load(
                    std::memory_order_relaxed);
            quantize[deck] = actual;
            // Quantize is host state, so acknowledge physical toggles too.
            sendLed(event.deck, ControlId::Quantize, actual);
            return;
        }

        if (event.id == ControlId::LoopIn ||
            event.id == ControlId::LoopOut ||
            event.id == ControlId::LoopExit ||
            event.id == ControlId::LoopHalve ||
            event.id == ControlId::LoopDouble ||
            event.id == ControlId::LoopAuto) {
            const bool actual = engine != nullptr &&
                engine->deck(event.deck).loopActive.load(
                    std::memory_order_relaxed);
            const bool changed = loopActive[deck] != actual;
            loopActive[deck] = actual;
            if (mirrorToController || changed)
                sendLoopLeds(event.deck, actual);
            return;
        }

        if (event.id == ControlId::FxOn) {
            const bool actual = event.value > 0.5;
            const bool changed = fxOn[deck] != actual;
            fxOn[deck] = actual;
            if (mirrorToController || changed)
                sendLed(event.deck, ControlId::FxOn, actual);
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

    void sendRaw(
        const std::optional<std::array<unsigned char, 3>>& message) noexcept
    {
        if (!message || midiOut == nullptr || !midiOut->isPortOpen())
            return;
        try {
            midiOut->sendMessage(message->data(), message->size());
        } catch (...) {
            closeOutput();
        }
    }

    void sendPerformancePadState(DeckId deck) noexcept
    {
        if (deck < 0 || deck >= 2)
            return;
        const auto index = static_cast<std::size_t>(deck);
        const int selectedMode = padMode[index];
        for (int mode = 0;
             mode < static_cast<int>(PerformancePadMode::Count); ++mode) {
            sendRaw(Flx4Mapping::padModeLedMessage(
                deck, mode, mode == selectedMode));
        }

        const unsigned int visibleMask =
            padEnabledMask[index] | padPressedMask[index];
        const auto sendBank = [this, deck, visibleMask](int bankMode) {
            const auto mode = static_cast<PerformancePadMode>(bankMode);
            const bool shiftedBank = performancePadModeIsShifted(mode);
            for (int pad = 0; pad < kPerformancePadCount; ++pad) {
                const unsigned int bit = 1U << static_cast<unsigned int>(pad);
                const unsigned char velocity =
                    (visibleMask & bit) != 0U ? 0x7F : 0x00;
                sendRaw(Flx4Mapping::performancePadLedMessage(
                    deck, bankMode, pad, shiftedBank, velocity));
                // HOT CUE has an explicit SHIFT delete gesture, so keep its
                // shifted address space mirrored too.
                if (mode == PerformancePadMode::HotCue) {
                    sendRaw(Flx4Mapping::performancePadLedMessage(
                        deck, bankMode, pad, true, velocity));
                }
            }
        };

        sendBank(selectedMode);
        // MIDI OUT can light a bank but cannot change the FLX4's private pad
        // latch. If the user selected a virtual layer, also render that layer's
        // state into the last bank the controller actually reported so the
        // physical pads do not visually contradict their host-routed action.
        const int reportedHardwareMode = hardwarePadMode[index].load(
            std::memory_order_acquire);
        if (reportedHardwareMode != selectedMode && reportedHardwareMode >= 0 &&
            reportedHardwareMode < static_cast<int>(PerformancePadMode::Count)) {
            sendBank(reportedHardwareMode);
        }
    }

    void sendLoopLeds(DeckId deck, bool on) noexcept
    {
        sendLed(deck, ControlId::LoopIn, on);
        sendLed(deck, ControlId::LoopOut, on);
        sendLed(deck, ControlId::LoopExit, on);
        // Mirror the modifier-specific note numbers too. The FLX4 uses the
        // same physical LEDs but addresses them separately while SHIFT is held.
        sendLed(deck, ControlId::LoopHalve, on);
        sendLed(deck, ControlId::LoopDouble, on);
        sendLed(deck, ControlId::LoopAuto, on);
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

            const bool actualLoop =
                engine->deck(deck).loopActive.load(std::memory_order_relaxed);
            if (loopActive[index] != actualLoop) {
                loopActive[index] = actualLoop;
                if (connected)
                    sendLoopLeds(deck, actualLoop);
            }

            const bool actualFx =
                engine->deck(deck).fxOn.load(std::memory_order_relaxed);
            if (fxOn[index] != actualFx) {
                fxOn[index] = actualFx;
                if (connected)
                    sendLed(deck, ControlId::FxOn, actualFx);
            }

            const bool actualHeadphoneCue =
                engine->headphoneCue[deck].load(std::memory_order_relaxed);
            if (headphoneCue[index] != actualHeadphoneCue) {
                headphoneCue[index] = actualHeadphoneCue;
                if (connected)
                    sendLed(deck, ControlId::HeadphoneCue,
                            actualHeadphoneCue);
            }

            const bool actualQuantize =
                engine->deck(deck).quantizeHotCues.load(
                    std::memory_order_relaxed);
            if (quantize[index] != actualQuantize) {
                quantize[index] = actualQuantize;
                if (connected)
                    sendLed(deck, ControlId::Quantize, actualQuantize);
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

    void reconcileChannelMeters() noexcept
    {
        if (engine == nullptr || !connected)
            return;
        for (DeckId deck = 0; deck < 2; ++deck) {
            const float peak = std::clamp(
                engine->deck(deck).channelLevel.load(
                    std::memory_order_relaxed),
                0.0f, 1.0f);
            unsigned char value = 0;
            if (peak > 0.0001f) {
                // Map -48 dBFS..0 dBFS onto the FLX4's documented 0..127
                // meter value. Its firmware converts this into two green,
                // two orange, and one red segment.
                const double db = 20.0 * std::log10(
                    static_cast<double>(peak));
                const double normalized = std::clamp(
                    (db + 48.0) / 48.0, 0.0, 1.0);
                value = static_cast<unsigned char>(
                    std::lround(normalized * 127.0));
            }
            const auto index = static_cast<std::size_t>(deck);
            if (channelMeter[index] == value)
                continue;
            channelMeter[index] = value;
            sendRaw(Flx4Mapping::channelLevelMessage(deck, value));
        }
    }

    void syncLedState() noexcept
    {
        channelMeter.fill(0xFF);
        for (DeckId deck = 0; deck < 2; ++deck) {
            const auto index = static_cast<std::size_t>(deck);
            if (engine != nullptr) {
                playing[index].store(
                    engine->deck(deck).playing.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                cue[index] = engine->deck(deck).cuePointSec.load(
                    std::memory_order_relaxed) >= 0.0;
                loopActive[index] = engine->deck(deck).loopActive.load(
                    std::memory_order_relaxed);
                fxOn[index] = engine->deck(deck).fxOn.load(
                    std::memory_order_relaxed);
                headphoneCue[index] = engine->headphoneCue[deck].load(
                    std::memory_order_relaxed);
                quantize[index] = engine->deck(deck).quantizeHotCues.load(
                    std::memory_order_relaxed);

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
            sendLoopLeds(deck, loopActive[index]);
            sendLed(deck, ControlId::FxOn, fxOn[index]);
            sendLed(deck, ControlId::HeadphoneCue,
                    headphoneCue[index]);
            sendLed(deck, ControlId::Quantize, quantize[index]);

            for (std::size_t pad = 0; pad < hotCue[index].size(); ++pad) {
                const auto id = static_cast<ControlId>(
                    static_cast<unsigned int>(ControlId::HotCue1) + pad);
                sendLed(deck, id, hotCue[index][pad]);
            }
            sendPerformancePadState(deck);
        }
        reconcileChannelMeters();
    }

    double engineValue(DeckId deck, ControlId id) const noexcept
    {
        if (engine == nullptr) return 0.0;
        if (id == ControlId::Crossfader)
            return engine->crossfader.load(std::memory_order_relaxed);
        if (deck < 0 || deck >= 2) return 0.0;
        const Deck& target = engine->deck(deck);
        switch (id) {
        case ControlId::Tempo:  return target.tempoRatio.load(std::memory_order_relaxed);
        case ControlId::Fader:  return target.fader.load(std::memory_order_relaxed);
        case ControlId::Trim:   return target.trim.load(std::memory_order_relaxed);
        case ControlId::EqLow:  return target.eqLow.load(std::memory_order_relaxed);
        case ControlId::EqMid:  return target.eqMid.load(std::memory_order_relaxed);
        case ControlId::EqHigh: return target.eqHigh.load(std::memory_order_relaxed);
        case ControlId::Filter: return target.filter.load(std::memory_order_relaxed);
        default:                return 0.0;
        }
    }

    void beginTakeoverTracking()
    {
        takeover.clear();
        takeoverTouched.clear();
        takeoverStartValues.clear();
        if (engine != nullptr) {
            static constexpr ControlId deckControls[] = {
                ControlId::Tempo, ControlId::Fader,
                ControlId::EqHigh, ControlId::EqMid, ControlId::EqLow,
                ControlId::Filter};
            for (DeckId deck = 0; deck < 2; ++deck)
                for (ControlId control : deckControls)
                    takeoverStartValues[{deck,
                        static_cast<unsigned int>(control)}] =
                            engineValue(deck, control);
            takeoverStartValues[{kNoDeck,
                static_cast<unsigned int>(ControlId::Crossfader)}] =
                    engineValue(kNoDeck, ControlId::Crossfader);
        }
        takeoverTracking = true;
        takeoverFrozen.store(false, std::memory_order_release);
        emit owner->softTakeoverChanged();
    }

    void finishTakeoverTracking()
    {
        takeoverTracking = false;
        if (!connected || takeoverTouched.empty()) {
            takeover.clear();
            takeoverTouched.clear();
            takeoverStartValues.clear();
            takeoverFrozen.store(false, std::memory_order_release);
            emit owner->softTakeoverChanged();
            return;
        }

        std::vector<ControlEvent> targets;
        targets.reserve(takeoverTouched.size());
        for (const auto& [deck, rawControl] : takeoverTouched) {
            const ControlId control = static_cast<ControlId>(rawControl);
            const double finalValue = engineValue(deck, control);
            const auto start = takeoverStartValues.find({deck, rawControl});
            // Only require pickup for a control whose authoritative virtual
            // value actually changed during this replay. Setup dispatches of
            // already-correct state must not make an untouched FLX4 control
            // look invalid.
            if (start != takeoverStartValues.end() &&
                std::fabs(start->second - finalValue) <= 1.0e-6)
                continue;
            targets.push_back(ControlEvent {deck, control, finalValue});
        }
        takeoverTouched.clear();
        takeoverStartValues.clear();
        takeover.arm(targets);
        takeoverFrozen.store(takeover.active(), std::memory_order_release);
        emit owner->softTakeoverChanged();
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
    std::array<bool, 2> loopActive {};
    std::array<bool, 2> fxOn {};
    std::array<bool, 2> headphoneCue {};
    std::array<bool, 2> quantize {true, true};
    std::array<std::array<bool, 8>, 2> hotCue {};
    std::array<unsigned char, 2> channelMeter {0xFF, 0xFF};
    std::array<int, 2> padMode {
        static_cast<int>(PerformancePadMode::HotCue),
        static_cast<int>(PerformancePadMode::HotCue)};
    std::array<std::atomic<int>, 2> activePadMode {
        std::atomic<int> {static_cast<int>(PerformancePadMode::HotCue)},
        std::atomic<int> {static_cast<int>(PerformancePadMode::HotCue)}};
    std::array<std::atomic<int>, 2> hardwarePadMode {
        std::atomic<int> {static_cast<int>(PerformancePadMode::HotCue)},
        std::atomic<int> {static_cast<int>(PerformancePadMode::HotCue)}};
    std::array<unsigned int, 2> padEnabledMask {};
    std::array<unsigned int, 2> padPressedMask {};
    SoftTakeover takeover;
    bool takeoverTracking = false;
    std::set<std::pair<DeckId, unsigned int>> takeoverTouched;
    std::map<std::pair<DeckId, unsigned int>, double> takeoverStartValues;
    std::atomic_bool takeoverFrozen {false};
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
    auto* meterTimer = new QTimer(this);
    meterTimer->setInterval(40);
    connect(meterTimer, &QTimer::timeout, this,
            [this] { impl_->reconcileChannelMeters(); });
    meterTimer->start();
}

MidiEngine::~MidiEngine() = default;

void MidiEngine::start()
{
    impl_->start();
}

void MidiEngine::beginTransitionTakeoverTracking()
{
    impl_->beginTakeoverTracking();
}

void MidiEngine::finishTransitionTakeoverTracking()
{
    impl_->finishTakeoverTracking();
}

std::vector<SoftTakeoverState> MidiEngine::pendingTakeovers() const
{
    return impl_->takeover.pending();
}

bool MidiEngine::controllerConnected() const
{
    return impl_->connected;
}

QString MidiEngine::controllerName() const
{
    return impl_->connectedName;
}

void MidiEngine::setPerformancePadState(
    int deck, int mode, unsigned int enabledMask,
    unsigned int pressedMask)
{
    if (deck < 0 || deck >= 2 || mode < 0 ||
        mode >= static_cast<int>(PerformancePadMode::Count)) {
        return;
    }
    const auto index = static_cast<std::size_t>(deck);
    impl_->activePadMode[index].store(mode, std::memory_order_release);
    const unsigned int normalizedEnabled = enabledMask & 0xFFU;
    const unsigned int normalizedPressed = pressedMask & 0xFFU;
    if (impl_->padMode[index] == mode &&
        impl_->padEnabledMask[index] == normalizedEnabled &&
        impl_->padPressedMask[index] == normalizedPressed) {
        return;
    }
    impl_->padMode[index] = mode;
    impl_->padEnabledMask[index] = normalizedEnabled;
    impl_->padPressedMask[index] = normalizedPressed;
    if (impl_->connected)
        impl_->sendPerformancePadState(deck);
}

} // namespace gvt
