// MasterRecorder — records the master output to a 16-bit stereo 48 kHz WAV.
// Audio thread pushes interleaved f32 into a lock-free SPSC ring via feed();
// a worker thread drains it to disk every ~10 ms. Owner: claude-recmaster.
#include "audio/MasterRecorder.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace gvt {

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
// Ring capacity in floats (interleaved samples). Power of two for cheap
// masking: 2^18 floats = 131072 stereo frames ≈ 2.7 s at 48 kHz.
constexpr size_t kRingSize = size_t(1) << 18;
constexpr size_t kRingMask = kRingSize - 1;

void putU32le(unsigned char* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
void putU16le(unsigned char* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// 44-byte canonical WAV header; riffSize/dataSize are patched on stop().
void writeWavHeader(QFile& f, uint32_t dataBytes) {
    unsigned char h[44];
    std::memcpy(h + 0, "RIFF", 4);
    putU32le(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putU32le(h + 16, 16);                       // fmt chunk size
    putU16le(h + 20, 1);                        // PCM
    putU16le(h + 22, kChannels);
    putU32le(h + 24, kSampleRate);
    putU32le(h + 28, kSampleRate * kChannels * (kBitsPerSample / 8)); // byte rate
    putU16le(h + 32, kChannels * (kBitsPerSample / 8));               // block align
    putU16le(h + 34, kBitsPerSample);
    std::memcpy(h + 36, "data", 4);
    putU32le(h + 40, dataBytes);
    f.write(reinterpret_cast<const char*>(h), sizeof h);
}

QString defaultRecordingPath() {
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty())
        music = QDir::homePath() + QStringLiteral("/Music");
    const QString dir = music + QStringLiteral("/Gravitino/Recordings");
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    return dir + QStringLiteral("/gravitino-") + stamp + QStringLiteral(".wav");
}
} // namespace

struct MasterRecorder::Impl {
    // SPSC ring: audio thread is the sole producer (head), worker the sole
    // consumer (tail). Indices are free-running and masked on access.
    std::vector<float> ring = std::vector<float>(kRingSize);
    std::atomic<size_t> head{0}; // next write index (producer)
    std::atomic<size_t> tail{0}; // next read index (consumer)

    std::atomic<bool> active{false};  // gate for feed(); cleared before join
    std::atomic<bool> stopFlag{false}; // tells the worker to finish
    std::atomic<uint64_t> framesWritten{0};
    std::atomic<uint64_t> droppedFrames{0};

    std::thread worker;
    QFile file;
    QString path;
    bool recording = false; // GUI-thread view

    void drainToFile() {
        std::vector<int16_t> pcm;
        pcm.reserve(kRingSize);
        for (;;) {
            const size_t t = tail.load(std::memory_order_relaxed);
            const size_t h = head.load(std::memory_order_acquire);
            size_t avail = h - t;
            if (avail == 0) {
                if (stopFlag.load(std::memory_order_acquire))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            pcm.clear();
            for (size_t i = 0; i < avail; ++i) {
                float s = ring[(t + i) & kRingMask];
                s = std::clamp(s, -1.0f, 1.0f);
                pcm.push_back(int16_t(std::lrintf(s * 32767.0f)));
            }
            tail.store(t + avail, std::memory_order_release);
            file.write(reinterpret_cast<const char*>(pcm.data()),
                       qint64(pcm.size() * sizeof(int16_t)));
            framesWritten.fetch_add(avail / kChannels, std::memory_order_relaxed);
        }
    }
};

MasterRecorder::MasterRecorder(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

MasterRecorder::~MasterRecorder() { stop(); }

bool MasterRecorder::start(const QString& path, QString* error) {
    if (impl_->recording) {
        if (error) *error = QStringLiteral("already recording");
        return false;
    }
    QString p = path.isEmpty() ? defaultRecordingPath() : path;
    const QFileInfo fi(p);
    if (!QDir().mkpath(fi.absolutePath())) {
        if (error) *error = QStringLiteral("cannot create directory %1").arg(fi.absolutePath());
        return false;
    }
    impl_->file.setFileName(p);
    if (!impl_->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(p, impl_->file.errorString());
        return false;
    }
    writeWavHeader(impl_->file, 0); // placeholder sizes, patched in stop()

    impl_->path = p;
    impl_->head.store(0, std::memory_order_relaxed);
    impl_->tail.store(0, std::memory_order_relaxed);
    impl_->framesWritten.store(0, std::memory_order_relaxed);
    impl_->droppedFrames.store(0, std::memory_order_relaxed);
    impl_->stopFlag.store(false, std::memory_order_release);
    impl_->worker = std::thread([this] { impl_->drainToFile(); });
    impl_->recording = true;
    impl_->active.store(true, std::memory_order_release);
    emit recordingChanged(true, p);
    return true;
}

void MasterRecorder::stop() {
    if (!impl_->recording)
        return;
    // Gate the audio thread out first: a concurrent feed() at worst writes
    // into the still-allocated ring (never freed until the destructor).
    impl_->active.store(false, std::memory_order_release);
    impl_->stopFlag.store(true, std::memory_order_release);
    if (impl_->worker.joinable())
        impl_->worker.join();

    const uint64_t dropped = impl_->droppedFrames.load(std::memory_order_relaxed);
    if (dropped > 0)
        qWarning("MasterRecorder: dropped %llu frames (ring overflow)",
                 static_cast<unsigned long long>(dropped));

    // Patch RIFF/data sizes in the header.
    const uint64_t dataBytes64 =
        impl_->framesWritten.load(std::memory_order_relaxed) * kChannels * (kBitsPerSample / 8);
    const uint32_t dataBytes = uint32_t(std::min<uint64_t>(dataBytes64, 0xFFFFFFFFu - 36));
    unsigned char u32[4];
    impl_->file.seek(4);
    putU32le(u32, 36 + dataBytes);
    impl_->file.write(reinterpret_cast<const char*>(u32), 4);
    impl_->file.seek(40);
    putU32le(u32, dataBytes);
    impl_->file.write(reinterpret_cast<const char*>(u32), 4);
    impl_->file.close();

    impl_->recording = false;
    emit recordingChanged(false, impl_->path);
}

bool MasterRecorder::isRecording() const { return impl_->recording; }

QString MasterRecorder::currentPath() const { return impl_->path; }

double MasterRecorder::recordedSec() const {
    return double(impl_->framesWritten.load(std::memory_order_relaxed)) / kSampleRate;
}

void MasterRecorder::feed(const float* interleavedStereo, int frames) {
    if (frames <= 0 || !impl_->active.load(std::memory_order_acquire))
        return;
    const size_t samples = size_t(frames) * kChannels;
    const size_t h = impl_->head.load(std::memory_order_relaxed);
    const size_t t = impl_->tail.load(std::memory_order_acquire);
    if (kRingSize - (h - t) < samples) {
        // Not enough space: drop the whole chunk, count it.
        impl_->droppedFrames.fetch_add(uint64_t(frames), std::memory_order_relaxed);
        return;
    }
    for (size_t i = 0; i < samples; ++i)
        impl_->ring[(h + i) & kRingMask] = interleavedStereo[i];
    impl_->head.store(h + samples, std::memory_order_release);
}

} // namespace gvt
