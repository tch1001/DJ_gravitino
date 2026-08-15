// PINNED INTERFACE — see docs/ARCHITECTURE.md before changing.
#pragma once
#include <QObject>
#include <atomic>
#include <memory>
#include "../analysis/TrackData.h"
#include "../control/ControlBus.h"

namespace gvt {

constexpr int kNumDecks = 2;

// Per-deck realtime state. Parameters are atomics written from the GUI thread,
// read by the audio thread. Implementation details live in Deck.cpp.
class Deck {
public:
    Deck();
    ~Deck();

    // GUI thread
    void loadTrack(TrackDataPtr t);          // stops, rewinds, swaps track
    TrackDataPtr track() const;              // may be null
    void play(); void stop();
    void handleCue(bool pressed);           // press/hold-preview/release semantics
    void cueJump();                         // jump to the stored deck cue point
    void setHotCue(int i);                   // store current pos
    void jumpHotCue(int i);                  // jump if set
    void nudge(double ticks);                // transient tempo bend from jog

    // Loops (all GUI thread; beat-snapped to the track grid; audio thread
    // wraps position inside [loopStartSec, loopEndSec) while loopActive).
    void loopAuto(double beats);             // start N-beat loop at current beat
    void loopIn();                           // set/replace loop start (pending)
    void loopOut();                          // set end + activate (after loopIn)
    void loopExit();                         // deactivate, keep stored bounds
    void loopHalve(); void loopDouble();     // resize active loop (min 1/8 beat)
    void beatJump(double beats);             // signed, beat-aligned jump
    std::atomic<double> loopStartSec { -1.0 };
    std::atomic<double> loopEndSec   { -1.0 };
    std::atomic<bool>   loopActive   { false };

    // DJ filter knob: 0.5 = off, <0.5 sweeps a low-pass down,
    // >0.5 sweeps a high-pass up. Processed after EQ in render.
    std::atomic<float> filter { 0.5f };

    std::atomic<double> tempoRatio { 1.0 };  // 1.0 = native
    std::atomic<float>  fader      { 1.0f };
    std::atomic<float>  trim       { 0.5f };
    std::atomic<float>  eqLow      { 0.5f }, eqMid { 0.5f }, eqHigh { 0.5f };
    std::atomic<bool>   playing    { false };
    std::atomic<double> cuePointSec { -1.0 };

    // Audio thread: render `frames` of post-fader stereo into out (add nothing,
    // overwrite). Advances position.
    void render(float* out, int frames);

    // Position introspection (any thread).
    double positionSec() const;
    void   seekSec(double sec);
    double beatPosition() const;             // beats via track beatgrid; 0 if no track
    double effectiveBpm() const;             // track bpm * tempoRatio; 0 if no track

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Owns the miniaudio device and the two decks; applies ControlBus events.
class AudioEngine : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(ControlBus* bus, QObject* parent = nullptr);
    ~AudioEngine();

    // Start realtime output. Returns false + error message on failure.
    bool start(QString* error);
    void stopDevice();

    Deck& deck(int i);
    std::atomic<float> crossfader { 0.0f };  // 0 = A, 1 = B

    // Offline mode for --selftest: instead of a live device, render `frames`
    // through the exact same mix path into an interleaved stereo buffer.
    void renderOffline(float* out, int frames);

    // Master-output tap (set/cleared from the GUI thread; the audio thread
    // calls tap->feed(interleavedStereo, frames) after the limiter when set).
    // MasterRecorder::feed is RT-safe (lock-free ring). See MasterRecorder.h.
    std::atomic<class MasterRecorder*> masterTap { nullptr };

    // Called by ControlBus subscription (wired in constructor): applies any
    // ControlEvent to engine state. Replay/MIDI/UI all land here.
    void applyEvent(const ControlEvent& e, Origin origin);

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
