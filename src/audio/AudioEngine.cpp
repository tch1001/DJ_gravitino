#include "AudioEngine.h"
#include "TempoRange.h"

#include "../../third_party/miniaudio.h"
#include "../audio/MasterRecorder.h"

#include <QDebug>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace gvt {
namespace {

constexpr int kScratchFrames = 256;
constexpr int kMasterChannels = 2;
constexpr int kFlx4Channels = 4;
constexpr std::size_t kCueRingFrames = 8192;

float normalizedValue(double value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

bool isReleaseAwareTrigger(ControlId id) noexcept
{
    return id == ControlId::Cue ||
        id == ControlId::PlatterTouch ||
        (id >= ControlId::HotCue1 && id <= ControlId::HotCue8) ||
        (id >= ControlId::SavedLoop1 && id <= ControlId::SavedLoop8);
}

} // namespace

struct AudioEngine::Impl {
    explicit Impl(AudioEngine* engine) noexcept : owner(engine) {}

    AudioEngine* owner = nullptr;
    std::array<Deck, kNumDecks> decks;
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> deckA {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> deckB {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> cueA {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> cueB {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> master {};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2U> phones {};
    std::array<float, kCueRingFrames * 2U> cueRing {};
    std::atomic<std::uint64_t> cueWriteFrame {0};
    std::atomic<std::uint64_t> cueReadFrame {0};
    std::atomic<int> headphoneTestFrames {0};
    std::atomic<float> headphoneSignalPeak {0.0f};
    double headphoneTestPhase = 0.0;
    ma_context context {};
    ma_device device {};
    ma_device cueDevice {};
    bool contextInitialized = false;
    bool deviceInitialized = false;
    bool deviceStarted = false;
    bool cueDeviceInitialized = false;
    bool cueDeviceStarted = false;
    bool fourChannelOutput = false;
    QString outputName;
    QString requestedOutputName;

    bool ensureContext(QString* error)
    {
        if (contextInitialized)
            return true;
        const ma_result result = ma_context_init(nullptr, 0, nullptr, &context);
        if (result == MA_SUCCESS) {
            contextInitialized = true;
            return true;
        }
        if (error != nullptr) {
            *error = QStringLiteral("Could not initialize CoreAudio: %1")
                .arg(QString::fromUtf8(ma_result_description(result)));
        }
        return false;
    }

    static void dataCallback(ma_device* device, void* output,
                             const void* input, ma_uint32 frameCount) noexcept
    {
        (void)input;
        auto* const impl = static_cast<Impl*>(device->pUserData);
        if (impl == nullptr || output == nullptr)
            return;

        impl->renderMix(static_cast<float*>(output),
                        static_cast<int>(frameCount),
                        static_cast<int>(device->playback.channels),
                        device->playback.channels < kFlx4Channels);
    }

    static void cueDataCallback(ma_device* device, void* output,
                                const void* input,
                                ma_uint32 frameCount) noexcept
    {
        (void)input;
        auto* const impl = static_cast<Impl*>(device->pUserData);
        if (impl == nullptr || output == nullptr)
            return;
        impl->readCueRing(static_cast<float*>(output),
                          static_cast<int>(frameCount),
                          static_cast<int>(device->playback.channels));
    }

    void pushCueRing(const float* input, int frames) noexcept
    {
        const std::uint64_t write = cueWriteFrame.load(std::memory_order_relaxed);
        const std::uint64_t read = cueReadFrame.load(std::memory_order_acquire);
        if (write - read + static_cast<std::uint64_t>(frames) > kCueRingFrames)
            return; // consumer catches up and the next callback resumes
        for (int frame = 0; frame < frames; ++frame) {
            const std::size_t ringFrame = static_cast<std::size_t>(
                (write + static_cast<std::uint64_t>(frame)) % kCueRingFrames);
            cueRing[ringFrame * 2U] = input[static_cast<std::size_t>(frame) * 2U];
            cueRing[ringFrame * 2U + 1U] =
                input[static_cast<std::size_t>(frame) * 2U + 1U];
        }
        cueWriteFrame.store(write + static_cast<std::uint64_t>(frames),
                            std::memory_order_release);
    }

    void readCueRing(float* output, int frames, int channels) noexcept
    {
        if (output == nullptr || frames <= 0 || channels <= 0)
            return;
        std::fill(output,
                  output + static_cast<std::size_t>(frames) *
                               static_cast<std::size_t>(channels),
                  0.0f);
        std::uint64_t read = cueReadFrame.load(std::memory_order_relaxed);
        const std::uint64_t write = cueWriteFrame.load(std::memory_order_acquire);
        std::uint64_t available = write - read;
        // Independent CoreAudio devices can drift slightly. Keep cue latency
        // bounded by dropping stale monitor frames rather than building delay.
        constexpr std::uint64_t kMaximumBacklog = kScratchFrames * 4U;
        if (available > kMaximumBacklog) {
            read = write - kScratchFrames * 2U;
            available = write - read;
        }
        const int readable = static_cast<int>(std::min<std::uint64_t>(
            available, static_cast<std::uint64_t>(frames)));
        float outputPeak = 0.0f;
        for (int frame = 0; frame < readable; ++frame) {
            const std::size_t ringFrame = static_cast<std::size_t>(
                (read + static_cast<std::uint64_t>(frame)) % kCueRingFrames);
            const std::size_t destination =
                static_cast<std::size_t>(frame) *
                static_cast<std::size_t>(channels);
            if (channels >= kFlx4Channels) {
                output[destination + 2U] = cueRing[ringFrame * 2U];
                output[destination + 3U] = cueRing[ringFrame * 2U + 1U];
                outputPeak = std::max(
                    {outputPeak,
                     std::fabs(output[destination + 2U]),
                     std::fabs(output[destination + 3U])});
            } else if (channels >= 2) {
                output[destination] = cueRing[ringFrame * 2U];
                output[destination + 1U] = cueRing[ringFrame * 2U + 1U];
                outputPeak = std::max(
                    {outputPeak,
                     std::fabs(output[destination]),
                     std::fabs(output[destination + 1U])});
            }
        }
        const float previousPeak = headphoneSignalPeak.load(
            std::memory_order_relaxed);
        headphoneSignalPeak.store(
            std::clamp(std::max(outputPeak, previousPeak * 0.90f),
                       0.0f, 1.0f),
            std::memory_order_relaxed);
        cueReadFrame.store(read + static_cast<std::uint64_t>(readable),
                           std::memory_order_release);
    }

    static bool deviceIsActive(const ma_device& candidate) noexcept
    {
        const ma_device_state state = ma_device_get_state(&candidate);
        return state == ma_device_state_starting ||
               state == ma_device_state_started;
    }

    bool cueOutputActive() const noexcept
    {
        return cueDeviceStarted && cueDeviceInitialized &&
               deviceIsActive(cueDevice);
    }

    void renderMix(float* output, int frames, int outputChannels,
                   bool feedCueRing = false) noexcept
    {
        if (output == nullptr || frames <= 0)
            return;
        outputChannels = outputChannels >= kFlx4Channels
                             ? kFlx4Channels : kMasterChannels;

        int rendered = 0;
        while (rendered < frames) {
            const int chunkFrames = std::min(kScratchFrames, frames - rendered);
            decks[0].render(deckA.data(), chunkFrames, cueA.data());
            decks[1].render(deckB.data(), chunkFrames, cueB.data());

            float xf = owner->crossfader.load(std::memory_order_relaxed);
            if (!std::isfinite(xf))
                xf = 0.0f;
            xf = std::clamp(xf, 0.0f, 1.0f);

            const float angle = xf * std::numbers::pi_v<float> * 0.5f;
            const float gainA = xf == 1.0f ? 0.0f : std::cos(angle);
            const float gainB = xf == 0.0f ? 0.0f : std::sin(angle);
            const bool monitorA =
                owner->headphoneCue[0].load(std::memory_order_relaxed);
            const bool monitorB =
                owner->headphoneCue[1].load(std::memory_order_relaxed);
            const bool monitorMaster =
                owner->masterCue.load(std::memory_order_relaxed);
            float monitorMix =
                owner->headphoneMix.load(std::memory_order_relaxed);
            if (!std::isfinite(monitorMix)) monitorMix = 0.0f;
            monitorMix = std::clamp(monitorMix, 0.0f, 1.0f);
            const float monitorAngle =
                monitorMix * std::numbers::pi_v<float> * 0.5f;
            const float cueGain = monitorMix == 1.0f
                                      ? 0.0f : std::cos(monitorAngle);
            const float masterGain = !monitorMaster || monitorMix == 0.0f
                                         ? 0.0f : std::sin(monitorAngle);
            const float cueBusGain = monitorA && monitorB ? 0.5f : 1.0f;
            float chunkPhonePeak = 0.0f;

            for (int frame = 0; frame < chunkFrames; ++frame) {
                const std::size_t stereo = static_cast<std::size_t>(frame) * 2U;
                const std::size_t destination =
                    static_cast<std::size_t>(rendered + frame) *
                    static_cast<std::size_t>(outputChannels);
                const int testFrames = headphoneTestFrames.load(
                    std::memory_order_relaxed);
                float headphoneTestSample = 0.0f;
                if (testFrames > 0) {
                    headphoneTestSample = static_cast<float>(
                        std::sin(headphoneTestPhase) * 0.12);
                    headphoneTestPhase +=
                        2.0 * std::numbers::pi_v<double> * 440.0 /
                        static_cast<double>(kSampleRate);
                    if (headphoneTestPhase >=
                        2.0 * std::numbers::pi_v<double>)
                        headphoneTestPhase -=
                            2.0 * std::numbers::pi_v<double>;
                    headphoneTestFrames.fetch_sub(1,
                        std::memory_order_relaxed);
                }
                for (int channel = 0; channel < 2; ++channel) {
                    const std::size_t sample =
                        stereo + static_cast<std::size_t>(channel);
                    const float mixed =
                        deckA[sample] * gainA + deckB[sample] * gainB;
                    const float masterSample = std::tanh(mixed);
                    master[sample] = masterSample;
                    output[destination + static_cast<std::size_t>(channel)] =
                        masterSample;

                    float cueSample = 0.0f;
                    if (monitorA) cueSample += cueA[sample];
                    if (monitorB) cueSample += cueB[sample];
                    cueSample *= cueBusGain;
                    const float phoneSample = std::clamp(
                        std::tanh(cueSample * cueGain) +
                            masterSample * masterGain + headphoneTestSample,
                        -1.0f, 1.0f);
                    phones[sample] = phoneSample;
                    chunkPhonePeak = std::max(
                        chunkPhonePeak, std::fabs(phoneSample));
                    if (outputChannels >= kFlx4Channels)
                        output[destination + 2U +
                               static_cast<std::size_t>(channel)] = phoneSample;
                }
            }

            // With a four-channel primary FLX4 stream, this buffer is already
            // the hardware callback output. With split MacBook/Bluetooth +
            // FLX4 routing, readCueRing() records the level only after the
            // secondary device callback has copied it into outputs 3/4.
            if (outputChannels >= kFlx4Channels) {
                const float previousPhonePeak = headphoneSignalPeak.load(
                    std::memory_order_relaxed);
                headphoneSignalPeak.store(
                    std::clamp(std::max(chunkPhonePeak,
                                        previousPhonePeak * 0.90f),
                               0.0f, 1.0f),
                    std::memory_order_relaxed);
            }

            if (feedCueRing)
                pushCueRing(phones.data(), chunkFrames);

            MasterRecorder* const tap =
                owner->masterTap.load(std::memory_order_acquire);
            if (tap != nullptr)
                tap->feed(master.data(), chunkFrames);

            rendered += chunkFrames;
        }
    }

    bool startFlx4CueDevice()
    {
        if (fourChannelOutput || !contextInitialized)
            return fourChannelOutput;
        if (cueOutputActive())
            return true;
        // CoreAudio stops a device when its USB endpoint disappears. Clear
        // the stale miniaudio object so a later hot-plug retry can reopen it.
        if (cueDeviceInitialized) {
            if (cueDeviceStarted)
                ma_device_stop(&cueDevice);
            ma_device_uninit(&cueDevice);
            cueDeviceInitialized = false;
            cueDeviceStarted = false;
        }

        ma_device_info* playback = nullptr;
        ma_uint32 playbackCount = 0;
        if (ma_context_get_devices(&context, &playback, &playbackCount,
                                   nullptr, nullptr) != MA_SUCCESS)
            return false;
        const ma_device_info* flx4 = nullptr;
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            const QString name = QString::fromUtf8(playback[i].name);
            if (name.contains(QStringLiteral("DDJ-FLX4"),
                              Qt::CaseInsensitive)) {
                flx4 = &playback[i];
                break;
            }
        }
        if (flx4 == nullptr)
            return false;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = &flx4->id;
        config.playback.format = ma_format_f32;
        config.playback.channels = kFlx4Channels;
        config.sampleRate = kSampleRate;
        config.periodSizeInFrames = kScratchFrames;
        config.periods = 2;
        config.noPreSilencedOutputBuffer = MA_TRUE;
        config.noClip = MA_TRUE;
        config.dataCallback = &Impl::cueDataCallback;
        config.pUserData = this;
        const ma_result initResult =
            ma_device_init(&context, &config, &cueDevice);
        if (initResult != MA_SUCCESS) {
            qWarning().noquote()
                << "Could not initialize DDJ-FLX4 headphone output:"
                << ma_result_description(initResult);
            return false;
        }
        cueDeviceInitialized = true;
        const ma_result startResult = ma_device_start(&cueDevice);
        if (startResult != MA_SUCCESS) {
            qWarning().noquote()
                << "Could not start DDJ-FLX4 headphone output:"
                << ma_result_description(startResult);
            ma_device_uninit(&cueDevice);
            cueDeviceInitialized = false;
            return false;
        }
        cueDeviceStarted = true;
        char clientMap[256] {};
        char deviceMap[256] {};
        ma_channel_map_to_string(cueDevice.playback.channelMap,
                                 cueDevice.playback.channels,
                                 clientMap, sizeof(clientMap));
        ma_channel_map_to_string(cueDevice.playback.internalChannelMap,
                                 cueDevice.playback.internalChannels,
                                 deviceMap, sizeof(deviceMap));
        qInfo().noquote()
            << "DDJ-FLX4 headphone output active:"
            << cueDevice.playback.name << cueDevice.playback.channels
            << "channels at" << cueDevice.sampleRate << "Hz; client map"
            << clientMap << "; device map" << deviceMap;
        return true;
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
    if (impl_->cueDeviceInitialized) {
        ma_device_uninit(&impl_->cueDevice);
        impl_->cueDeviceInitialized = false;
    }
    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
    }
    if (impl_->contextInitialized) {
        ma_context_uninit(&impl_->context);
        impl_->contextInitialized = false;
    }
}

bool AudioEngine::start(QString* error)
{
    return start(QString(), error);
}

bool AudioEngine::start(const QString& preferredOutputName, QString* error)
{
    if (error != nullptr)
        error->clear();
    if (impl_->deviceStarted) {
        if (impl_->requestedOutputName == preferredOutputName)
            return true;
        return switchOutputDevice(preferredOutputName, error);
    }

    if (!impl_->ensureContext(error))
        return false;

    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kMasterChannels;
    config.sampleRate = kSampleRate;
    config.periodSizeInFrames = kScratchFrames;
    config.periods = 2;
    config.noPreSilencedOutputBuffer = MA_TRUE;
    config.noClip = MA_TRUE;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_.get();

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&impl_->context, &playback, &playbackCount,
                               nullptr, nullptr) != MA_SUCCESS) {
        if (error != nullptr)
            *error = QStringLiteral("Could not list CoreAudio output devices");
        return false;
    }

    const ma_device_info* selected = nullptr;
    if (!preferredOutputName.isEmpty()) {
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            if (QString::fromUtf8(playback[i].name) == preferredOutputName) {
                selected = &playback[i];
                break;
            }
        }
        if (selected == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Audio output “%1” is not available")
                    .arg(preferredOutputName);
            }
            return false;
        }
    }

