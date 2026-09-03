#include <QtGlobal>
#include "AudioEngine.h"

#include "Eq.h"
#include "Fx.h"
#include "signalsmith-stretch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace gvt {
namespace {

constexpr double kJogRatioPerTick = 0.004;
constexpr double kMaximumJogRatio = 0.25;
// FLX4 platter packets are relative rotation counts. Ten milliseconds per
// count keeps a slow turn controllable while making a deliberate spin an
// audible coarse scrub, distinctly stronger than the rim's tempo bend.
constexpr double kPlatterScratchSecondsPerTick = 0.01;
constexpr double kMaximumScratchFramesPerOutputFrame = 12.0;
constexpr double kCuePreviewToleranceSec = 0.05;
// Reaches 0.1% of the initial bend after approximately 200 ms at 48 kHz.
constexpr double kJogDecayPerFrame = 0.999280701;
constexpr double kMinimumLoopBeats = 0.125;
constexpr double kMaximumLoopBeats = 64.0;

float clampUnit(float value, float fallback) noexcept
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, 0.0f, 1.0f);
}

double trimGain(float knob) noexcept
{
    knob = clampUnit(knob, 0.5f);
    if (knob <= 0.5f)
        return static_cast<double>(knob) * 2.0;

    const double boostDb = (static_cast<double>(knob) - 0.5) * 12.0;
    return std::pow(10.0, boostDb / 20.0);
}

bool hasUsableBeatGrid(const TrackDataPtr& track) noexcept
{
    return track != nullptr && std::isfinite(track->bpm) && track->bpm > 0.0 &&
           std::isfinite(track->firstBeatSec) && track->frameCount() > 0;
}

double trackDurationSec(const TrackData& track) noexcept
{
    return static_cast<double>(track.frameCount()) /
           static_cast<double>(kSampleRate);
}

double clampTrackSec(const TrackData& track, double sec) noexcept
{
    return std::clamp(sec, 0.0, trackDurationSec(track));
}

double snapToPlayableBeat(const TrackDataPtr& track, double sec) noexcept
{
    if (!hasUsableBeatGrid(track) || !std::isfinite(sec))
        return sec;

    const double duration = trackDurationSec(*track);
    if (!(duration > 0.0))
        return sec;

    // Quantized hot cues must remain playable. In particular, avoid snapping
    // a point near EOF to the first grid line after the track, which would
    // make PLAY's normal end-of-track restart rule jump back to zero.
    const double finalPlayableSec = std::nextafter(duration, 0.0);
    const double firstPlayableBeat = std::ceil(track->beatAtSec(0.0));
    const double lastPlayableBeat =
        std::floor(track->beatAtSec(finalPlayableSec));
    if (!std::isfinite(firstPlayableBeat) ||
        !std::isfinite(lastPlayableBeat) ||
        firstPlayableBeat > lastPlayableBeat) {
        return std::clamp(sec, 0.0, finalPlayableSec);
    }

    const double nearestBeat = std::clamp(
        std::round(track->beatAtSec(sec)),
        firstPlayableBeat, lastPlayableBeat);
    return std::clamp(track->secAtBeat(nearestBeat),
                      0.0, finalPlayableSec);
}

} // namespace

