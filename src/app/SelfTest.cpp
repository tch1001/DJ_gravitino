// Headless end-to-end check: loads the demo tracks, performs a scripted .gvt
// transition through the real engine in offline mode, writes selftest_out.wav.
// Run: ./build/gravitino --selftest [trackA.mp3 trackB.mp3]
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QtGlobal>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../analysis/TrackData.h"
#include "../audio/AudioEngine.h"
#include "../control/ControlBus.h"
#include "../transitions/Transition.h"
#include "../transitions/TransitionEngine.h"

namespace gvt {

namespace {

bool writeWav16(const QString& path, const std::vector<float>& interleaved, int rate) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const uint32_t dataBytes = (uint32_t)(interleaved.size() * 2);
    auto put32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto put16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF"); put32(36 + dataBytes); f.write("WAVE");
    f.write("fmt "); put32(16); put16(1); put16(2);
    put32((uint32_t)rate); put32((uint32_t)rate * 4); put16(4); put16(16);
    f.write("data"); put32(dataBytes);
    for (float s : interleaved) {
        int v = (int)std::lround(qBound(-1.0f, s, 1.0f) * 32767.0f);
        put16((uint16_t)(int16_t)v);
    }
    return true;
}

double rmsRange(const std::vector<float>& b, size_t beginFrame, size_t endFrame) {
    double acc = 0; size_t n = 0;
    for (size_t i = beginFrame * 2; i < endFrame * 2 && i < b.size(); ++i) { acc += (double)b[i] * b[i]; ++n; }
    return n ? std::sqrt(acc / (double)n) : 0.0;
}

} // namespace