    if (preferredOutputName.isEmpty()) {
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            if (playback[i].isDefault == MA_TRUE &&
                QString::fromUtf8(playback[i].name).contains(
                    QStringLiteral("DDJ-FLX4"), Qt::CaseInsensitive)) {
                selected = &playback[i];
                break;
            }
        }
    }

    const bool selectedFlx4 = selected != nullptr &&
        QString::fromUtf8(selected->name).contains(
            QStringLiteral("DDJ-FLX4"), Qt::CaseInsensitive);
    if (selected != nullptr)
        config.playback.pDeviceID = &selected->id;
    if (selectedFlx4)
        config.playback.channels = kFlx4Channels;

    ma_result initResult =
        ma_device_init(&impl_->context, &config, &impl_->device);
    if (initResult != MA_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not initialize audio output: %1")
                .arg(QString::fromUtf8(ma_result_description(initResult)));
        }
        return false;
    }
    impl_->deviceInitialized = true;
    impl_->fourChannelOutput = selectedFlx4 &&
        impl_->device.playback.channels >= kFlx4Channels;
    impl_->outputName = QString::fromUtf8(impl_->device.playback.name);
    impl_->requestedOutputName = preferredOutputName;
    impl_->cueReadFrame.store(0, std::memory_order_relaxed);
    impl_->cueWriteFrame.store(0, std::memory_order_relaxed);

    const ma_result startResult = ma_device_start(&impl_->device);
    if (startResult != MA_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not start audio output: %1")
                .arg(QString::fromUtf8(ma_result_description(startResult)));
        }
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
        impl_->fourChannelOutput = false;
        impl_->outputName.clear();
        return false;
    }

    impl_->deviceStarted = true;
    if (!impl_->fourChannelOutput)
        impl_->startFlx4CueDevice();
    emit outputDeviceChanged(
        impl_->outputName,
        impl_->fourChannelOutput || impl_->cueDeviceStarted);
    return true;
}