struct Deck::Impl {
    TrackDataPtr ownedTrack;
    std::atomic<TrackData*> audioTrack { nullptr };
    std::atomic<double> positionFrames { 0.0 };
    std::atomic<double> trackBpm { 0.0 };
    std::atomic<double> firstBeatSec { 0.0 };
    std::atomic<int64_t> trackFrameCount { 0 };
    std::atomic<bool> renderGate { true };
    StemSetPtr ownedStems;                       // GUI-side retention
    std::atomic<StemSet*> audioStems { nullptr };// audio-thread view
    std::atomic<unsigned int> activeRenders { 0 };
    std::atomic<double> pendingJogRatio { 0.0 };
    std::atomic<bool> scratchActive { false };
    std::atomic<bool> scratchResumePlaying { false };
    std::atomic<double> pendingScratchFrames { 0.0 };
    std::atomic<bool> cuePreviewing { false };
    std::atomic<int> hotCuePreviewIndex { -1 };
    std::atomic<double> hotCuePreviewSec { -1.0 };
    std::array<double, 8> transitionCueSecs {
        -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    std::array<double, 8> transitionLoopEndSecs {
        -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    std::atomic<int> savedLoopPreviewIndex { -1 };
    std::atomic<double> savedLoopPreviewSec { -1.0 };
    double jogRatio = 0.0; // Audio-thread-owned after a safe track swap.
    double pendingLoopInSec = -1.0; // GUI-thread-owned.
    bool hasPendingLoopIn = false;
    Eq eq;
    DjFilter djFilter;
    DeckFx fx;

    // Per-deck stereo key-lock processor. Buffers are sized for the realtime
    // callback up front; larger offline calls may grow them outside normal
    // device operation. A fixed seed makes regression output deterministic.
    signalsmith::stretch::SignalsmithStretch<float> keyLock {0x475654};
    std::array<std::vector<float>, 2> keyInput;
    std::array<std::vector<float>, 2> keyOutput;
    std::array<std::vector<float>, 2> keySeek;
    double keyInputRemainder = 0.0;
    double keyExpectedPositionFrames = 0.0;
    bool keyLockPrimed = false;

    Impl()
    {
        // About 43 ms analysis with evenly split computation: short enough
        // for DJ transport, while still retaining substantially better bass
        // and transient quality than a tiny granular shifter.
        keyLock.configure(2, 2048, 512, true);
        for (int channel = 0; channel < 2; ++channel) {
            keyInput[channel].resize(1025);
            keyOutput[channel].resize(256);
            keySeek[channel].resize(8192);
        }
    }

    void waitForRender() const noexcept
    {
        while (activeRenders.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
    }
};

Deck::Deck() : impl_(std::make_unique<Impl>()) {}

Deck::~Deck()
{
    playing.store(false, std::memory_order_release);
    impl_->renderGate.store(false, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst); // see loadTrack()
    impl_->waitForRender();
}

void Deck::loadTrack(TrackDataPtr track)
{
    // The render gate also covers tail-only callbacks after transport stops.
    // Closing it before draining protects both the PCM pointer and FX state.
    playing.store(false, std::memory_order_release);
    impl_->renderGate.store(false, std::memory_order_release);
    // StoreLoad barrier: without it this thread can read activeRenders==0 from
    // its store buffer while the audio thread still sees renderGate==true
    // (Dekker's pattern) — and we'd free PCM mid-render.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    impl_->waitForRender();

    impl_->audioTrack.store(nullptr, std::memory_order_release);
    impl_->audioStems.store(nullptr, std::memory_order_release);
    impl_->ownedStems.reset();
    impl_->ownedTrack = std::move(track);
    impl_->positionFrames.store(0.0, std::memory_order_release);
    impl_->pendingJogRatio.store(0.0, std::memory_order_release);
    impl_->scratchActive.store(false, std::memory_order_release);
    impl_->scratchResumePlaying.store(false, std::memory_order_release);
    impl_->pendingScratchFrames.store(0.0, std::memory_order_release);
    impl_->cuePreviewing.store(false, std::memory_order_release);
    impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
    impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
    impl_->transitionCueSecs.fill(-1.0);
    impl_->transitionLoopEndSecs.fill(-1.0);
    impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
    impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
    channelLevel.store(0.0f, std::memory_order_release);
    impl_->jogRatio = 0.0;
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;
    impl_->eq.reset();
    impl_->djFilter.reset();
    impl_->fx.reset();
    impl_->keyLock.reset();
    impl_->keyInputRemainder = 0.0;
    impl_->keyExpectedPositionFrames = 0.0;
    impl_->keyLockPrimed = false;
    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(-1.0, std::memory_order_release);
    loopEndSec.store(-1.0, std::memory_order_release);

    TrackData* const rawTrack = impl_->ownedTrack.get();
    if (rawTrack == nullptr) {
        cuePointSec.store(-1.0, std::memory_order_release);
        impl_->trackBpm.store(0.0, std::memory_order_release);
        impl_->firstBeatSec.store(0.0, std::memory_order_release);
        impl_->trackFrameCount.store(0, std::memory_order_release);
        impl_->renderGate.store(true, std::memory_order_release);
        return;
    }

    cuePointSec.store(rawTrack->firstBeatSec, std::memory_order_release);
    impl_->trackBpm.store(rawTrack->bpm, std::memory_order_release);
    impl_->firstBeatSec.store(rawTrack->firstBeatSec, std::memory_order_release);
    impl_->trackFrameCount.store(rawTrack->frameCount(), std::memory_order_release);
    impl_->audioTrack.store(rawTrack, std::memory_order_release);
    impl_->renderGate.store(true, std::memory_order_release);
}

void Deck::attachStems(StemSetPtr stems)
{
    TrackData* const rawTrack = impl_->ownedTrack.get();
    if (stems) {
        if (rawTrack == nullptr) return;
        const int64_t diff = std::llabs(stems->frameCount() - rawTrack->frameCount());
        if (diff > (int64_t)kSampleRate) {
            qWarning("attachStems: stem length differs from track by %.1fs — refused",
                     (double)diff / kSampleRate);
            return;
        }
    }
    if (impl_->audioStems.load(std::memory_order_acquire) != nullptr) {
        // Replacing/detaching: drain the audio thread before the old set dies.
        impl_->renderGate.store(false, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        impl_->waitForRender();
        impl_->audioStems.store(nullptr, std::memory_order_release);
        impl_->ownedStems.reset();
        impl_->renderGate.store(true, std::memory_order_release);
    }
    if (stems) {
        // Attach-from-null is seamless: retain, then publish.
        impl_->ownedStems = std::move(stems);
        impl_->audioStems.store(impl_->ownedStems.get(), std::memory_order_release);
    }
}

bool Deck::stemsAttached() const
{
    return impl_->audioStems.load(std::memory_order_acquire) != nullptr;
}

TrackDataPtr Deck::track() const
{
    return impl_->ownedTrack;
}

void Deck::play()
{
    startPlayback(true);
}

void Deck::startPlayback(bool latchPreview)
{
    if (impl_->audioTrack.load(std::memory_order_acquire) == nullptr)
        return;
    if (latchPreview) {
        // PLAY takes ownership of an active momentary preview. Clearing these
        // markers makes the later CUE/pad release a no-op, so transport keeps
        // rolling from the preview position.
        impl_->cuePreviewing.store(false, std::memory_order_release);
        impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
        impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
        impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
        impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
    }
    // At end-of-track the render callback would immediately clear `playing`
    // again — rewind so play-after-EOF restarts instead of doing nothing.
    const auto frames = impl_->trackFrameCount.load(std::memory_order_acquire);
    if (impl_->positionFrames.load(std::memory_order_acquire) >=
        (double)frames - 1.0)
        impl_->positionFrames.store(0.0, std::memory_order_release);
    if (impl_->scratchActive.load(std::memory_order_acquire)) {
        // PLAY while the platter is held means "continue when released";
        // ordinary forward transport must remain suspended during scratching.
        impl_->scratchResumePlaying.store(true, std::memory_order_release);
        playing.store(false, std::memory_order_release);
    } else {
        playing.store(true, std::memory_order_release);
    }
}

bool Deck::previewActive() const
{
    return impl_->cuePreviewing.load(std::memory_order_acquire) ||
           impl_->hotCuePreviewIndex.load(std::memory_order_acquire) >= 0 ||
           impl_->savedLoopPreviewIndex.load(std::memory_order_acquire) >= 0;
}

void Deck::stop()
{
    impl_->scratchResumePlaying.store(false, std::memory_order_release);
    playing.store(false, std::memory_order_release);
}

void Deck::handleCue(bool pressed)
{
    const TrackDataPtr currentTrack = track();
    if (!currentTrack)
        return;

    if (!pressed) {
        if (!impl_->cuePreviewing.exchange(false, std::memory_order_acq_rel))
            return;

        stop();
        cueJump();
        return;
    }

    if (playing.load(std::memory_order_acquire)) {
        impl_->cuePreviewing.store(false, std::memory_order_release);
        stop();

        double targetSec = cuePointSec.load(std::memory_order_acquire);
        if (!std::isfinite(targetSec) || targetSec < 0.0)
            targetSec = currentTrack->firstBeatSec;
        seekSec(targetSec);
        return;
    }

    const double targetSec = cuePointSec.load(std::memory_order_acquire);
    const double currentSec = positionSec();
    if (std::isfinite(targetSec) && targetSec >= 0.0 &&
        std::abs(currentSec - targetSec) <= kCuePreviewToleranceSec) {
        impl_->cuePreviewing.store(true, std::memory_order_release);
        startPlayback(false);
        return;
    }

    impl_->cuePreviewing.store(false, std::memory_order_release);
    cuePointSec.store(currentSec, std::memory_order_release);
}

void Deck::cueJump()
{
    if (!track())
        return;

    const double cueSec = cuePointSec.load(std::memory_order_acquire);
    if (!std::isfinite(cueSec) || cueSec < 0.0)
        return;
    seekSec(cueSec);
}

void Deck::handleHotCue(int index, bool pressed)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (!currentTrack)
        return;

    if (!pressed) {
        if (impl_->hotCuePreviewIndex.exchange(
                -1, std::memory_order_acq_rel) != index)
            return;

        const double returnSec = impl_->hotCuePreviewSec.exchange(
            -1.0, std::memory_order_acq_rel);
        stop();
        if (std::isfinite(returnSec) && returnSec >= 0.0)
            seekSec(returnSec);
        return;
    }

    const double storedCueSec = currentTrack->hotCues[index];
    if (!std::isfinite(storedCueSec) || storedCueSec < 0.0) {
        impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
        impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
        setHotCue(index);
        return;
    }
    const double cueSec = quantizeHotCues.load(std::memory_order_acquire)
        ? snapToPlayableBeat(currentTrack, storedCueSec)
        : storedCueSec;

    impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
    impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
    impl_->hotCuePreviewSec.store(cueSec, std::memory_order_release);
    impl_->hotCuePreviewIndex.store(index, std::memory_order_release);
    seekSec(cueSec);
    startPlayback(false);
}

void Deck::setTransitionCues(const std::array<double, 8>& seconds)
{
    impl_->transitionCueSecs = seconds;
    impl_->transitionLoopEndSecs.fill(-1.0);
}

void Deck::setTransitionPerformanceSlots(
    const std::array<double, 8>& startSeconds,
    const std::array<double, 8>& endSeconds)
{
    impl_->transitionCueSecs = startSeconds;
    impl_->transitionLoopEndSecs = endSeconds;
}

void Deck::clearTransitionCues()
{
    impl_->transitionCueSecs.fill(-1.0);
    impl_->transitionLoopEndSecs.fill(-1.0);
}

void Deck::handleTransitionCue(int index, bool pressed)
{
    if (index < 0 || index >= 8 || !track()) return;
    const double startSec =
        impl_->transitionCueSecs[static_cast<std::size_t>(index)];
    const double endSec =
        impl_->transitionLoopEndSecs[static_cast<std::size_t>(index)];
    const bool savedLoop = std::isfinite(startSec) && startSec >= 0.0 &&
                           std::isfinite(endSec) && endSec > startSec;
    if (savedLoop) {
        if (!pressed) {
            if (impl_->savedLoopPreviewIndex.exchange(
                    -1, std::memory_order_acq_rel) != index)
                return;
            const double returnSec = impl_->savedLoopPreviewSec.exchange(
                -1.0, std::memory_order_acq_rel);
            stop();
            if (std::isfinite(returnSec) && returnSec >= 0.0)
                seekSec(returnSec);
            return;
        }

        const bool alreadyPlaying =
            playing.load(std::memory_order_acquire) && !previewActive();
        if (alreadyPlaying) {
            impl_->cuePreviewing.store(false, std::memory_order_release);
            impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
            impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
            impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
            impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
            armSavedLoop(startSec, endSec);
            return;
        }
        if (!activateSavedLoop(startSec, endSec)) return;
        impl_->cuePreviewing.store(false, std::memory_order_release);
        impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
        impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
        impl_->savedLoopPreviewIndex.store(index, std::memory_order_release);
        impl_->savedLoopPreviewSec.store(startSec, std::memory_order_release);
        seekSec(startSec);
        startPlayback(false);
        return;
    }
    if (!pressed) {
        if (impl_->hotCuePreviewIndex.exchange(
                -1, std::memory_order_acq_rel) != index)
            return;
        const double returnSec = impl_->hotCuePreviewSec.exchange(
            -1.0, std::memory_order_acq_rel);
        stop();
        if (std::isfinite(returnSec) && returnSec >= 0.0) seekSec(returnSec);
        return;
    }

    const double cueSec = startSec;
    if (!std::isfinite(cueSec) || cueSec < 0.0) return;
    impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
    impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
    impl_->hotCuePreviewSec.store(cueSec, std::memory_order_release);
    impl_->hotCuePreviewIndex.store(index, std::memory_order_release);
    seekSec(cueSec); // exact semantic cue; never quantize away fractional beats
    startPlayback(false);
}

void Deck::handleSavedLoop(int index, bool pressed)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (!currentTrack)
        return;

    if (!pressed) {
        if (impl_->savedLoopPreviewIndex.exchange(
                -1, std::memory_order_acq_rel) != index)
            return;

        const double returnSec = impl_->savedLoopPreviewSec.exchange(
            -1.0, std::memory_order_acq_rel);
        stop();
        if (std::isfinite(returnSec) && returnSec >= 0.0)
            seekSec(returnSec);
        return;
    }

    const SavedLoopSlot& slot = currentTrack->savedLoops[index];
    const bool alreadyPlaying =
        playing.load(std::memory_order_acquire) && !previewActive();
    if (alreadyPlaying) {
        // A live deck must not jump just because the DJ selected a saved loop.
        // Store/activate its region and let normal playback enter it; the
        // render loop will wrap naturally at OUT.
        impl_->cuePreviewing.store(false, std::memory_order_release);
        impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
        impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
        impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
        impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
        if (slot.isSet())
            armSavedLoop(slot.startSec, slot.endSec);
        return;
    }

    if (!slot.isSet() || !activateSavedLoop(slot.startSec, slot.endSec)) {
        impl_->savedLoopPreviewIndex.store(-1, std::memory_order_release);
        impl_->savedLoopPreviewSec.store(-1.0, std::memory_order_release);
        return;
    }

    const double loopStart = loopStartSec.load(std::memory_order_acquire);
    impl_->cuePreviewing.store(false, std::memory_order_release);
    impl_->hotCuePreviewIndex.store(-1, std::memory_order_release);
    impl_->hotCuePreviewSec.store(-1.0, std::memory_order_release);
    impl_->savedLoopPreviewIndex.store(index, std::memory_order_release);
    impl_->savedLoopPreviewSec.store(loopStart, std::memory_order_release);
    seekSec(loopStart);
    startPlayback(false);
}

void Deck::setHotCue(int index)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (currentTrack) {
        const double currentSec = positionSec();
        currentTrack->hotCues[index] =
            quantizeHotCues.load(std::memory_order_acquire)
            ? snapToPlayableBeat(currentTrack, currentSec)
            : currentSec;
    }
}

void Deck::jumpHotCue(int index)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (currentTrack && currentTrack->hotCues[index] >= 0.0) {
        const double storedCueSec = currentTrack->hotCues[index];
        seekSec(quantizeHotCues.load(std::memory_order_acquire)
                    ? snapToPlayableBeat(currentTrack, storedCueSec)
                    : storedCueSec);
    }
}

