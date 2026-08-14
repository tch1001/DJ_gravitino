// Headless end-to-end check: loads the demo tracks, performs a scripted .gvt
// transition through the real engine in offline mode, writes selftest_out.wav.
// Run: ./build/gravitino --selftest [trackA.mp3 trackB.mp3]
#include <QCoreApplication>
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
    return 0;
}

} // namespace gvt
