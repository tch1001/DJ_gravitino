// Protect shared Perform/editor preparation and dispatch with actual rendered
// audio. Fixtures are synthetic and never touch library files or song cues.
#include "transitions/TransitionPlayback.h"
#include "transitions/TransitionEngine.h"
#include "transitions/TransitionPlayerExt.h"
#include "transitions/TransitionTransportTrace.h"

#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); ++failures; \
} } while (0)

gvt::TrackDataPtr track(double frequency, double bpm) {
    auto t = std::make_shared<gvt::TrackData>();
    t->bpm = bpm;
    t->durationSec = 8.0;
    t->firstBeatSec = 0.25;
    t->canonicalBeatOffset = 0.375;
    t->pcm.resize(8U * gvt::kSampleRate * 2U);
    for (std::size_t i = 0; i < t->pcm.size() / 2; ++i) {
        const float value = 0.2f * static_cast<float>(std::sin(
            static_cast<double>(i) * frequency * 6.283185307179586 / gvt::kSampleRate));
        t->pcm[i * 2] = t->pcm[i * 2 + 1] = value;
    }
    t->hotCues[0] = 0.8;
    return t;
}

gvt::GvtFile fixture() {
    using namespace gvt;
    GvtFile f;
    f.sourceFormat = TransitionSourceFormat::PortableYaml;
    f.masterBpm = 120;
    f.from.bpm = 100;
    f.to.bpm = 96;
    f.anchorFromBeat = 2.75;
    f.anchorToBeat = 6.5;
    f.endBeat = 2.0;
    f.initialComplete = true;
    f.initialFrom.captured = f.initialTo.captured = true;
    f.initialFrom.positionBeat = -0.125; // intentionally differs from entry
    f.initialFrom.playing = false; // Perform still rolls the outgoing song
    f.initialFrom.cueBeat = 1.0;
    f.initialFrom.loopActive = true;
    f.initialFrom.loopStartBeat = 2.0;
    f.initialFrom.loopEndBeat = 3.0;
    f.initialTo.positionBeat = 1.0;
    f.initialTo.cueBeat = 1.0;
    f.initialTo.tempoRatio = 1.25;
    f.initialTo.fxOn = true;
    f.initialTo.fxWet = 0.2;
    f.initialTo.stemVocals = 0.9;
    TransitionHotCue cue;
    cue.id = "launch";
    cue.role = Role::ToDeck;
    cue.trackBeat = 4.125; // neither initial position nor standalone PLAY source
    cue.preferredPad = 3;
    f.transitionCues.push_back(cue);
    GvtEvent press {0.1, Role::ToDeck, ControlId::TransitionCue1, 1, Curve::Step};
    press.cueId = cue.id;
    GvtEvent release = press;
    release.beat = 0.3;
    release.value = 0;
    f.events = {press,
        {0.2, Role::ToDeck, ControlId::Play, 1, Curve::Step}, release,
        {0.5, Role::ToDeck, ControlId::FxWet, 0.8, Curve::Linear},
        {0.5, Role::ToDeck, ControlId::StemVocals, 0.5, Curve::SCurve},
        {0.7, Role::FromDeck, ControlId::LoopExit, 1, Curve::Step},
        {0.9, Role::FromDeck, ControlId::BeatJump, 1, Curve::Step},
        {1.1, Role::ToDeck, ControlId::Tempo, 1.3, Curve::Step},
        {1.2, Role::FromDeck, ControlId::Stop, 1, Curve::Step},
        {1.5, Role::ToDeck, ControlId::Fader, 0.25, Curve::Linear}};
    f.initialCrossfaderPresent = f.initialMixerCaptured = true;
    f.initialCrossfader = 1.0;
    f.events.push_back({1.75, Role::Mixer, ControlId::Crossfader, 0, Curve::Step});
    return f;
}

