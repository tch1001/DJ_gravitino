#include "AudioEngine.h"

#include "Eq.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace gvt {
namespace {

constexpr double kJogRatioPerTick = 0.004;
constexpr double kMaximumJogRatio = 0.25;
constexpr double kCuePreviewToleranceSec = 0.05;
// Reaches 0.1% of the initial bend after approximately 200 ms at 48 kHz.
constexpr double kJogDecayPerFrame = 0.999280701;
constexpr double kMinimumLoopBeats = 0.125;
constexpr double kMaximumLoopBeats = 64.0;
constexpr double kManualLoopSubdivisions = 8.0;

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

double snapToBeatSubdivision(const TrackData& track, double sec) noexcept
{
    const double beat = track.beatAtSec(sec);
    const double snappedBeat =
        std::round(beat * kManualLoopSubdivisions) /
        kManualLoopSubdivisions;
    return clampTrackSec(track, track.secAtBeat(snappedBeat));
}

} // namespace

struct Deck::Impl {
    TrackDataPtr ownedTrack;
    std::atomic<TrackData*> audioTrack { nullptr };
    std::atomic<double> positionFrames { 0.0 };
    std::atomic<double> trackBpm { 0.0 };
    std::atomic<double> firstBeatSec { 0.0 };
    std::atomic<int64_t> trackFrameCount { 0 };
    std::atomic<unsigned int> activeRenders { 0 };
    std::atomic<double> pendingJogRatio { 0.0 };
    std::atomic<bool> cuePreviewing { false };
    double jogRatio = 0.0; // Audio-thread-owned after a safe track swap.
    double pendingLoopInSec = -1.0; // GUI-thread-owned.
    bool hasPendingLoopIn = false;
    Eq eq;
    DjFilter djFilter;

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
    std::atomic_thread_fence(std::memory_order_seq_cst); // see loadTrack()
    impl_->waitForRender();
}

void Deck::loadTrack(TrackDataPtr track)
{
    // Once playing is false, a new render cannot enter the protected section.
    // Waiting drains a callback that observed the previous playing state.
    playing.store(false, std::memory_order_release);
    // StoreLoad barrier: without it this thread can read activeRenders==0 from
    // its store buffer while the audio thread still sees playing==true
    // (Dekker's pattern) — and we'd free PCM mid-render.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    impl_->waitForRender();

    impl_->audioTrack.store(nullptr, std::memory_order_release);
    impl_->ownedTrack = std::move(track);
    impl_->positionFrames.store(0.0, std::memory_order_release);
    impl_->pendingJogRatio.store(0.0, std::memory_order_release);
    impl_->cuePreviewing.store(false, std::memory_order_release);
    impl_->jogRatio = 0.0;
    impl_->pendingLoopInSec = -1.0;
    impl_->hasPendingLoopIn = false;
    impl_->eq.reset();
    impl_->djFilter.reset();
    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(-1.0, std::memory_order_release);
    loopEndSec.store(-1.0, std::memory_order_release);

    TrackData* const rawTrack = impl_->ownedTrack.get();
    if (rawTrack == nullptr) {
        cuePointSec.store(-1.0, std::memory_order_release);
        impl_->trackBpm.store(0.0, std::memory_order_release);
        impl_->firstBeatSec.store(0.0, std::memory_order_release);
        impl_->trackFrameCount.store(0, std::memory_order_release);
        return;
    }

    cuePointSec.store(rawTrack->firstBeatSec, std::memory_order_release);
    impl_->trackBpm.store(rawTrack->bpm, std::memory_order_release);
    impl_->firstBeatSec.store(rawTrack->firstBeatSec, std::memory_order_release);
    impl_->trackFrameCount.store(rawTrack->frameCount(), std::memory_order_release);
    impl_->audioTrack.store(rawTrack, std::memory_order_release);
}

TrackDataPtr Deck::track() const
{
    return impl_->ownedTrack;
}

void Deck::play()
{
    if (impl_->audioTrack.load(std::memory_order_acquire) == nullptr)
        return;
    // At end-of-track the render callback would immediately clear `playing`
    // again — rewind so play-after-EOF restarts instead of doing nothing.
    const auto frames = impl_->trackFrameCount.load(std::memory_order_acquire);
    if (impl_->positionFrames.load(std::memory_order_acquire) >=
        (double)frames - 1.0)
        impl_->positionFrames.store(0.0, std::memory_order_release);
    playing.store(true, std::memory_order_release);
}

void Deck::stop()
{
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
        play();
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

void Deck::setHotCue(int index)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (currentTrack)
        currentTrack->hotCues[index] = positionSec();
}

void Deck::jumpHotCue(int index)
{
    if (index < 0 || index >= 8)
        return;

    const TrackDataPtr currentTrack = track();
    if (currentTrack && currentTrack->hotCues[index] >= 0.0)
        seekSec(currentTrack->hotCues[index]);
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
    if (!hasUsableBeatGrid(currentTrack))
        return;

    const double start = snapToBeatSubdivision(*currentTrack, positionSec());
    loopActive.store(false, std::memory_order_release);
    loopStartSec.store(start, std::memory_order_release);
    loopEndSec.store(-1.0, std::memory_order_release);
    impl_->pendingLoopInSec = start;
    impl_->hasPendingLoopIn = true;
}