void Deck::nudge(double ticks)
{
    if (!std::isfinite(ticks) || ticks == 0.0)
        return;

    const double delta = ticks * kJogRatioPerTick;
    double pending = impl_->pendingJogRatio.load(std::memory_order_relaxed);
    double desired = 0.0;
    do {
        desired = std::clamp(pending + delta,
                             -kMaximumJogRatio, kMaximumJogRatio);
    } while (!impl_->pendingJogRatio.compare_exchange_weak(
        pending, desired, std::memory_order_release, std::memory_order_relaxed));
}

void Deck::scratch(double ticks)
{
    if (!std::isfinite(ticks) || ticks == 0.0)
        return;

    const int64_t trackFrames =
        impl_->trackFrameCount.load(std::memory_order_acquire);
    if (trackFrames <= 0)
        return;

    const double deltaFrames = ticks * kPlatterScratchSecondsPerTick *
                               static_cast<double>(kSampleRate);
    if (impl_->scratchActive.load(std::memory_order_acquire)) {
        double pending = impl_->pendingScratchFrames.load(
            std::memory_order_relaxed);
        while (!impl_->pendingScratchFrames.compare_exchange_weak(
            pending, pending + deltaFrames, std::memory_order_release,
            std::memory_order_relaxed)) {
        }
        return;
    }

    // Keep the direct positional fallback for synthetic/UI callers that do
    // not have a separate touch gesture. The FLX4 path always brackets wheel
    // movement with beginScratch()/endScratch() and therefore renders audio.
    const double trackEndFrame = static_cast<double>(trackFrames);
    double lowerFrame = 0.0;
    double upperFrame = std::nextafter(trackEndFrame, 0.0);

    // Unlike an ordinary seek, scratching across a loop edge must not disable
    // the loop. Keep the platter position inside the active half-open bounds.
    if (loopActive.load(std::memory_order_acquire)) {
        const double startSec = loopStartSec.load(std::memory_order_acquire);
        const double endSec = loopEndSec.load(std::memory_order_acquire);
        const double startFrame = startSec * static_cast<double>(kSampleRate);
        const double endFrame = endSec * static_cast<double>(kSampleRate);
        if (std::isfinite(startFrame) && std::isfinite(endFrame) &&
            endFrame > startFrame) {
            const double boundedStart =
                std::clamp(startFrame, 0.0, upperFrame);
            const double boundedEnd =
                std::clamp(endFrame, 0.0, trackEndFrame);
            if (boundedEnd > boundedStart) {
                lowerFrame = boundedStart;
                upperFrame = std::nextafter(boundedEnd, boundedStart);
            }
        }
    }

    double current = impl_->positionFrames.load(std::memory_order_acquire);
    double desired = lowerFrame;
    do {
        if (!std::isfinite(current))
            current = lowerFrame;
        desired = std::clamp(current + deltaFrames, lowerFrame, upperFrame);
    } while (!impl_->positionFrames.compare_exchange_weak(
        current, desired, std::memory_order_release, std::memory_order_acquire));
}

