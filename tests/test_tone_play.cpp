// Synthetic audio protects portable tone recipes and exact sampler timing
// without opening, tagging or changing any user audio, grids or transitions.
#include "audio/TonePlayProcessor.h"
#include "transitions/TransitionEngine.h"
#include "transitions/TransitionGraph.h"
#include "transitions/TransitionPlayback.h"
#include "transitions/TransitionPlayerExt.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c);                                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
using namespace gvt;
TrackDataPtr source() {
    auto t = std::make_shared<TrackData>();
    t->bpm = 120;
    t->firstBeatSec = .25;
    t->canonicalBeatOffset = .125;
    t->durationSec = 4;
    t->pcm.resize(4 * kSampleRate * 2);
    for (size_t i = 0; i < t->pcm.size() / 2; ++i)
        t->pcm[2 * i] = t->pcm[2 * i + 1] =
            .2f * std::sin(i * 440 * 6.283185307179586 / kSampleRate);
    t->hotCues[0] = .75;
    return t;
}
GvtFile recipe() {
    GvtFile f;
    f.id = "58900b9c-c3a9-4a4c-acf7-7e2b6223f5a8";
    f.sourceFormat = TransitionSourceFormat::PortableYaml;
    f.name = "Synthetic tone-play test";
    f.from.title = "Outgoing";
    f.to.title = "Incoming";
    f.from.bpm = f.to.bpm = f.masterBpm = 120;
    f.from.durationSec = f.to.durationSec = 4;
    f.anchorFromBeat = 1;
    f.anchorToBeat = 0;
    f.initialComplete = true;
    f.initialFrom.captured = f.initialTo.captured = true;
    f.initialFrom.fader = f.initialTo.fader = 0;
    f.endBeat = 2;
    TonePlayPattern p;
    p.sourceStartBeat = -.25;
    p.sourceEndBeat = .75;
    p.notes = {{.125, .25, 60, .8}, {.5, .5, 72, .6}};
    f.tonePlay = p;
    return f;
}
void yaml_preserves_notes_precision_and_unknown_fields_and_requires_capability() {
    auto f = recipe();
    f.tonePlay->sourceStartBeat = -.123456789012;
    f.tonePlay->notes[0].beat = .123456789012;
    f.tonePlay->extraYaml["future.option"] = "retained";
    f.tonePlay->notes[0].extraYaml["future.articulation"] = 3;
    const auto yaml = transitionSerialize(f);
    QString error;
    GvtFile parsed;
    CHECK(transitionParse(yaml, parsed, &error));
    CHECK(parsed.tonePlay.has_value());
    if (!parsed.tonePlay)
        return;
    CHECK(parsed.requirements.contains("tone-play.v1"));
    CHECK(parsed.requirements.contains("timeline.v1"));
    CHECK(!parsed.tonePlay->replaceOutgoing);
    CHECK(parsed.tonePlay->sourceStartBeat == f.tonePlay->sourceStartBeat);
    CHECK(parsed.tonePlay->notes[0].beat == f.tonePlay->notes[0].beat);
    CHECK(parsed.tonePlay->notes[0].extraYaml == f.tonePlay->notes[0].extraYaml);
    CHECK(transitionSerialize(parsed) == yaml);
    auto missing = yaml;
    missing.replace("tone-play.v1", "unrelated.v1");
    CHECK(!transitionParse(missing, parsed, &error));
    CHECK(error.contains("tone-play.v1"));
    f.endBeat = .1;
    CHECK(!transitionParse(transitionSerialize(f), parsed, &error));
    f.endBeat.reset();
    CHECK(std::abs(transitionGraphEffectiveEndBeat(f) - 2.0) < 1e-9);
    auto p = *f.tonePlay;
    p.notes.assign(17, {.25, .5, 60, 1});
    CHECK(!validateTonePlay(p, &error));
    p.notes.resize(16);
    CHECK(validateTonePlay(p, &error));
    p.notes[0].pitch = 96;
    CHECK(!validateTonePlay(p, &error));
    p.notes[0].pitch = 60;
    p.notes[0].duration = 0;
    CHECK(!validateTonePlay(p, &error));
    p.notes.clear();
    p.gain = INFINITY;
    CHECK(!validateTonePlay(p, &error));
    auto json = serializeTonePlay(*f.tonePlay);
    json["replace_outgoing"] = "false";
    std::optional<TonePlayPattern> result;
    CHECK(!parseTonePlay(json, result, &error));
    auto notes = json["notes"].toArray();
    auto n = notes[0].toObject();
    n["pitch"] = 60.5;
    notes[0] = n;
    json["notes"] = notes;
    json["replace_outgoing"] = false;
    CHECK(!parseTonePlay(json, result, &error));
}
std::vector<float> render(TonePlayProcessor &sampler, int frames, int block, double tempo = 1,
                          double start = 0) {
    std::vector<float> result(frames * 2), gain(frames);
    for (int i = 0; i < frames; i += block) {
        const int count = std::min(block, frames - i);
        sampler.render(result.data() + i * 2, gain.data() + i, count,
                       start + i * tempo / kSampleRate, true, tempo);
    }
    return result;
}
void notes_are_sample_clocked_block_invariant_and_pitched_without_changing_source() {
    const auto t = source();
    auto p = *recipe().tonePlay;
    p.notes = {{.125, .5, 60, 1}};
    TonePlayProcessor a, b;
    QString error;
    CHECK(a.prepare(*t, p, 0, .5, .125, &error));
    CHECK(b.prepare(*t, p, 0, .5, .125, &error));
    a.enable(true);
    b.enable(true);
    const auto one = render(a, 48000, 1), block = render(b, 48000, 257);
    CHECK(one == block);
    const int onset = 9000; // anchor .125 s + note .125 beat at 120 BPM
    CHECK(std::all_of(one.begin(), one.begin() + onset * 2, [](float f) { return f == 0; }));
    CHECK(std::abs(one[(onset + 100) * 2]) > .001);
    CHECK(std::all_of(one.begin() + 21000 * 2, one.end(), [](float f) { return f == 0; }));
    CHECK(std::abs(a.beat() - 1.75) < 1e-8);
    p.notes = {{0, 1, 72, 1}};
    CHECK(a.prepare(*t, p, 0, 1, 0, &error));
    a.enable(true);
    auto pitched = render(a, 12000, 256);
    int crossings = 0;
    for (int i = 1000; i < 10000; ++i)
        if (pitched[(i - 1) * 2] < 0 && pitched[i * 2] >= 0)
            ++crossings;
    CHECK(crossings >= 164 && crossings <= 166); // 880 Hz, one octave up
    CHECK(t->hotCues[0] == .75 && t->firstBeatSec == .25);
    a.enable(false);
    auto silence = render(a, 256, 256);
    CHECK(std::all_of(silence.begin(), silence.end(), [](float f) { return f == 0; }));
    CHECK(!a.prepare(*t, p, -.01, 1, 0, &error));
    CHECK(!a.prepare(*t, p, 0, 5, 0, &error));
}
void layering_is_default_and_replacement_only_ducks_the_authored_phrase() {
    const auto t = source();
    auto p = *recipe().tonePlay;
    p.notes = {{.25, .5, 60, 1}};
    TonePlayProcessor sampler;
    QString error;
    std::vector<float> out(48000), gain(24000);
    CHECK(!p.replaceOutgoing);
    CHECK(sampler.prepare(*t, p, 0, .5, 0, &error));
    sampler.enable(true);
    sampler.render(out.data(), gain.data(), 24000, 0, true, 1);
    CHECK(std::all_of(gain.begin(), gain.end(), [](float v) { return v == 1; }));
    p.replaceOutgoing = true;
    CHECK(sampler.prepare(*t, p, 0, .5, 0, &error));
    sampler.enable(true);
    sampler.render(out.data(), gain.data(), 256, 0, false, 1);
    CHECK(sampler.beat() == -1);
    CHECK(std::all_of(out.begin(), out.begin() + 512, [](float v) { return v == 0; }));
    sampler.render(out.data(), gain.data(), 24000, 0, true, 1);
    CHECK(gain[5900] == 1 && gain[6300] == 0 && gain[17999] > .99 && gain[18000] == 1);
}
void loops_stops_and_jumps_do_not_retrigger_the_pattern_and_tempo_changes_retime_gates() {
    auto p = *recipe().tonePlay;
    p.notes = {{0, 4, 60, 1}, {1, 1, 67, .8}};
    TonePlaySustain h;
    h.loopStartBeat = 0; h.loopEndBeat = .5; h.duration = 4;
    p.sustain = h;
    const auto t = source();
    QString error;
    TonePlayProcessor a, b;
    CHECK(a.prepare(*t, p, 0, 2, 0, &error));
    CHECK(b.prepare(*t, p, 0, 2, 0, &error));
    a.enable(true);
    b.enable(true);
    std::array<float, 512> x{}, y{};
    std::array<float, 256> gains{};
    double expectedBeat = 0;
    for (int i = 0; i < 300; ++i) {
        const double tempo = i < 100 ? 1 : 1.5;
        a.render(x.data(), gains.data(), 256, i * 256.0 / kSampleRate, true, tempo);
        b.render(y.data(), gains.data(), 256, i % 7 * .01, i < 10, tempo);
        CHECK(x == y);
        expectedBeat += 256.0 / kSampleRate * 2 * tempo;
    }
    CHECK(std::abs(a.beat() - expectedBeat) < 1e-8);
    CHECK(std::all_of(x.begin(), x.end(), [](float f) { return f == 0; }));
}
void perform_and_preview_share_sampler_audio_in_both_deck_orders(int outgoing) {
    const auto t = source();
    auto f = recipe();
    TonePlaySustain h;
    h.loopStartBeat = 0; h.loopEndBeat = .5; h.duration = 1.5;
    f.tonePlay->sustain = h;
    f.tonePlay->effects = TonePlayEffects{};
    f.tonePlay->effects->tailBeats = .5;
    f.tonePlay->effects->sustainDucking = .8;
    QString error;
    ControlBus busA, busB;
    AudioEngine a(&busA), b(&busB);
    a.deck(outgoing).loadTrack(t);
    a.deck(1 - outgoing).loadTrack(t);
    b.deck(0).loadTrack(t);
    b.deck(1).loadTrack(t);
    prepareTransitionSetup(busA, a, f, outgoing);
    positionTransitionPerform(a, f, outgoing);
    prepareTransitionSetup(busB, b, f, 0);
    positionTransitionPerform(b, f, 0);
    TransitionPlayer perform(&busA, &a), preview(&busB, &b);
    transitionPlayerUseExternalClock(&perform, true);
    transitionPlayerUseExternalClock(&preview, true);
    CHECK(perform.arm(f, outgoing, false, &error));
    CHECK(preview.arm(f, 0, true, &error));
    busA.dispatch({outgoing, ControlId::Play, 1}, Origin::Replay);
    busB.dispatch({0, ControlId::Play, 1}, Origin::Replay);
    std::array<float, 512> x{}, y{};
    double beat = 0, energy = 0;
    for (int chunk = 0; chunk < 100; ++chunk) {
        transitionPlayerAdvanceToBeat(&perform, beat);
        transitionPlayerAdvanceToBeat(&preview, beat);
        a.renderOffline(x.data(), 256);
        b.renderOffline(y.data(), 256);
        for (size_t i = 0; i < x.size(); ++i) {
            CHECK(std::abs(x[i] - y[i]) < 1e-7);
            energy += std::abs(x[i]);
        }
        beat += 256.0 / kSampleRate * 2;
    }
    CHECK(energy > 100);
    CHECK(std::abs(a.tonePlayBeat() - beat) < 1e-8);
    perform.abort();
    preview.abort();
    a.renderOffline(x.data(), 256);
    CHECK(std::all_of(x.begin(), x.end(), [](float v) { return v == 0; }));
    transitionPlayerSetMode(&perform, PlayerMode::Tutorial);
    CHECK(!perform.arm(f, outgoing, true, &error));
    CHECK(error.contains("Tutor"));
}
void replacing_prepared_samples_safely_drains_active_callbacks() {
    TonePlayProcessor sampler;
    const auto t = source();
    const auto p = *recipe().tonePlay;
    std::atomic<bool> done{false};
    QString error;
    std::thread reader([&] {
        std::array<float, 512> out{};
        std::array<float, 256> gain{};
        while (!done.load())
            sampler.render(out.data(), gain.data(), 256, 0, true, 1);
    });
    for (int i = 0; i < 30; ++i) {
        CHECK(sampler.prepare(*t, p, 0, .2, 0, &error));
        sampler.enable(true);
        sampler.clear();
    }
    done.store(true);
    reader.join();
}