bool AudioEngine::switchOutputDevice(const QString& preferredOutputName,
                                     QString* error)
{
    if (error != nullptr)
        error->clear();
    if (impl_->deviceStarted &&
        impl_->requestedOutputName == preferredOutputName) {
        const bool hadPhones = headphoneOutputAvailable();
        if (!impl_->fourChannelOutput)
            impl_->startFlx4CueDevice();
        if (!hadPhones && headphoneOutputAvailable())
            emit outputDeviceChanged(impl_->outputName, true);
        return true;
    }

    const QString previousPreference = impl_->requestedOutputName;
    stopDevice();
    if (impl_->cueDeviceInitialized) {
        ma_device_uninit(&impl_->cueDevice);
        impl_->cueDeviceInitialized = false;
    }
    if (impl_->deviceInitialized) {
        ma_device_uninit(&impl_->device);
        impl_->deviceInitialized = false;
    }
    impl_->fourChannelOutput = false;
    impl_->outputName.clear();

    QString requestedError;
    if (start(preferredOutputName, &requestedError))
        return true;

    // A failed switch should not strand a playing set without output. Restore
    // the previous preference, then automatic output as a final fallback.
    QString restoreError;
    if (!start(previousPreference, &restoreError) &&
        !previousPreference.isEmpty())
        start(QString(), &restoreError);
    if (error != nullptr)
        *error = requestedError;
    return false;
}