void Deck::beginScratch()
{
    if (impl_->audioTrack.load(std::memory_order_acquire) == nullptr)
        return;
    if (impl_->scratchActive.exchange(true, std::memory_order_acq_rel))
        return;

    impl_->pendingScratchFrames.store(0.0, std::memory_order_release);
    const bool resume = playing.exchange(false, std::memory_order_acq_rel);
    impl_->scratchResumePlaying.store(resume, std::memory_order_release);
}

void Deck::endScratch()
{
    if (!impl_->scratchActive.exchange(false, std::memory_order_acq_rel))
        return;

    impl_->pendingScratchFrames.store(0.0, std::memory_order_release);
    const bool resume = impl_->scratchResumePlaying.exchange(
        false, std::memory_order_acq_rel);
    if (resume && impl_->audioTrack.load(std::memory_order_acquire) != nullptr)
        playing.store(true, std::memory_order_release);
}

void Deck::updateBeatGrid(double bpm, double firstBeatSec)
{
    if (!std::isfinite(bpm) || bpm <= 0.0 ||
        !std::isfinite(firstBeatSec)) {
        return;
    }

    // Beat-based GUI actions read TrackData, while realtime timing/sync/FX
    // read the copied atomics. Publish both halves of the live grid together
    // so a manual correction takes effect without unloading the track.
    if (impl_->ownedTrack) {
        impl_->ownedTrack->bpm = bpm;
        impl_->ownedTrack->firstBeatSec = firstBeatSec;
    }
    impl_->trackBpm.store(bpm, std::memory_order_release);
    impl_->firstBeatSec.store(firstBeatSec, std::memory_order_release);
}

