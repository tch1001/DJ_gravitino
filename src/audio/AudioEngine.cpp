#include "AudioEngine.h"

#include "../../third_party/miniaudio.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace gvt {
namespace {

constexpr int kScratchFrames = 256;

float normalizedValue(double value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

} // namespace

struct AudioEngine::Impl {
    explicit Impl(AudioEngine* engine) noexcept : owner(engine) {}

    AudioEngine* owner = nullptr;
    std::array<Deck, kNumDecks> decks;
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> deckA {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> deckB {};
    ma_device device {};
    bool deviceInitialized = false;
    bool deviceStarted = false;

    static void dataCallback(ma_device* device, void* output,
                             const void* input, ma_uint32 frameCount) noexcept
    {
        (void)input;
        auto* const impl = static_cast<Impl*>(device->pUserData);
        if (impl == nullptr || output == nullptr)
            return;

        impl->renderMix(static_cast<float*>(output),
                        static_cast<int>(frameCount));
    }

    void renderMix(float* output, int frames) noexcept
    {
        if (output == nullptr || frames <= 0)
            return;

        int rendered = 0;
        while (rendered < frames) {
            const int chunkFrames = std::min(kScratchFrames, frames - rendered);
            decks[0].render(deckA.data(), chunkFrames);
            decks[1].render(deckB.data(), chunkFrames);

            float xf = owner->crossfader.load(std::memory_order_relaxed);
            if (!std::isfinite(xf))
                xf = 0.0f;
            xf = std::clamp(xf, 0.0f, 1.0f);

            const float angle = xf * std::numbers::pi_v<float> * 0.5f;
            const float gainA = xf == 1.0f ? 0.0f : std::cos(angle);
            const float gainB = xf == 0.0f ? 0.0f : std::sin(angle);
            const std::size_t outputOffset =
                static_cast<std::size_t>(rendered) * 2U;

            for (std::size_t sample = 0;
                 sample < static_cast<std::size_t>(chunkFrames) * 2U;
                 ++sample) {
                const float mixed =
                    deckA[sample] * gainA + deckB[sample] * gainB;
                output[outputOffset + sample] = std::tanh(mixed);
            }

            rendered += chunkFrames;
        }
    }
};

AudioEngine::AudioEngine(ControlBus* bus, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this))
{
    if (bus != nullptr) {
        QObject::connect(bus, &ControlBus::eventDispatched,
                         this, &AudioEngine::applyEvent,
                         Qt::DirectConnection);
    }
}

AudioEngine::~AudioEngine()
{
    stopDevice();
    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
    }
}

bool AudioEngine::start(QString* error)
{
    if (error != nullptr)
        error->clear();
    if (impl_->deviceStarted)
        return true;

    if (!impl_->deviceInitialized) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = kSampleRate;
        config.periodSizeInFrames = kScratchFrames;
        config.periods = 2;
        config.noPreSilencedOutputBuffer = MA_TRUE;
        config.noClip = MA_TRUE;
        config.dataCallback = &Impl::dataCallback;
        config.pUserData = impl_.get();

        const ma_result initResult =
            ma_device_init(nullptr, &config, &impl_->device);
        if (initResult != MA_SUCCESS) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not initialize audio output: %1")
                    .arg(QString::fromUtf8(ma_result_description(initResult)));
            }
            return false;
        }
        impl_->deviceInitialized = true;
    }

    const ma_result startResult = ma_device_start(&impl_->device);
    if (startResult != MA_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not start audio output: %1")
                .arg(QString::fromUtf8(ma_result_description(startResult)));
        }
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
        return false;
    }

    impl_->deviceStarted = true;
    return true;
}

void AudioEngine::stopDevice()
{
    if (!impl_->deviceStarted)
        return;

    ma_device_stop(&impl_->device);
    impl_->deviceStarted = false;
}

Deck& AudioEngine::deck(int index)
{
    assert(index >= 0 && index < kNumDecks);
    return impl_->decks[static_cast<std::size_t>(index)];
}

void AudioEngine::renderOffline(float* out, int frames)
{
    impl_->renderMix(out, frames);
}

void AudioEngine::applyEvent(const ControlEvent& event, Origin origin)
{
    (void)origin;

    if (controlIsTrigger(event.id) &&
        (!std::isfinite(event.value) || event.value <= 0.0)) {
        return;
    }

    if (event.id == ControlId::Crossfader) {
        if (std::isfinite(event.value))
            crossfader.store(normalizedValue(event.value),
                             std::memory_order_relaxed);
        return;
    }

    if (event.deck < 0 || event.deck >= kNumDecks)
        return;

    Deck& target = deck(event.deck);
    switch (event.id) {
    case ControlId::Play:
        target.play();
        break;
    case ControlId::Stop:
        target.stop();
        break;
    case ControlId::Cue:
        target.cueJump();
        break;
    case ControlId::Load:
        // TransitionPlayer resolves and loads the TrackData before dispatch.
        break;
    case ControlId::TempoSync: {
        Deck& other = deck(1 - event.deck);
        const TrackDataPtr targetTrack = target.track();
        const double otherBpm = other.effectiveBpm();
        if (!targetTrack || !std::isfinite(targetTrack->bpm) ||
            targetTrack->bpm <= 0.0 || !std::isfinite(otherBpm) ||
            otherBpm <= 0.0) {
            break;
        }

        target.tempoRatio.store(otherBpm / targetTrack->bpm,
                                std::memory_order_relaxed);

        const double targetBeat = target.beatPosition();
        const double otherBeat = other.beatPosition();
        if (std::isfinite(targetBeat) && std::isfinite(otherBeat)) {
            const double otherPhase = otherBeat - std::floor(otherBeat);
            const double alignedBeat =
                std::round(targetBeat - otherPhase) + otherPhase;
            target.seekSec(targetTrack->secAtBeat(alignedBeat));
        }
        break;
    }
    case ControlId::HotCue1:
    case ControlId::HotCue2:
    case ControlId::HotCue3:
    case ControlId::HotCue4:
    case ControlId::HotCue5:
    case ControlId::HotCue6:
    case ControlId::HotCue7:
    case ControlId::HotCue8:
        target.jumpHotCue(static_cast<int>(event.id) -
                          static_cast<int>(ControlId::HotCue1));
        break;
    case ControlId::Tempo:
        if (std::isfinite(event.value) && event.value > 0.0) {
            target.tempoRatio.store(std::clamp(event.value, 0.01, 4.0),
                                    std::memory_order_relaxed);
        }
        break;
    case ControlId::Fader:
        if (std::isfinite(event.value))
            target.fader.store(normalizedValue(event.value),
                               std::memory_order_relaxed);
        break;
    case ControlId::Trim:
        if (std::isfinite(event.value))
            target.trim.store(normalizedValue(event.value),
                              std::memory_order_relaxed);
        break;
    case ControlId::EqLow:
        if (std::isfinite(event.value))
            target.eqLow.store(normalizedValue(event.value),
                               std::memory_order_relaxed);
        break;
    case ControlId::EqMid:
        if (std::isfinite(event.value))
            target.eqMid.store(normalizedValue(event.value),
                               std::memory_order_relaxed);
        break;
    case ControlId::EqHigh:
        if (std::isfinite(event.value))
            target.eqHigh.store(normalizedValue(event.value),
                                std::memory_order_relaxed);
        break;
    case ControlId::Jog:
        target.nudge(event.value);
        break;
    case ControlId::Crossfader:
    case ControlId::Count:
        break;
    }
}

} // namespace gvt