void AudioEngine::stopDevice()
{
    if (impl_->cueDeviceStarted) {
        ma_device_stop(&impl_->cueDevice);
        impl_->cueDeviceStarted = false;
    }

    if (impl_->deviceStarted) {
        ma_device_stop(&impl_->device);
        impl_->deviceStarted = false;
    }
}

Deck& AudioEngine::deck(int index)
{
    assert(index >= 0 && index < kNumDecks);
    return impl_->decks[static_cast<std::size_t>(index)];
}

void AudioEngine::renderOffline(float* out, int frames)
{
    impl_->renderMix(out, frames, kMasterChannels);
}

void AudioEngine::renderOfflineFourChannel(float* out, int frames)
{
    impl_->renderMix(out, frames, kFlx4Channels);
}

bool AudioEngine::headphoneOutputAvailable() const
{
    return (impl_->fourChannelOutput && impl_->deviceStarted &&
            Impl::deviceIsActive(impl_->device)) ||
           impl_->cueOutputActive();
}

float AudioEngine::headphoneSignalLevel() const
{
    return impl_->headphoneSignalPeak.load(std::memory_order_relaxed);
}

void AudioEngine::startHeadphoneTest(int milliseconds)
{
    if (!headphoneOutputAvailable())
        return;
    const int boundedMs = std::clamp(milliseconds, 250, 10000);
    impl_->headphoneTestFrames.store(
        boundedMs * kSampleRate / 1000, std::memory_order_release);
    qInfo().noquote() << "FLX4 headphone test tone started for"
                      << boundedMs << "ms";
}