void Deck::loopAuto(double beats)
{
    const TrackDataPtr currentTrack = track();
    if (!hasUsableBeatGrid(currentTrack) || !std::isfinite(beats))
        return;

    beats = std::clamp(beats, kMinimumLoopBeats, kMaximumLoopBeats);
    const double currentBeat = currentTrack->beatAtSec(positionSec());
    if (!std::isfinite(currentBeat))
        return;

    const double start = clampTrackSec(
        *currentTrack, currentTrack->secAtBeat(std::floor(currentBeat)));
    const double beatDuration = 60.0 / currentTrack->bpm;
    const double end = clampTrackSec(
        *currentTrack, start + beats * beatDuration);
    if (!(end > start))
        return;

    // Disable first so the audio thread never observes a newly written start
    // paired with the previous end.
    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(start, std::memory_order_release);
    loopEndSec.store(end, std::memory_order_release);
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;
    loopActive.store(true, std::memory_order_release);
}

void Deck::loopIn()
{
    const TrackDataPtr currentTrack = track();
    if (!currentTrack || currentTrack->frameCount() <= 0)
        return;

    const double current = clampTrackSec(*currentTrack, positionSec());
    const double start = quantizeHotCues.load(std::memory_order_acquire)
        ? snapToPlayableBeat(currentTrack, current) : current;
    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(start, std::memory_order_release);
    loopEndSec.store(-1.0, std::memory_order_release);
    impl_->pendingLoopInSec = start;
    impl_->hasPendingLoopIn = true;
}

void Deck::loopOut()
{
    const TrackDataPtr currentTrack = track();
    if (!currentTrack || currentTrack->frameCount() <= 0 ||
        !impl_->hasPendingLoopIn)
        return;

    const double current = positionSec();
    const double start = impl_->pendingLoopInSec;
    if (!std::isfinite(current) || !(current > start))
        return;

    const bool quantized = quantizeHotCues.load(std::memory_order_acquire) &&
                           hasUsableBeatGrid(currentTrack);
    double end = quantized
        ? snapToPlayableBeat(currentTrack, current)
        : clampTrackSec(*currentTrack, current);
    if (quantized && !(end > start)) {
        // IN is itself on a whole beat, so the shortest quantized manual loop
        // is the following whole beat. Never manufacture an off-grid OUT.
        end = clampTrackSec(
            *currentTrack,
            currentTrack->secAtBeat(
                std::floor(currentTrack->beatAtSec(start) + 1.0e-7) + 1.0));
    } else if (!quantized && hasUsableBeatGrid(currentTrack)) {
        const double minimumEnd =
            start + kMinimumLoopBeats * 60.0 / currentTrack->bpm;
        end = clampTrackSec(*currentTrack, std::max(end, minimumEnd));
    }
    if (!(end > start))
        return;

    loopEndSec.store(end, std::memory_order_release);
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;
    loopActive.store(true, std::memory_order_release);
}

void Deck::loopExit()
{
    // Bounds deliberately survive. LoopIn starts a fresh manual-loop arm.
    loopActive.store(false, std::memory_order_release);
}

void Deck::loopHalve()
{
    if (!loopActive.load(std::memory_order_acquire))
        return;

    const TrackDataPtr currentTrack = track();
    if (!hasUsableBeatGrid(currentTrack))
        return;

    const double start = loopStartSec.load(std::memory_order_acquire);
    const double end = loopEndSec.load(std::memory_order_acquire);
    if (!std::isfinite(start) || !std::isfinite(end) || !(end > start))
        return;

    const double lengthBeats = (end - start) * currentTrack->bpm / 60.0;
    const double newLengthBeats =
        std::max(kMinimumLoopBeats, lengthBeats * 0.5);
    const double newEnd = clampTrackSec(
        *currentTrack, start + newLengthBeats * 60.0 / currentTrack->bpm);
    if (!(newEnd > start))
        return;

    loopEndSec.store(newEnd, std::memory_order_release);
}

void Deck::loopDouble()
{
    if (!loopActive.load(std::memory_order_acquire))
        return;

    const TrackDataPtr currentTrack = track();
    if (!hasUsableBeatGrid(currentTrack))
        return;

    const double start = loopStartSec.load(std::memory_order_acquire);
    const double end = loopEndSec.load(std::memory_order_acquire);
    if (!std::isfinite(start) || !std::isfinite(end) || !(end > start))
        return;

    const double lengthBeats = (end - start) * currentTrack->bpm / 60.0;
    const double newLengthBeats =
        std::min(kMaximumLoopBeats, lengthBeats * 2.0);
    const double newEnd = clampTrackSec(
        *currentTrack, start + newLengthBeats * 60.0 / currentTrack->bpm);
    if (!(newEnd > start))
        return;

    loopEndSec.store(newEnd, std::memory_order_release);
}

bool Deck::activateSavedLoop(double startSec, double endSec)
{
    const TrackDataPtr currentTrack = track();
    if (!currentTrack || !std::isfinite(startSec) ||
        !std::isfinite(endSec)) {
        return false;
    }

    const double duration = trackDurationSec(*currentTrack);
    startSec = std::clamp(startSec, 0.0, duration);
    endSec = std::clamp(endSec, 0.0, duration);
    if (!(endSec > startSec))
        return false;

    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(startSec, std::memory_order_release);
    loopEndSec.store(endSec, std::memory_order_release);
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;

    const double position = positionSec();
    if (position < startSec || position >= endSec)
        seekSec(startSec);
    loopActive.store(true, std::memory_order_release);
    return true;
}

bool Deck::armSavedLoop(double startSec, double endSec)
{
    const TrackDataPtr currentTrack = track();
    if (!currentTrack || !std::isfinite(startSec) ||
        !std::isfinite(endSec)) {
        return false;
    }

    const double duration = trackDurationSec(*currentTrack);
    startSec = std::clamp(startSec, 0.0, duration);
    endSec = std::clamp(endSec, 0.0, duration);
    if (!(endSec > startSec))
        return false;

    // Arming a region that has already passed must not make the next audio
    // callback wrap backwards. Leave it inactive and let the UI explain that
    // pausing is the way to jump to an earlier saved loop.
    if (positionSec() >= endSec) {
        loopActive.store(false, std::memory_order_release);
        return false;
    }

    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(startSec, std::memory_order_release);
    loopEndSec.store(endSec, std::memory_order_release);
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;
    loopActive.store(true, std::memory_order_release);
    return true;
}