int runSelfTest(const QStringList& args) {
    std::printf("== Gravitino selftest ==\n");
    QString pathA = QDir::homePath() + "/Music/PioneerDJ/Demo Tracks/Demo Track 1.mp3";
    QString pathB = QDir::homePath() + "/Music/PioneerDJ/Demo Tracks/Demo Track 2.mp3";
    QStringList extra;
    for (const QString& a : args)
        if (a.endsWith(".mp3", Qt::CaseInsensitive)) extra << a;
    if (extra.size() >= 2) { pathA = extra[0]; pathB = extra[1]; }

    QString err;
    TrackDataPtr ta = loadAndAnalyzeTrack(pathA, &err);
    if (!ta) { std::printf("FAIL: load A (%s): %s\n", qPrintable(pathA), qPrintable(err)); return 1; }
    TrackDataPtr tb = loadAndAnalyzeTrack(pathB, &err);
    if (!tb) { std::printf("FAIL: load B (%s): %s\n", qPrintable(pathB), qPrintable(err)); return 1; }
    std::printf("A: %-28s bpm=%.2f firstBeat=%.3fs dur=%.1fs fp=%s\n", qPrintable(ta->title),
                ta->bpm, ta->firstBeatSec, ta->durationSec, qPrintable(ta->fingerprint));
    std::printf("B: %-28s bpm=%.2f firstBeat=%.3fs dur=%.1fs fp=%s\n", qPrintable(tb->title),
                tb->bpm, tb->firstBeatSec, tb->durationSec, qPrintable(tb->fingerprint));
    if (ta->bpm < 60 || ta->bpm > 200 || tb->bpm < 60 || tb->bpm > 200) {
        std::printf("FAIL: implausible BPM\n"); return 1;
    }

    ControlBus bus;
    AudioEngine engine(&bus); // offline: never call start()
    engine.deck(0).loadTrack(ta);
    engine.deck(1).loadTrack(tb);

    // Scripted 16-beat blend, built as text to exercise the .gvt parser too.
    const QString gvtText = QStringLiteral(
        "gravitino-transition 1\n"
        "[meta]\nname = selftest blend\nauthor = selftest\n"
        "[from]\ntitle = %1\nbpm = %2\n"
        "[to]\ntitle = %3\nbpm = %4\n"
        "[sync]\nanchor_from = 16.0\nanchor_to = 0.0\nmaster_bpm = %2\n"
        "[events]\n"
        "0.0   b  tempo_sync\n"
        "0.0   b  fader 1.0\n"
        "0.0   b  eq_low 0.0\n"
        "0.0   b  play\n"
        "0.0   x  xfader 0.0\n"
        "12.0  x  xfader 1.0 linear\n"
        "12.0  b  eq_low 0.5 linear\n"
        "14.0  a  stop\n")
        .arg(ta->title).arg(ta->bpm, 0, 'f', 2).arg(tb->title).arg(tb->bpm, 0, 'f', 2);

    GvtFile file;
    QStringList warnings;
    if (!gvtParse(gvtText, file, &err, &warnings)) {
        std::printf("FAIL: gvtParse: %s\n", qPrintable(err)); return 1;
    }
    for (const QString& w : warnings) std::printf("warn: %s\n", qPrintable(w));
    if (file.events.size() != 8) { std::printf("FAIL: expected 8 events, got %zu\n", file.events.size()); return 1; }

    // Round-trip through the serializer to a real file and back.
    const QString gvtPath = "selftest_transition.gvt";
    if (!gvtSaveFile(file, gvtPath, &err)) { std::printf("FAIL: gvtSaveFile: %s\n", qPrintable(err)); return 1; }
    GvtFile file2;
    if (!gvtLoadFile(gvtPath, file2, &err, nullptr) || file2.events.size() != file.events.size()) {
        std::printf("FAIL: round-trip load: %s\n", qPrintable(err)); return 1;
    }

    ControlBus& busRef = bus;
    busRef.dispatch({0, ControlId::Play, 1.0}, Origin::System);

    TransitionPlayer player(&bus, &engine);
    if (!player.arm(file2, /*fromDeck=*/0, /*startNow=*/false, &err)) {
        std::printf("FAIL: player.arm: %s\n", qPrintable(err)); return 1;
    }
    bool done = false, completed = false;
    QObject::connect(&player, &TransitionPlayer::finished,
                     [&](bool ok) { done = true; completed = ok; });

    // Offline render loop; process events so the player's timer fires.
    const int chunk = 256;
    std::vector<float> out;
    std::vector<float> buf(chunk * 2);
    const double secondsToRender = engine.deck(0).track()->secAtBeat(16.0 + 20.0) + 2.0;
    const int64_t totalFrames = (int64_t)(secondsToRender * kSampleRate);
    for (int64_t rendered = 0; rendered < totalFrames && !done; rendered += chunk) {
        engine.renderOffline(buf.data(), chunk);
        out.insert(out.end(), buf.begin(), buf.end());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    // Offline rendering outruns wall time; after the final FromDeck stop the
    // player clock runs on wall time, so give the event loop a few real
    // seconds to let the schedule finish (live playback is real time anyway).
    {
        QElapsedTimer et; et.start();
        while (!done && et.elapsed() < 4000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    std::printf("rendered %.1fs, player %s\n", (double)out.size() / 2.0 / kSampleRate,
                done ? (completed ? "completed" : "aborted") : "STILL ACTIVE");
    if (!done || !completed) { std::printf("FAIL: transition did not complete\n"); return 1; }

    // Sanity on the audio: signal present, no NaN, and after 'a stop' the tail
    // (deck B solo) is still audible.
    bool nan = false; float peak = 0;
    for (float s : out) { if (std::isnan(s)) nan = true; peak = std::max(peak, std::abs(s)); }
    size_t frames = out.size() / 2;
    double rmsHead = rmsRange(out, 0, frames / 4);
    double rmsTail = rmsRange(out, frames * 9 / 10, frames);
    std::printf("peak=%.3f rmsHead=%.4f rmsTail=%.4f nan=%d\n", peak, rmsHead, rmsTail, (int)nan);
    if (nan || peak < 0.01 || rmsHead < 0.001 || rmsTail < 0.001) {
        std::printf("FAIL: audio sanity\n"); return 1;
    }
    if (!writeWav16("selftest_out.wav", out, kSampleRate)) {
        std::printf("FAIL: wav write\n"); return 1;
    }
    std::printf("OK: wrote selftest_out.wav (%zu frames) — listen to verify the blend\n", frames);

    // ---- Recorder round-trip: capture live-style events, save, reload ------
    engine.deck(0).seekSec(ta->firstBeatSec);
    engine.deck(1).seekSec(tb->firstBeatSec);
    bus.dispatch({0, ControlId::Play, 1.0}, Origin::System); // from-deck rolling
    TransitionRecorder rec(&bus, &engine);
    rec.start(0);
    struct Step { double atSec; ControlEvent e; };
    const Step steps[] = {
        {1.0, {1, ControlId::Play, 1.0}},
        {2.0, {kNoDeck, ControlId::Crossfader, 0.3}},
        {4.0, {kNoDeck, ControlId::Crossfader, 0.7}},
        {5.0, {0, ControlId::EqLow, 0.0}},
        {6.0, {kNoDeck, ControlId::Crossfader, 1.0}},
    };
    size_t next = 0;
    for (int64_t rendered = 0; rendered < (int64_t)(8.0 * kSampleRate); rendered += chunk) {
        while (next < std::size(steps) &&
               rendered >= (int64_t)(steps[next].atSec * kSampleRate)) {
            bus.dispatch(steps[next].e, Origin::Ui);
            ++next;
        }
        engine.renderOffline(buf.data(), chunk);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    GvtFile recd = rec.finish();
    std::printf("recorder: %zu events, anchor_from=%.2f master_bpm=%.2f\n",
                recd.events.size(), recd.anchorFromBeat, recd.masterBpm);
    if (recd.events.size() < 5) { std::printf("FAIL: recorder lost events\n"); return 1; }
    if (recd.masterBpm < 60 || recd.masterBpm > 200) { std::printf("FAIL: recorder bpm\n"); return 1; }
    recd.name = "selftest recorded";
    if (!gvtSaveFile(recd, "selftest_recorded.gvt", &err)) {
        std::printf("FAIL: save recorded: %s\n", qPrintable(err)); return 1;
    }
    GvtFile recd2;
    if (!gvtLoadFile("selftest_recorded.gvt", recd2, &err, nullptr) ||
        recd2.events.size() != recd.events.size()) {
        std::printf("FAIL: recorded round-trip\n"); return 1;
    }
    std::printf("OK: recorder round-trip (selftest_recorded.gvt)\n");

    // ---- Loop + beat-jump sanity through the real render path -------------
    Deck& da = engine.deck(0);
    da.seekSec(ta->secAtBeat(32.0));
    bus.dispatch({0, ControlId::Play, 1.0}, Origin::System);
    bus.dispatch({0, ControlId::LoopAuto, 4.0}, Origin::System);
    if (!da.loopActive.load()) { std::printf("FAIL: loopAuto did not activate\n"); return 1; }
    const double ls = da.loopStartSec.load(), le = da.loopEndSec.load();
    const double lenBeats = (le - ls) * ta->bpm / 60.0;
    if (std::fabs(lenBeats - 4.0) > 0.05) {
        std::printf("FAIL: loop length %.3f beats (want 4)\n", lenBeats); return 1;
    }
    for (int i = 0; i < (int)(6.0 * kSampleRate / chunk); ++i) // 6 s > loop len
        engine.renderOffline(buf.data(), chunk);
    const double posInLoop = da.positionSec();
    if (posInLoop < ls - 0.05 || posInLoop > le + 0.05) {
        std::printf("FAIL: position %.3f escaped loop [%.3f, %.3f]\n",
                    posInLoop, ls, le); return 1;
    }
    bus.dispatch({0, ControlId::LoopExit, 1.0}, Origin::System);
    if (da.loopActive.load()) { std::printf("FAIL: loopExit ignored\n"); return 1; }
    const double beatBefore = da.beatPosition();
    bus.dispatch({0, ControlId::BeatJump, 8.0}, Origin::System);
    const double jumped = da.beatPosition() - beatBefore;
    if (std::fabs(jumped - 8.0) > 0.6) {
        std::printf("FAIL: beat_jump moved %.2f beats (want ~8)\n", jumped); return 1;
    }
    std::printf("OK: loop wrap + exit + beat_jump (+%.2f beats)\n", jumped);

    // ---- Stems: fake set (vocals = master, rest silent) through render ----
    {
        auto stemsSet = std::make_shared<StemSet>();
        const size_t n = ta->pcm.size();
        stemsSet->vocals.resize(n);
        for (size_t i = 0; i < n; ++i)
            stemsSet->vocals[i] = (int16_t)std::lround(
                qBound(-1.0f, ta->pcm[i], 1.0f) * 32767.0f);
        stemsSet->melody.assign(n, 0);
        stemsSet->bass.assign(n, 0);
        stemsSet->drums.assign(n, 0);
        da.attachStems(stemsSet);
        if (!da.stemsAttached()) { std::printf("FAIL: attachStems\n"); return 1; }
        // Isolate deck A in the mix: silence deck B, crossfader full A.
        bus.dispatch({1, ControlId::Stop, 1.0}, Origin::System);
        bus.dispatch({kNoDeck, ControlId::Crossfader, 0.0}, Origin::System);
        da.seekSec(30.0);
        bus.dispatch({0, ControlId::Play, 1.0}, Origin::System);

        auto renderRms = [&](double sec) {
            std::vector<float> acc;
            for (int i = 0; i < (int)(sec * kSampleRate / chunk); ++i) {
                engine.renderOffline(buf.data(), chunk);
                acc.insert(acc.end(), buf.begin(), buf.end());
            }
            return rmsRange(acc, 0, acc.size() / 2);
        };
        const double rmsFull = renderRms(1.0);          // gains all 1 → master
        bus.dispatch({0, ControlId::StemVocals, 0.0}, Origin::System);
        bus.dispatch({0, ControlId::StemMelody, 0.0}, Origin::System);
        bus.dispatch({0, ControlId::StemBass, 0.0}, Origin::System);
        bus.dispatch({0, ControlId::StemDrums, 0.0}, Origin::System);
        renderRms(0.2); // let EQ/filter biquad state ring down (~10 ms)
        const double rmsMuted = renderRms(1.0);         // everything muted
        bus.dispatch({0, ControlId::StemVocals, 1.0}, Origin::System);
        const double rmsVoc = renderRms(1.0);           // vocals-only = master
        std::printf("stems: full=%.4f muted=%.5f vocalsOnly=%.4f\n",
                    rmsFull, rmsMuted, rmsVoc);
        if (rmsMuted > 1e-4 || rmsFull < 0.01 ||
            std::fabs(rmsVoc - rmsFull) > 0.25 * rmsFull) {
            std::printf("FAIL: stem mixing\n"); return 1;
        }
        std::printf("OK: stem attach + mute + solo through render path\n");
    }
    return 0;
}

} // namespace gvt