QString AudioEngine::outputDeviceName() const
{
    return impl_->outputName;
}

QString AudioEngine::outputDevicePreference() const
{
    return impl_->requestedOutputName;
}

QList<AudioOutputDevice> AudioEngine::availableOutputDevices(QString* error)
{
    if (error != nullptr)
        error->clear();
    QList<AudioOutputDevice> result;
    if (!impl_->ensureContext(error))
        return result;

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    const ma_result query = ma_context_get_devices(
        &impl_->context, &playback, &playbackCount, nullptr, nullptr);
    if (query != MA_SUCCESS) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not list CoreAudio outputs: %1")
                .arg(QString::fromUtf8(ma_result_description(query)));
        }
        return result;
    }
    result.reserve(static_cast<qsizetype>(playbackCount));
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        result.append(AudioOutputDevice {
            QString::fromUtf8(playback[i].name), playback[i].isDefault == MA_TRUE});
    }
    return result;
}

void AudioEngine::applyEvent(const ControlEvent& event, Origin origin)
{
    (void)origin;

    if (controlIsTrigger(event.id)) {
        if (!std::isfinite(event.value))
            return;
        if (!isReleaseAwareTrigger(event.id) && event.value <= 0.0)
            return;
    }

    if (event.id == ControlId::Crossfader) {
        if (std::isfinite(event.value))
            crossfader.store(normalizedValue(event.value),
                             std::memory_order_relaxed);
        return;
    }

    if (event.id == ControlId::MasterCue) {
        masterCue.store(event.value > 0.5, std::memory_order_relaxed);
        return;
    }

    if (event.id == ControlId::HeadphoneMix) {
        if (std::isfinite(event.value))
            headphoneMix.store(normalizedValue(event.value),
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
        target.handleCue(event.value >= 0.5);
        break;
    case ControlId::Load:
        // TransitionPlayer resolves and loads the TrackData before dispatch.
        break;
    case ControlId::TempoSync: {
        Deck& other = deck(1 - event.deck);
        const TrackDataPtr targetTrack = target.track();
        const TrackDataPtr otherTrack = other.track();
        if (!targetTrack || !otherTrack ||
            !std::isfinite(targetTrack->bpm) || targetTrack->bpm <= 0.0 ||
            !std::isfinite(otherTrack->bpm) || otherTrack->bpm <= 0.0) {
            break;
        }

        // One-shot phase alignment only. Tempo remains under the user's
        // control, so tracks at different effective BPM will drift afterward.
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
    case ControlId::HeadphoneCue:
        headphoneCue[event.deck].store(event.value > 0.5,
                                       std::memory_order_relaxed);
        break;
    case ControlId::HotCue1:
    case ControlId::HotCue2:
    case ControlId::HotCue3:
    case ControlId::HotCue4:
    case ControlId::HotCue5:
    case ControlId::HotCue6:
    case ControlId::HotCue7:
    case ControlId::HotCue8: {
        const int index = static_cast<int>(event.id) -
                          static_cast<int>(ControlId::HotCue1);
        target.handleHotCue(index, event.value >= 0.5);
        break;
    }
    case ControlId::SavedLoop1:
    case ControlId::SavedLoop2:
    case ControlId::SavedLoop3:
    case ControlId::SavedLoop4:
    case ControlId::SavedLoop5:
    case ControlId::SavedLoop6:
    case ControlId::SavedLoop7:
    case ControlId::SavedLoop8: {
        const int index = static_cast<int>(event.id) -
                          static_cast<int>(ControlId::SavedLoop1);
        target.handleSavedLoop(index, event.value >= 0.5);
        break;
    }
    case ControlId::LoopIn:
        if (event.value >= 0.5)
            target.loopIn();
        break;
    case ControlId::LoopOut:
        if (event.value >= 0.5)
            target.loopOut();
        break;
    case ControlId::LoopExit:
        if (event.value >= 0.5)
            target.loopExit();
        break;
    case ControlId::LoopHalve:
        if (event.value >= 0.5)
            target.loopHalve();
        break;
    case ControlId::LoopDouble:
        if (event.value >= 0.5)
            target.loopDouble();
        break;
    case ControlId::Tempo:
        if (std::isfinite(event.value) && event.value > 0.0) {
            target.tempoRatio.store(std::clamp(event.value, 0.01, 4.0),
                                    std::memory_order_relaxed);
        }
        break;
    case ControlId::TempoRange: {
        const double current = target.tempoRange.load(
            std::memory_order_relaxed);
        const double selected = event.value > 0.0
            ? closestSeratoTempoRange(event.value)
            : nextSeratoTempoRange(current);
        // Range selection remaps the fader only; it must never alter an
        // already-playing deck's current effective BPM.
        target.tempoRange.store(selected, std::memory_order_relaxed);
        break;
    }
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
    case ControlId::LoopAuto:
        target.loopAuto(event.value);
        break;
    case ControlId::BeatJump:
        target.beatJump(event.value);
        break;
    case ControlId::Filter:
        if (std::isfinite(event.value))
            target.filter.store(normalizedValue(event.value),
                                std::memory_order_relaxed);
        break;
    case ControlId::StemVocals:
        target.stemVocals.store(normalizedValue(event.value),
                                std::memory_order_relaxed);
        break;
    case ControlId::StemMelody:
        target.stemMelody.store(normalizedValue(event.value),
                                std::memory_order_relaxed);
        break;
    case ControlId::StemBass:
        target.stemBass.store(normalizedValue(event.value),
                              std::memory_order_relaxed);
        break;
    case ControlId::StemDrums:
        target.stemDrums.store(normalizedValue(event.value),
                               std::memory_order_relaxed);
        break;
    case ControlId::FxType:
        if (std::isfinite(event.value)) {
            const double bounded = std::clamp(event.value, 0.0, 2.0);
            target.fxType.store(
                static_cast<int>(std::lround(bounded)),
                std::memory_order_relaxed);
        }
        break;
    case ControlId::FxOn:
        target.fxOn.store(event.value > 0.5, std::memory_order_relaxed);
        break;
    case ControlId::FxWet:
        if (std::isfinite(event.value))
            target.fxWet.store(normalizedValue(event.value),
                               std::memory_order_relaxed);
        break;
    case ControlId::FxBeats:
        if (std::isfinite(event.value)) {
            target.fxBeats.store(std::clamp(event.value, 0.25, 4.0),
                                 std::memory_order_relaxed);
        }
        break;
    case ControlId::Jog:
        target.nudge(event.value);
        break;
    case ControlId::PlatterScratch:
        target.scratch(event.value);
        break;
    case ControlId::PlatterTouch:
        if (event.value >= 0.5)
            target.beginScratch();
        else
            target.endScratch();
        break;
    case ControlId::Quantize:
        target.quantizeHotCues.store(
            event.value > 0.5, std::memory_order_release);
        break;
    case ControlId::PerformancePadMode:
    case ControlId::PerformancePad1:
    case ControlId::PerformancePad2:
    case ControlId::PerformancePad3:
    case ControlId::PerformancePad4:
    case ControlId::PerformancePad5:
    case ControlId::PerformancePad6:
    case ControlId::PerformancePad7:
    case ControlId::PerformancePad8:
        // MidiEngine forwards these UI gestures to DeckWidget, which resolves
        // programmable assignments into ordinary deck ControlEvents.
        break;
    case ControlId::Crossfader:
    case ControlId::MasterCue:
    case ControlId::HeadphoneMix:
    case ControlId::BrowseSelect:
    case ControlId::BrowseNavigate:
    case ControlId::Count:
        break;
    }
}

} // namespace gvt