void Deck::loopOut()
{
    const TrackDataPtr currentTrack = track();
    if (!hasUsableBeatGrid(currentTrack) || !impl_->hasPendingLoopIn)
        return;

    const double current = positionSec();
    const double start = impl_->pendingLoopInSec;
    if (!std::isfinite(current) || !(current > start))
        return;

    double end = snapToBeatSubdivision(*currentTrack, current);
    const double minimumEnd =
        start + kMinimumLoopBeats * 60.0 / currentTrack->bpm;
    end = clampTrackSec(*currentTrack, std::max(end, minimumEnd));
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

void Deck::render(float* out, int frames)
{
    if (out == nullptr || frames <= 0)
        return;

    std::fill_n(out, static_cast<std::size_t>(frames) * 2U, 0.0f);

    // The second playing check closes the race with loadTrack(): a render that
    // saw the old true value either publishes itself here or exits before ever
    // reading the raw track pointer.
    if (!playing.load(std::memory_order_acquire))
        return;

    impl_->activeRenders.fetch_add(1, std::memory_order_acq_rel);
    // StoreLoad barrier pairing with loadTrack(): the re-check below must not
    // read a stale playing==true after publishing activeRenders.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!playing.load(std::memory_order_acquire)) {
        impl_->activeRenders.fetch_sub(1, std::memory_order_release);
        return;
    }

    TrackData* const currentTrack =
        impl_->audioTrack.load(std::memory_order_acquire);
    if (currentTrack == nullptr || currentTrack->frameCount() <= 0) {
        playing.store(false, std::memory_order_release);
        impl_->activeRenders.fetch_sub(1, std::memory_order_release);
        return;
    }

    double ratio = tempoRatio.load(std::memory_order_relaxed);
    if (!std::isfinite(ratio) || ratio <= 0.0)
        ratio = 1.0;
    ratio = std::clamp(ratio, 0.01, 4.0);

    impl_->jogRatio = std::clamp(
        impl_->jogRatio +
            impl_->pendingJogRatio.exchange(0.0, std::memory_order_acq_rel),
        -kMaximumJogRatio, kMaximumJogRatio);

    const double startPosition =
        impl_->positionFrames.load(std::memory_order_acquire);
    double position = std::isfinite(startPosition)
        ? std::max(0.0, startPosition)
        : 0.0;
    const int64_t trackFrames = currentTrack->frameCount();
    const double inputGain = trimGain(trim.load(std::memory_order_relaxed));
    bool reachedEnd = false;

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
        if (!std::isfinite(loopStartFrame) || !std::isfinite(loopEndFrame) ||
            !(loopLengthFrames > 0.0)) {
            loopEnabled = false;
        }
    }

    const auto wrapLoopPosition = [&] {
        if (loopEnabled && position >= loopEndFrame) {
            const double overshoot =
                std::fmod(position - loopStartFrame, loopLengthFrames);
            position = loopStartFrame +
                (overshoot >= 0.0 ? overshoot : overshoot + loopLengthFrames);
        }
    };

    for (int frame = 0; frame < frames; ++frame) {
        wrapLoopPosition();
        if (position >= static_cast<double>(trackFrames)) {
            position = static_cast<double>(trackFrames);
            reachedEnd = true;
            break;
        }

        const int64_t frame0 = static_cast<int64_t>(position);
        const int64_t frame1 = std::min(frame0 + 1, trackFrames - 1);
        const double fraction = position - static_cast<double>(frame0);
        const std::size_t sample0 = static_cast<std::size_t>(frame0) * 2U;
        const std::size_t sample1 = static_cast<std::size_t>(frame1) * 2U;
        const int outputBase = frame * 2;

        for (int channel = 0; channel < 2; ++channel) {
            const double first = currentTrack->pcm[
                sample0 + static_cast<std::size_t>(channel)];
            const double second = currentTrack->pcm[
                sample1 + static_cast<std::size_t>(channel)];
            out[outputBase + channel] = static_cast<float>(
                (first + (second - first) * fraction) * inputGain);
        }

        const double playbackRatio =
            std::clamp(ratio + impl_->jogRatio, 0.01, 4.0);
        position += playbackRatio;
        wrapLoopPosition();
        impl_->jogRatio *= kJogDecayPerFrame;
        if (std::abs(impl_->jogRatio) < 1.0e-8)
            impl_->jogRatio = 0.0;
    }

    if (position >= static_cast<double>(trackFrames)) {
        position = static_cast<double>(trackFrames);
        reachedEnd = true;
    }

    impl_->eq.process(out, frames,
                      eqLow.load(std::memory_order_relaxed),
                      eqMid.load(std::memory_order_relaxed),
                      eqHigh.load(std::memory_order_relaxed));
    impl_->djFilter.process(
        out, frames, filter.load(std::memory_order_relaxed));

    const float channelGain =
        clampUnit(fader.load(std::memory_order_relaxed), 1.0f);
    for (std::size_t sample = 0;
         sample < static_cast<std::size_t>(frames) * 2U; ++sample) {
        out[sample] *= channelGain;
    }

    double expectedPosition = startPosition;
    const bool positionCommitted = impl_->positionFrames.compare_exchange_strong(
        expectedPosition, position, std::memory_order_release,
        std::memory_order_relaxed);
    if (reachedEnd && positionCommitted)
        playing.store(false, std::memory_order_release);

    impl_->activeRenders.fetch_sub(1, std::memory_order_release);
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