bool Deck::retriggerSavedLoop(double startSec, double endSec)
{
    if (!activateSavedLoop(startSec, endSec))
        return false;

    // Saved-loop pads behave like one-shot hot cues: every press returns to
    // the authored IN point and leaves transport running. Loop EXIT remains
    // the explicit way to disengage the loop.
    seekSec(loopStartSec.load(std::memory_order_acquire));
    play();
    return true;
}

void Deck::beatJump(double beats)
{
    const TrackDataPtr currentTrack = track();
    if (!hasUsableBeatGrid(currentTrack) || !std::isfinite(beats))
        return;

    const double currentBeat = currentTrack->beatAtSec(positionSec());
    if (!std::isfinite(currentBeat))
        return;

    const double duration = trackDurationSec(*currentTrack);
    if (!(duration > 0.0))
        return;

    const double finalPlayableSec = std::nextafter(duration, 0.0);
    const double targetSec = std::clamp(
        currentTrack->secAtBeat(std::round(currentBeat) + beats),
        0.0, finalPlayableSec);
    seekSec(targetSec);
}

void Deck::render(float* out, int frames, float* preFaderOut)
{
    if (out == nullptr || frames <= 0)
        return;

    std::fill_n(out, static_cast<std::size_t>(frames) * 2U, 0.0f);
    if (preFaderOut != nullptr)
        std::fill_n(preFaderOut, static_cast<std::size_t>(frames) * 2U, 0.0f);

    // A stopped deck normally costs nothing, except while an effect has stored
    // energy to release. Tail-only renders use zero input and never advance the
    // track position.
    const bool scratchRequested =
        impl_->scratchActive.load(std::memory_order_acquire);
    if (!impl_->renderGate.load(std::memory_order_acquire) ||
        (!playing.load(std::memory_order_acquire) && !scratchRequested &&
         !impl_->fx.hasTail())) {
        channelLevel.store(0.0f, std::memory_order_relaxed);
        return;
    }

    impl_->activeRenders.fetch_add(1, std::memory_order_acq_rel);
    struct ActiveRenderGuard {
        std::atomic<unsigned int>& counter;
        ~ActiveRenderGuard() { counter.fetch_sub(1, std::memory_order_release); }
    } activeRenderGuard {impl_->activeRenders};

    // StoreLoad barrier pairing with loadTrack(): the re-check below must not
    // read a stale open gate after publishing activeRenders.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!impl_->renderGate.load(std::memory_order_acquire))
        return;

    const bool scratching =
        impl_->scratchActive.load(std::memory_order_acquire);
    bool renderTrack = scratching || playing.load(std::memory_order_acquire);
    TrackData* currentTrack = renderTrack
        ? impl_->audioTrack.load(std::memory_order_acquire)
        : nullptr;
    if (renderTrack &&
        (currentTrack == nullptr || currentTrack->frameCount() <= 0)) {
        playing.store(false, std::memory_order_release);
        renderTrack = false;
    }
    if (!renderTrack && !impl_->fx.hasTail())
        return;

    double ratio = tempoRatio.load(std::memory_order_relaxed);
    if (!std::isfinite(ratio) || ratio <= 0.0)
        ratio = 1.0;
    ratio = std::clamp(ratio, 0.01, 4.0);

    const double startPosition =
        impl_->positionFrames.load(std::memory_order_acquire);
    double position = std::isfinite(startPosition)
        ? std::max(0.0, startPosition)
        : 0.0;
    bool reachedEnd = false;

    if (renderTrack) {
        double scratchAdvance = 0.0;
        bool scratchHasMotion = false;
        if (scratching) {
            const double requested = impl_->pendingScratchFrames.exchange(
                0.0, std::memory_order_acq_rel);
            const double maximum =
                static_cast<double>(frames) *
                kMaximumScratchFramesPerOutputFrame;
            const double movement = std::clamp(requested, -maximum, maximum);
            const double remainder = requested - movement;
            if (std::abs(remainder) > 1.0e-9) {
                double pending = impl_->pendingScratchFrames.load(
                    std::memory_order_relaxed);
                while (!impl_->pendingScratchFrames.compare_exchange_weak(
                    pending, pending + remainder, std::memory_order_release,
                    std::memory_order_relaxed)) {
                }
            }
            scratchAdvance = movement / static_cast<double>(frames);
            scratchHasMotion = std::abs(scratchAdvance) > 1.0e-9;
        } else {
            impl_->jogRatio = std::clamp(
                impl_->jogRatio + impl_->pendingJogRatio.exchange(
                    0.0, std::memory_order_acq_rel),
                -kMaximumJogRatio, kMaximumJogRatio);
        }

        const int64_t trackFrames = currentTrack->frameCount();
        const double inputGain = trimGain(
            trim.load(std::memory_order_relaxed));
        const StemSet* const stems =
            impl_->audioStems.load(std::memory_order_acquire);
        const double gV = std::clamp((double)stemVocals.load(std::memory_order_relaxed), 0.0, 1.0);
        const double gM = std::clamp((double)stemMelody.load(std::memory_order_relaxed), 0.0, 1.0);
        const double gB = std::clamp((double)stemBass.load(std::memory_order_relaxed), 0.0, 1.0);
        const double gD = std::clamp((double)stemDrums.load(std::memory_order_relaxed), 0.0, 1.0);
        // All-full gains play the untouched master; anything less mixes stems.
        const bool useStems = stems != nullptr &&
            (gV < 0.99 || gM < 0.99 || gB < 0.99 || gD < 0.99);
        const int64_t stemFrames = useStems ? stems->frameCount() : 0;
        bool loopEnabled = loopActive.load(std::memory_order_acquire);
        double loopStartFrame = 0.0;
        double loopEndFrame = 0.0;
        double loopLengthFrames = 0.0;
        if (loopEnabled) {
            loopStartFrame = std::clamp(
                loopStartSec.load(std::memory_order_acquire) *
                    static_cast<double>(kSampleRate),
                0.0, static_cast<double>(trackFrames));
            loopEndFrame = std::clamp(
                loopEndSec.load(std::memory_order_acquire) *
                    static_cast<double>(kSampleRate),
                0.0, static_cast<double>(trackFrames));
            loopLengthFrames = loopEndFrame - loopStartFrame;
            if (!std::isfinite(loopStartFrame) ||
                !std::isfinite(loopEndFrame) ||
                !(loopLengthFrames > 0.0)) {
                loopEnabled = false;
            }
        }

        const auto wrapLoopPosition = [&] {
            if (loopEnabled && position >= loopEndFrame) {
                const double overshoot =
                    std::fmod(position - loopStartFrame, loopLengthFrames);
                position = loopStartFrame + (overshoot >= 0.0
                    ? overshoot : overshoot + loopLengthFrames);
            }
        };

        const auto clampScratchPosition = [&] {
            const double lower = loopEnabled ? loopStartFrame : 0.0;
            const double exclusiveEnd = loopEnabled
                ? loopEndFrame : static_cast<double>(trackFrames);
            const double upper = std::nextafter(exclusiveEnd, lower);
            position = std::clamp(position, lower, upper);
        };

        const auto sourceSample = [&](double sourcePosition,
                                      int channel) -> float {
            sourcePosition = std::clamp(
                sourcePosition, 0.0,
                std::nextafter(static_cast<double>(trackFrames), 0.0));
            const int64_t frame0 = static_cast<int64_t>(sourcePosition);
            const int64_t frame1 = std::min(frame0 + 1, trackFrames - 1);
            const double fraction =
                sourcePosition - static_cast<double>(frame0);
            if (useStems) {
                const int64_t sf0 = std::min(frame0, stemFrames - 1);
                const int64_t sf1 = std::min(frame1, stemFrames - 1);
                const std::size_t a = static_cast<std::size_t>(sf0) * 2U +
                                      static_cast<std::size_t>(channel);
                const std::size_t b = static_cast<std::size_t>(sf1) * 2U +
                                      static_cast<std::size_t>(channel);
                constexpr double kInv = 1.0 / 32768.0;
                const auto stemSample = [&](const std::vector<int16_t>& samples,
                                            double gain) {
                    if (gain <= 0.0 || samples.empty()) return 0.0;
                    const double first = samples[a] * kInv;
                    const double second = samples[b] * kInv;
                    return (first + (second - first) * fraction) * gain;
                };
                return static_cast<float>(
                    stemSample(stems->vocals, gV) +
                    stemSample(stems->melody, gM) +
                    stemSample(stems->bass, gB) +
                    stemSample(stems->drums, gD));
            }

            const std::size_t sample0 =
                static_cast<std::size_t>(frame0) * 2U +
                static_cast<std::size_t>(channel);
            const std::size_t sample1 =
                static_cast<std::size_t>(frame1) * 2U +
                static_cast<std::size_t>(channel);
            const double first = currentTrack->pcm[sample0];
            const double second = currentTrack->pcm[sample1];
            return static_cast<float>(first + (second - first) * fraction);
        };

        const bool keyLockEnabled =
            preservePitch.load(std::memory_order_relaxed) && !scratching;
        if (keyLockEnabled) {
            const double playbackRatio =
                std::clamp(ratio + impl_->jogRatio, 0.01, 4.0);
            const bool positionDiscontinuity =
                !impl_->keyLockPrimed ||
                std::fabs(startPosition - impl_->keyExpectedPositionFrames) >
                    1.1;

            if (positionDiscontinuity) {
                impl_->keyInputRemainder = 0.0;
                const int seekFrames = std::max(
                    1, static_cast<int>(std::ceil(
                           impl_->keyLock.outputSeekLength(playbackRatio))));
                for (int channel = 0; channel < 2; ++channel)
                    impl_->keySeek[channel].resize(
                        static_cast<std::size_t>(seekFrames));
                const double seekStart = position - seekFrames;
                for (int frame = 0; frame < seekFrames; ++frame) {
                    const double sourcePosition = seekStart + frame;
                    for (int channel = 0; channel < 2; ++channel) {
                        impl_->keySeek[channel][static_cast<std::size_t>(frame)] =
                            sourcePosition >= 0.0 &&
                            sourcePosition < static_cast<double>(trackFrames)
                                ? sourceSample(sourcePosition, channel)
                                : 0.0f;
                    }
                }
                std::array<float*, 2> seekPointers {
                    impl_->keySeek[0].data(), impl_->keySeek[1].data()};
                impl_->keyLock.outputSeek(seekPointers, seekFrames);
                impl_->keyLockPrimed = true;
            }

            const double wantedInput =
                static_cast<double>(frames) * playbackRatio +
                impl_->keyInputRemainder;
            const int inputFrames = std::max(
                0, static_cast<int>(std::floor(wantedInput)));
            impl_->keyInputRemainder = wantedInput - inputFrames;
            for (int channel = 0; channel < 2; ++channel) {
                impl_->keyInput[channel].resize(
                    static_cast<std::size_t>(inputFrames));
                impl_->keyOutput[channel].resize(
                    static_cast<std::size_t>(frames));
            }

            for (int frame = 0; frame < inputFrames; ++frame) {
                wrapLoopPosition();
                if (position >= static_cast<double>(trackFrames)) {
                    position = static_cast<double>(trackFrames);
                    reachedEnd = true;
                    for (int channel = 0; channel < 2; ++channel)
                        impl_->keyInput[channel][static_cast<std::size_t>(frame)] =
                            0.0f;
                    continue;
                }
                for (int channel = 0; channel < 2; ++channel)
                    impl_->keyInput[channel][static_cast<std::size_t>(frame)] =
                        sourceSample(position, channel);
                position += 1.0;
                wrapLoopPosition();
            }

            std::array<float*, 2> inputPointers {
                impl_->keyInput[0].data(), impl_->keyInput[1].data()};
            std::array<float*, 2> outputPointers {
                impl_->keyOutput[0].data(), impl_->keyOutput[1].data()};
            impl_->keyLock.process(inputPointers, inputFrames,
                                   outputPointers, frames);
            for (int frame = 0; frame < frames; ++frame) {
                const int outputBase = frame * 2;
                for (int channel = 0; channel < 2; ++channel)
                    out[outputBase + channel] =
                        impl_->keyOutput[channel][static_cast<std::size_t>(frame)] *
                        static_cast<float>(inputGain);
            }
            impl_->keyExpectedPositionFrames = position;
            impl_->jogRatio *= std::pow(kJogDecayPerFrame, frames);
            if (std::abs(impl_->jogRatio) < 1.0e-8)
                impl_->jogRatio = 0.0;
        } else {
            impl_->keyLockPrimed = false;
            impl_->keyInputRemainder = 0.0;
            for (int frame = 0; frame < frames; ++frame) {
                if (scratching)
                    clampScratchPosition();
                else
                    wrapLoopPosition();
                if (scratching && !scratchHasMotion)
                    break;
                if (position >= static_cast<double>(trackFrames)) {
                    position = static_cast<double>(trackFrames);
                    reachedEnd = true;
                    break;
                }

                const int outputBase = frame * 2;
                for (int channel = 0; channel < 2; ++channel)
                    out[outputBase + channel] =
                        sourceSample(position, channel) *
                        static_cast<float>(inputGain);

                if (scratching) {
                    position += scratchAdvance;
                    clampScratchPosition();
                } else {
                    const double playbackRatio =
                        std::clamp(ratio + impl_->jogRatio, 0.01, 4.0);
                    position += playbackRatio;
                    wrapLoopPosition();
                    impl_->jogRatio *= kJogDecayPerFrame;
                    if (std::abs(impl_->jogRatio) < 1.0e-8)
                        impl_->jogRatio = 0.0;
                }
            }
        }

        if (scratching) {
            clampScratchPosition();
        } else if (position >= static_cast<double>(trackFrames)) {
            position = static_cast<double>(trackFrames);
            reachedEnd = true;
        }

        impl_->eq.process(out, frames,
                          eqLow.load(std::memory_order_relaxed),
                          eqMid.load(std::memory_order_relaxed),
                          eqHigh.load(std::memory_order_relaxed));
        impl_->djFilter.process(
            out, frames, filter.load(std::memory_order_relaxed));
    }

    const double trackBpm = impl_->trackBpm.load(std::memory_order_relaxed);
    const double bpm = std::isfinite(trackBpm) && trackBpm > 0.0
        ? trackBpm * ratio : 0.0;
    impl_->fx.process(
        out, frames,
        fxType.load(std::memory_order_relaxed),
        fxOn.load(std::memory_order_relaxed),
        fxWet.load(std::memory_order_relaxed),
        fxBeats.load(std::memory_order_relaxed), bpm);

    float peak = 0.0f;
    for (std::size_t sample = 0;
         sample < static_cast<std::size_t>(frames) * 2U; ++sample)
        peak = std::max(peak, std::fabs(out[sample]));
    const float previousLevel = channelLevel.load(std::memory_order_relaxed);
    channelLevel.store(std::clamp(std::max(peak, previousLevel * 0.94f),
                                  0.0f, 1.0f),
                       std::memory_order_relaxed);

    // Headphone PFL is post-EQ/filter/FX but pre-channel-fader. Copy the one
    // render result so monitoring never advances the deck a second time.
    if (preFaderOut != nullptr)
        std::copy_n(out, static_cast<std::size_t>(frames) * 2U, preFaderOut);

    const float channelGain =
        clampUnit(fader.load(std::memory_order_relaxed), 1.0f);
    for (std::size_t sample = 0;
         sample < static_cast<std::size_t>(frames) * 2U; ++sample) {
        out[sample] *= channelGain;
    }

    if (renderTrack) {
        double expectedPosition = startPosition;
        const bool positionCommitted =
            impl_->positionFrames.compare_exchange_strong(
                expectedPosition, position, std::memory_order_release,
                std::memory_order_relaxed);
        if (reachedEnd && positionCommitted)
            playing.store(false, std::memory_order_release);
    }
}

