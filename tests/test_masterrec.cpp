// test_masterrec — MasterRecorder end-to-end: start into a temp WAV, feed a
// 440 Hz sine from this thread, stop, and validate the written file.
#include "audio/MasterRecorder.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
uint32_t u32le(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}
uint16_t u16le(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                         #cond);                                            \
            return 1;                                                       \
        }                                                                   \
    } while (0)
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("rec.wav"));

    gvt::MasterRecorder rec;
    QString err;
    CHECK(rec.start(path, &err));
    CHECK(rec.isRecording());
    CHECK(rec.currentPath() == path);

    // Feed 48000 * 2 frames (2 s) of a 440 Hz sine in 256-frame chunks,
    // pacing slightly so the ~2.7 s ring never overflows.
    constexpr int kRate = 48000;
    constexpr int kTotalFrames = kRate * 2;
    constexpr int kChunk = 256;
    std::vector<float> buf(kChunk * 2);
    int fed = 0;
    double phase = 0.0;
    const double inc = 2.0 * M_PI * 440.0 / kRate;
    while (fed < kTotalFrames) {
        const int n = std::min(kChunk, kTotalFrames - fed);
        for (int i = 0; i < n; ++i) {
            const float s = float(0.5 * std::sin(phase));
            phase += inc;
            buf[size_t(i) * 2] = s;
            buf[size_t(i) * 2 + 1] = s;
        }
        rec.feed(buf.data(), n);
        fed += n;
        if ((fed / kChunk) % 32 == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Poll until the worker has drained ~all of it (>= 1.9 s) or timeout.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rec.recordedSec() < 1.9 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(rec.recordedSec() >= 1.9);

    rec.stop();
    CHECK(!rec.isRecording());

    // Re-open and validate the WAV.
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray all = f.readAll();
    CHECK(all.size() >= 44);
    const auto* h = reinterpret_cast<const unsigned char*>(all.constData());
    CHECK(std::memcmp(h + 0, "RIFF", 4) == 0);
    CHECK(std::memcmp(h + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(h + 12, "fmt ", 4) == 0);
    CHECK(u16le(h + 20) == 1);       // PCM
    CHECK(u16le(h + 22) == 2);       // stereo
    CHECK(u32le(h + 24) == 48000);   // sample rate
    CHECK(u16le(h + 34) == 16);      // bits per sample
    CHECK(std::memcmp(h + 36, "data", 4) == 0);

    const uint32_t dataBytes = u32le(h + 40);
    CHECK(u32le(h + 4) == 36 + dataBytes);
    CHECK(qint64(dataBytes) == all.size() - 44);
    // With no drops, data size must match every frame fed: frames * 2ch * 2B.
    CHECK(dataBytes == uint32_t(kTotalFrames) * 2 * 2);

    // Samples must be non-zero (a real sine, not silence).
    const auto* pcm = reinterpret_cast<const int16_t*>(h + 44);
    const size_t nSamples = dataBytes / 2;
    int16_t peak = 0;
    for (size_t i = 0; i < nSamples; ++i)
        peak = std::max<int16_t>(peak, int16_t(std::abs(int(pcm[i]))));
    CHECK(peak > 8000); // 0.5 amplitude ≈ 16384

    std::printf("test_masterrec OK: %.3f s recorded, %u data bytes, peak %d\n",
                rec.recordedSec(), dataBytes, int(peak));
    return 0;
}
