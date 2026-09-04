// Exclusive editor-preview routing must neither advance nor mutate live decks.
#include "audio/AudioEngine.h"
#include "control/ControlBus.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

struct ConstantPreview final : gvt::AudioPreviewSource {
    void read(float* output, int frames) noexcept override {
        for (int i = 0; i < frames * 2; ++i) output[i] = 0.25f;
    }
};

gvt::TrackDataPtr track()
{
    auto result = std::make_shared<gvt::TrackData>();
    result->durationSec = 2.0;
    result->bpm = 120.0;
    result->pcm.resize(static_cast<std::size_t>(gvt::kSampleRate) * 4U, 0.1f);
    return result;
}
}

int main()
{
    gvt::ControlBus bus;
    gvt::AudioEngine engine(&bus);
    engine.deck(0).loadTrack(track());
    engine.deck(0).play();
    const double before = engine.deck(0).positionSec();
    ConstantPreview preview;
    QString error;
    CHECK(engine.acquireExclusivePreview(&preview, &error));
    CHECK(engine.exclusivePreviewActive());
    float output[512] {};
    engine.renderOffline(output, 256);
    CHECK(std::fabs(output[0] - 0.25f) < 1e-6f);
    CHECK(std::fabs(engine.deck(0).positionSec() - before) < 1e-9);
    bus.dispatch({0, gvt::ControlId::Fader, 0.1}, gvt::Origin::Ui);
    CHECK(std::fabs(engine.deck(0).fader.load() - 1.0f) < 1e-6f);
    bus.dispatch({0, gvt::ControlId::Fader, 0.2}, gvt::Origin::Replay);
    CHECK(std::fabs(engine.deck(0).fader.load() - 1.0f) < 1e-6f);
    bus.dispatch({0, gvt::ControlId::Stop, 1.0}, gvt::Origin::System);
    CHECK(engine.deck(0).playing.load());
    float fourChannel[64 * 4] {};
    engine.renderOfflineFourChannel(fourChannel, 64);
    CHECK(std::fabs(fourChannel[0] - 0.25f) < 1e-6f);
    CHECK(std::fabs(fourChannel[1] - 0.25f) < 1e-6f);
    CHECK(std::fabs(fourChannel[2]) < 1e-9f);
    CHECK(std::fabs(fourChannel[3]) < 1e-9f);
    engine.releaseExclusivePreview(&preview);
    CHECK(!engine.exclusivePreviewActive());
    engine.renderOffline(output, 256);
    CHECK(engine.deck(0).positionSec() > before);
    if (failures) return 1;
    std::printf("test_audio_preview: exclusive source isolation passed\n");
    return 0;
}
