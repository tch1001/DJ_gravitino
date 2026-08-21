// PINNED INTERFACE — see docs/ARCHITECTURE.md before changing.
#pragma once
#include <QList>
#include <QObject>
#include <atomic>
#include <memory>
#include "../analysis/TrackData.h"
#include "../control/ControlBus.h"

namespace gvt {

constexpr int kNumDecks = 2;

struct AudioOutputDevice {
    QString name;
    bool isDefault = false;
};

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
    bool previewActive() const;              // CUE/hot-cue held without PLAY latch
    void handleCue(bool pressed);           // press/hold-preview/release semantics
    void handleHotCue(int i, bool pressed); // hold: play; release: stop + return
    void handleSavedLoop(int i, bool pressed); // same hold/PLAY-latch contract
    void cueJump();                         // jump to the stored deck cue point
    void setHotCue(int i);                   // store current pos
    void jumpHotCue(int i);                  // jump if set
    void nudge(double ticks);                // transient tempo bend from jog
    void beginScratch();                     // suspend transport while top held
    void scratch(double ticks);              // signed audible platter motion
    void endScratch();                       // restore pre-touch play state
    void updateBeatGrid(double bpm, double firstBeatSec); // live regrid

    // Per-deck Quantize. When enabled, hot cues and manual loop IN/OUT resolve
    // to playable whole beats on the track grid.
    std::atomic<bool> quantizeHotCues { true };

    // Loops (all GUI thread; manual IN/OUT use whole-beat snapping only while
    // Quantize is enabled; audio wraps [loopStartSec, loopEndSec)).
    void loopAuto(double beats);             // start N-beat loop at current beat
    void loopIn();                           // set/replace loop start (pending)
    void loopOut();                          // set end + activate (after loopIn)
    void loopExit();                         // deactivate, keep stored bounds
    void loopHalve(); void loopDouble();     // resize active loop (min 1/8 beat)
    bool activateSavedLoop(double startSec, double endSec);
    bool retriggerSavedLoop(double startSec, double endSec); // jump to IN + play
    void beatJump(double beats);             // signed, beat-aligned jump
    std::atomic<double> loopStartSec { -1.0 };
    std::atomic<double> loopEndSec   { -1.0 };
    std::atomic<bool>   loopActive   { false };

    // DJ filter knob: 0.5 = off, <0.5 sweeps a low-pass down,
    // >0.5 sweeps a high-pass up. Processed after EQ in render.
    std::atomic<float> filter { 0.5f };

    // Per-deck FX insert (post-filter, pre-fader). See ControlId::Fx*.
    std::atomic<int>    fxType  { 0 };     // 0 echo, 1 reverb, 2 flanger
    std::atomic<bool>   fxOn    { false };
    std::atomic<float>  fxWet   { 0.5f };
    std::atomic<double> fxBeats { 0.5 };   // echo delay / flanger period

    // Stems: attach/detach separated stems for the CURRENT track (GUI thread;
    // same safe-swap discipline as loadTrack; loadTrack detaches). While
    // attached and any level < ~0.99, render mixes the four stem buffers
    // (int16 -> float) instead of TrackData::pcm.
    void attachStems(StemSetPtr stems);
    bool stemsAttached() const;
    std::atomic<float> stemVocals { 1.0f }, stemMelody { 1.0f },
                       stemBass   { 1.0f }, stemDrums  { 1.0f };

    std::atomic<double> tempoRatio { 1.0 };  // 1.0 = native
    std::atomic<float>  fader      { 1.0f };
    std::atomic<float>  trim       { 0.5f };
    std::atomic<float>  eqLow      { 0.5f }, eqMid { 0.5f }, eqHigh { 0.5f };
    // Post-EQ/filter/FX, pre-channel-fader peak envelope for the FLX4's
    // five-segment channel level meter.
    std::atomic<float>  channelLevel { 0.0f };
    std::atomic<bool>   playing    { false };
    std::atomic<double> cuePointSec { -1.0 };

    // Audio thread: render `frames` of post-fader stereo into out (add nothing,
    // overwrite). Advances position.
    void render(float* out, int frames, float* preFaderOut = nullptr);

    // Position introspection (any thread).
    double positionSec() const;
    void   seekSec(double sec);
    double beatPosition() const;             // beats via track beatgrid; 0 if no track
    double effectiveBpm() const;             // track bpm * tempoRatio; 0 if no track

private:
    void startPlayback(bool latchPreview);
    struct Impl; std::unique_ptr<Impl> impl_;
};

// Owns the miniaudio device and the two decks; applies ControlBus events.
class AudioEngine : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(ControlBus* bus, QObject* parent = nullptr);
    ~AudioEngine();

    // Start realtime output. An empty preference means the macOS system
    // default. Selecting the FLX4 by name enables its four-channel routing.
    bool start(QString* error);
    bool start(const QString& preferredOutputName, QString* error);
    // Live device switch. Deck transport is preserved while CoreAudio is
    // briefly stopped and reopened. On failure the previous output is restored
    // when possible.
    bool switchOutputDevice(const QString& preferredOutputName, QString* error);
    void stopDevice();

    Deck& deck(int i);
    std::atomic<float> crossfader { 0.5f };  // startup center; 0 = A, 1 = B
    std::atomic<bool> headphoneCue[kNumDecks] {}; // channel PFL selection
    std::atomic<bool> masterCue { false };        // master in headphone bus
    std::atomic<float> headphoneMix { 0.0f };     // 0 = CUE, 1 = MASTER

    bool headphoneOutputAvailable() const;
    float headphoneSignalLevel() const;
    // Low-level diagnostic routed only to the FLX4 phones bus (outputs 3/4).
    // It never enters MASTER or a transition/master recording.
    void startHeadphoneTest(int milliseconds = 2000);
    QString outputDeviceName() const;
    QString outputDevicePreference() const;
    QList<AudioOutputDevice> availableOutputDevices(QString* error = nullptr);

    // Offline mode for --selftest: instead of a live device, render `frames`
    // through the exact same mix path into an interleaved stereo buffer.
    void renderOffline(float* out, int frames);
    // Test/diagnostic path: interleaved MASTER L/R, PHONES L/R.
    void renderOfflineFourChannel(float* out, int frames);

    // Master-output tap (set/cleared from the GUI thread; the audio thread
    // calls tap->feed(interleavedStereo, frames) after the limiter when set).
    // MasterRecorder::feed is RT-safe (lock-free ring). See MasterRecorder.h.
    std::atomic<class MasterRecorder*> masterTap { nullptr };

    // Called by ControlBus subscription (wired in constructor): applies any
    // ControlEvent to engine state. Replay/MIDI/UI all land here.
    void applyEvent(const ControlEvent& e, Origin origin);

signals:
    void outputDeviceChanged(const QString& name, bool headphoneOutputAvailable);

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