double Deck::positionSec() const
{
    return impl_->positionFrames.load(std::memory_order_acquire) /
           static_cast<double>(kSampleRate);
}

void Deck::seekSec(double sec)
{
    if (!std::isfinite(sec))
        return;

    const int64_t frames =
        impl_->trackFrameCount.load(std::memory_order_acquire);
    const double targetFrame = std::clamp(
        sec * static_cast<double>(kSampleRate), 0.0,
        static_cast<double>(std::max<int64_t>(frames, 0)));

    if (loopActive.load(std::memory_order_acquire)) {
        const double start = loopStartSec.load(std::memory_order_acquire);
        const double end = loopEndSec.load(std::memory_order_acquire);
        const double targetSec = targetFrame / static_cast<double>(kSampleRate);
        if (!std::isfinite(start) || !std::isfinite(end) ||
            targetSec < start || targetSec >= end) {
            // External seeks outside an active loop use Serato-style exit:
            // deactivate the loop but preserve its stored bounds.
            loopActive.store(false, std::memory_order_release);
        }
    }
    impl_->positionFrames.store(targetFrame, std::memory_order_release);
}

double Deck::beatPosition() const
{
    const double bpm = impl_->trackBpm.load(std::memory_order_acquire);
    if (!std::isfinite(bpm) || bpm <= 0.0)
        return 0.0;

    const double firstBeat =
        impl_->firstBeatSec.load(std::memory_order_acquire);
    return (positionSec() - firstBeat) * bpm / 60.0;
}

double Deck::effectiveBpm() const
{
    const double bpm = impl_->trackBpm.load(std::memory_order_acquire);
    const double ratio = tempoRatio.load(std::memory_order_relaxed);
    if (!std::isfinite(bpm) || bpm <= 0.0 ||
        !std::isfinite(ratio) || ratio <= 0.0) {
        return 0.0;
    }
    return bpm * ratio;
}

} // namespace gvt