TonePlaySustain holdFor(const TonePlayPattern &p) {
    TonePlaySustain h;
    h.beat = .125; h.duration = 4;
    h.loopStartBeat = p.sourceStartBeat + .2;
    h.loopEndBeat = p.sourceEndBeat - .1;
    return h;
}
void sustain_is_portable_capability_gated_and_does_not_change_old_notes() {
    auto f = recipe();
    f.tonePlay->sustain = holdFor(*f.tonePlay);
    f.tonePlay->sustain->extraYaml["future.expression"] = "kept";
    f.endBeat = 5;
    QString error;
    GvtFile parsed;
    const auto yaml = transitionSerialize(f);
    CHECK(transitionParse(yaml, parsed, &error));
    CHECK(parsed.requirements.contains("tone-play-sustain.v1"));
    CHECK(parsed.unsupportedRequirements.isEmpty());
    CHECK(parsed.tonePlay && parsed.tonePlay->sustain);
    CHECK(transitionSerialize(parsed) == yaml);
    CHECK(parsed.tonePlay->sustain->extraYaml == f.tonePlay->sustain->extraYaml);
    CHECK(tonePlayEndBeat(*parsed.tonePlay) == 4.125);
    auto missing = yaml;
    missing.replace("tone-play-sustain.v1", "future.v9");
    CHECK(!transitionParse(missing, parsed, &error));
    CHECK(error.contains("tone-play-sustain.v1"));
    f.endBeat = 2;
    CHECK(!transitionParse(transitionSerialize(f), parsed, &error));
    const auto valid = *f.tonePlay;
    for (int field = 0; field < 7; ++field) {
        auto bad = valid;
        auto &h = *bad.sustain;
        if (field == 0) h.duration = NAN;
        if (field == 1) h.loopStartBeat = valid.sourceStartBeat - .01;
        if (field == 2) h.loopEndBeat = h.loopStartBeat;
        if (field == 3) h.crossfadeMs = 0;
        if (field == 4) h.pitch = 96;
        if (field == 5) h.gain = 2;
        if (field == 6) h.attackMs = INFINITY;
        CHECK(!validateTonePlay(bad, &error));
    }
    auto json = serializeTonePlay(valid);
    auto h = json["sustain"].toObject();
    h["pitch"] = 60.5; json["sustain"] = h;
    std::optional<TonePlayPattern> result;
    CHECK(!parseTonePlay(json, result, &error));
    json["sustain"] = false;
    CHECK(!parseTonePlay(json, result, &error));
    auto legacy = recipe();
    CHECK(!transitionSerialize(legacy).contains("sustain"));
}
void sustained_voice_runs_under_independent_hits_and_is_block_invariant() {
    const auto t = source();
    auto p = *recipe().tonePlay;
    p.notes = {{.25, .0625, 60, 1}, {1.25, .0625, 67, .7}, {2.25, .0625, 60, 1}};
    p.sustain = holdFor(p);
    p.sustain->duration = 3;
    p.sustain->crossfadeMs = 17.3; // not a pitch-period multiple
    auto hits = p; hits.sustain.reset();
    auto background = p; background.notes.clear();
    TonePlayProcessor both, singleFrame, onlyHits, onlyHold;
    QString error;
    for (auto *s : {&both, &singleFrame}) {
        CHECK(s->prepare(*t, p, 0, .5, 0, &error)); s->enable(true);
    }
    CHECK(onlyHits.prepare(*t, hits, 0, .5, 0, &error)); onlyHits.enable(true);
    CHECK(onlyHold.prepare(*t, background, 0, .5, 0, &error)); onlyHold.enable(true);
    const auto mix = render(both, 96000, 257);
    CHECK(mix == render(singleFrame, 96000, 1));
    const auto foreground = render(onlyHits, 96000, 511);
    const auto bed = render(onlyHold, 96000, 256);
    double errorSum = 0, heldEnergy = 0, maxJump = 0;
    for (size_t i = 0; i < mix.size(); ++i) {
        CHECK(std::isfinite(mix[i]));
        errorSum += std::abs(mix[i] - foreground[i] - bed[i]);
        if (i > 48000 && i < 120000) heldEnergy += std::abs(bed[i]);
        if (i >= 2) maxJump = std::max(maxJump, std::abs(double(bed[i] - bed[i-2])));
    }
    CHECK(errorSum < .001);
    CHECK(heldEnergy > 100); // sustains long after the half-second source ends
    CHECK(maxJump < .01); // smooth joins rather than hard restarts
    CHECK(std::all_of(mix.begin() + 151000, mix.end(), [](float v) { return v == 0; }));
    // Disable is immediate; the old one-shot path is exactly unchanged.
    p.sustain->enabled = false;
    CHECK(both.prepare(*t, p, 0, .5, 0, &error)); both.enable(true);
    CHECK(render(both, 96000, 256) == foreground);
    p.sustain->enabled = true;
    p.sustain->loopEndBeat = p.sustain->loopStartBeat + .001;
    CHECK(!both.prepare(*t, p, 0, .5, 0, &error));
}
void note_effects_round_trip_with_required_capability_and_bounded_parameters() {
    auto f = recipe();
    f.tonePlay->effects = TonePlayEffects{};
    f.tonePlay->effects->sustainDucking = .8;
    f.tonePlay->effects->extraYaml["future.color"] = "warm";
    f.endBeat = 6;
    QString error;
    GvtFile parsed;
    const auto yaml = transitionSerialize(f);
    CHECK(transitionParse(yaml, parsed, &error));
    CHECK(parsed.requirements.contains("tone-play-effects.v1"));
    CHECK(parsed.unsupportedRequirements.isEmpty());
    CHECK(transitionSerialize(parsed) == yaml);
    CHECK(tonePlayEndBeat(*f.tonePlay) == 5);
    auto missing = yaml;
    missing.replace("tone-play-effects.v1", "future.v9");
    CHECK(!transitionParse(missing, parsed, &error));
    CHECK(error.contains("tone-play-effects.v1"));
    f.endBeat = 2;
    CHECK(!transitionParse(transitionSerialize(f), parsed, &error));
    for (int i = 0; i < 6; ++i) {
        auto p = *f.tonePlay;
        if (i == 0) p.effects->echo = NAN;
        if (i == 1) p.effects->reverb = 1.1;
        if (i == 2) p.effects->echoBeats = .125;
        if (i == 3) p.effects->tailBeats = INFINITY;
        if (i == 4) p.effects->tailBeats = 17;
        if (i == 5) p.effects->sustainDucking = -1;
        CHECK(!validateTonePlay(p, &error));
    }
    CHECK(!transitionSerialize(recipe()).contains("effects"));
}
void note_tails_preserve_dry_attacks_stop_cleanly_and_never_process_the_background() {
    const auto t = source();
    auto p = *recipe().tonePlay;
    p.notes = {{.125, .0625, 61, 1}};
    const auto audioFor = [&](const TonePlayPattern &pattern, int block = 257, double tempo = 1) {
        TonePlayProcessor s;
        QString error;
        CHECK(s.prepare(*t, pattern, 0, .5, 0, &error));
        s.enable(true);
        auto audio = render(s, 96000, block, tempo);
        s.enable(false);
        const auto silence = render(s, 256, 256);
        CHECK(std::all_of(silence.begin(), silence.end(), [](float x) { return x == 0; }));
        return audio;
    };
    const auto dry = audioFor(p);
    p.effects = TonePlayEffects{};
    p.effects->tailBeats = 2;
    const auto wet = audioFor(p);
    CHECK(wet == audioFor(p, 1));
    double difference = 0, tailEnergy = 0;
    for (int i = 3000; i < 4000; ++i)
        difference += std::abs(wet[i*2] - dry[i*2]);
    CHECK(difference < .0001); // no return before the earliest reverb delay
    for (int i = 6000; i < 30000; ++i) tailEnergy += wet[i*2]*wet[i*2];
    CHECK(tailEnergy > .001);
    CHECK(std::all_of(wet.begin()+105000, wet.end(), [](float x) { return x == 0; }));
    p.effects->enabled = false;
    CHECK(audioFor(p) == dry);
    p.effects->enabled = true;
    p.effects->reverb = 0;
    p.effects->echo = .1;
    const auto fast = audioFor(p, 257, 2);
    double echoEnergy = 0;
    for (int i = 4600; i < 5000; ++i) echoEnergy += fast[i*2]*fast[i*2];
    CHECK(echoEnergy > .0001); // quarter-beat echo follows doubled tempo
    p.sustain = holdFor(p);
    p.notes.clear();
    const auto held = audioFor(p);
    p.effects.reset();
    CHECK(audioFor(p) == held); // long vowel is not fed into note FX
    p.notes = {{.5, .0625, 61, 1}};
    auto dryNotes = p; dryNotes.sustain.reset();
    const auto hits = audioFor(dryNotes);
    p.effects = TonePlayEffects{};
    p.effects->echo = p.effects->reverb = 0;
    p.effects->sustainDucking = .9;
    const auto ducked = audioFor(p);
    double bed = 0, duckedBed = 0;
    for (int i = 12300; i < 13200; ++i) {
        bed += held[i*2]*held[i*2];
        duckedBed += std::pow(ducked[i*2]-hits[i*2],2);
    }
    CHECK(duckedBed < bed*.1);
    p.sustain.reset();
    CHECK(audioFor(p) == hits); // ducking cannot alter a solo C-sharp note
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    yaml_preserves_notes_precision_and_unknown_fields_and_requires_capability();
    notes_are_sample_clocked_block_invariant_and_pitched_without_changing_source();
    layering_is_default_and_replacement_only_ducks_the_authored_phrase();
    loops_stops_and_jumps_do_not_retrigger_the_pattern_and_tempo_changes_retime_gates();
    for (int deck : {0, 1})
        perform_and_preview_share_sampler_audio_in_both_deck_orders(deck);
    replacing_prepared_samples_safely_drains_active_callbacks();
    sustain_is_portable_capability_gated_and_does_not_change_old_notes();
    sustained_voice_runs_under_independent_hits_and_is_block_invariant();
    note_effects_round_trip_with_required_capability_and_bounded_parameters();
    note_tails_preserve_dry_attacks_stop_cleanly_and_never_process_the_background();
    return failures ? 1 : 0;
}