void audition_and_perform_render_the_same_recipe_in_either_deck_order(bool keyLock,
                                                                      int from) {
    using namespace gvt;
    ControlBus liveBus, auditionBus;
    AudioEngine live(&liveBus), audition(&auditionBus);
    const auto out = track(220, 125); // local grids corrected since authoring
    const auto in = track(330, 110);
    live.deck(from).loadTrack(out);
    live.deck(1 - from).loadTrack(in);
    audition.deck(0).loadTrack(out);
    audition.deck(1).loadTrack(in);
    live.crossfader.store(0.37f);
    live.deck(from).trim.store(0.43f);
    live.deck(1 - from).trim.store(0.61f);
    live.deck(from).preservePitch.store(keyLock);
    live.deck(1 - from).preservePitch.store(keyLock);
    copyTransitionPlaybackContext(live, from, auditionBus, audition);
    CHECK(audition.deck(0).preservePitch.load() == keyLock);
    CHECK(audition.deck(0).trim.load() == live.deck(from).trim.load());
    const GvtFile file = fixture();
    prepareTransitionSetup(liveBus, live, file, from);
    prepareTransitionSetup(auditionBus, audition, file, 0);
    positionTransitionPerform(live, file, from);
    positionTransitionPerform(audition, file, 0);
    CHECK(std::fabs(transitionBeatAtSec(file, *out, live.deck(from).positionSec())
                    - file.anchorFromBeat) < 1e-9);
    TransitionPlayer perform(&liveBus, &live), preview(&auditionBus, &audition);
    transitionPlayerUseExternalClock(&perform, true);
    transitionPlayerUseExternalClock(&preview, true);
    QString error;
    CHECK(perform.arm(file, from, false, &error));
    CHECK(preview.arm(file, 0, true, &error));
    liveBus.dispatch({from, ControlId::Play, 1}, Origin::Replay);
    auditionBus.dispatch({0, ControlId::Play, 1}, Origin::Replay);
    std::array<float, 512> a {}, b {};
    double beat = 0;
    double energy = 0;
    double maximumError = 0;
    while (beat < 2.0) {
        transitionPlayerAdvanceToBeat(&perform, beat);
        transitionPlayerAdvanceToBeat(&preview, beat);
        live.renderOffline(a.data(), 256);
        audition.renderOffline(b.data(), 256);
        for (std::size_t i = 0; i < a.size(); ++i) {
            maximumError = std::max(maximumError, static_cast<double>(std::fabs(a[i] - b[i])));
            energy += std::fabs(a[i]);
        }
        CHECK(std::fabs(live.deck(from).positionSec() - audition.deck(0).positionSec()) < 1e-9);
        CHECK(std::fabs(live.deck(1-from).positionSec() - audition.deck(1).positionSec()) < 1e-9);
        if (beat > 0.32 && beat < 0.4) {
            CHECK(live.deck(1-from).playing.load());
            CHECK(transitionBeatAtSec(file, *in, live.deck(1-from).positionSec()) < file.anchorToBeat);
        }
        beat += 256.0 / kSampleRate * live.deck(from).effectiveBpm() / 60.0;
    }
    CHECK(maximumError < 1e-5);
    CHECK(energy > 10);
    CHECK(std::fabs(live.crossfader.load() - 0.37f) < 1e-7);
    CHECK(out->hotCues[0] == 0.8 && in->hotCues[0] == 0.8);
    CHECK(file.initialFrom.positionBeat == -0.125 && !file.initialFrom.playing);
    const auto trace = buildTransitionTransportTrace(file, out, in, 2.0);
    const auto start = trace.positionAt(Role::FromDeck, 0.0);
    CHECK(start && start->audible && std::fabs(start->sourceBeat - 2.75) < 1e-8);
}

void missing_snapshot_ramps_start_from_live_fx_and_stem_values() {
    using namespace gvt;
    ControlBus bus;
    AudioEngine engine(&bus);
    engine.deck(0).loadTrack(track(220, 120));
    engine.deck(1).loadTrack(track(330, 120));
    engine.deck(1).fxWet.store(0.2f);
    engine.deck(1).stemVocals.store(0.8f);
    GvtFile file;
    file.masterBpm = 120;
    file.endBeat = 4;
    file.events = {
        {2, Role::ToDeck, ControlId::FxWet, 0.8, Curve::Linear},
        {2, Role::ToDeck, ControlId::StemVocals, 0.4, Curve::Linear}};
    TransitionPlayer player(&bus, &engine);
    transitionPlayerUseExternalClock(&player, true);
    QString error;
    CHECK(player.arm(file, 0, true, &error));
    transitionPlayerAdvanceToBeat(&player, 1);
    CHECK(std::fabs(engine.deck(1).fxWet.load() - 0.5) < 1e-6);
    CHECK(std::fabs(engine.deck(1).stemVocals.load() - 0.6) < 1e-6);
    player.abort();
    file.unsupportedRequirements << "future.unknown";
    CHECK(!player.arm(file, 0, true, &error));
    CHECK(error.contains("capabilities"));
    file.unsupportedRequirements.clear();
    file.events.front().cueId = "missing-cue";
    CHECK(!player.arm(file, 0, true, &error));
    CHECK(error.contains("unallocated"));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    for (bool keyLock : {false, true})
        for (int from : {0, 1})
            audition_and_perform_render_the_same_recipe_in_either_deck_order(keyLock, from);
    missing_snapshot_ramps_start_from_live_fx_and_stem_values();
    return failures ? 1 : 0;
}
