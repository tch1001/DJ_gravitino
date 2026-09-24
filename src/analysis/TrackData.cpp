// TrackData — decode + tags + fingerprint + beat analysis.
// Owned by claude-analysis. See docs/ARCHITECTURE.md and
// docs/TRANSITION_FORMAT.md (fingerprint spec).

#include "TrackData.h"
#include "AnalysisInternal.h"
#include "KeyAnalyzer.h"

#include "../../third_party/miniaudio.h"  // impl compiled in third_party/miniaudio_impl.c

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

#include <QFileInfo>
#include <QCryptographicHash>
#include <QFile>
#include <QByteArray>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

namespace gvt {

namespace detail {

QString decodedAudioHash(const std::vector<float>& pcm, const WorkProgress& progress)
{
    static_assert(sizeof(float) == 4);
    // MP3 tag/gapless metadata can change how much end-padding the decoder
    // flushes. Ignore only terminal stereo frames of digital silence/subnormal
    // filter residue (below float's minimum normal magnitude). Never trim the
    // front, normal-valued audio, or interior silence: their timing is authored.
    size_t samples = pcm.size();
    while (samples >= 2 &&
           std::abs(pcm[samples - 1]) < std::numeric_limits<float>::min() &&
           std::abs(pcm[samples - 2]) < std::numeric_limits<float>::min()) {
        samples -= 2;
        if (progress && samples % 262144 == 0) progress(0.0);
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("gravitino-pcm-v1/f32le/48000/stereo/no-terminal-digital-padding"));
    constexpr size_t chunk = 262144;
    for (size_t start = 0; start < samples; start += chunk) {
        if (progress) progress(double(start) / samples);
        const size_t count = std::min(chunk, samples - start);
        if constexpr (std::endian::native == std::endian::little) {
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(pcm.data() + start),
                                       qsizetype(count * sizeof(float))));
        } else {
            std::vector<quint32> little(count);
            for (size_t i = 0; i < count; ++i)
                little[i] = qToLittleEndian(std::bit_cast<quint32>(pcm[start + i]));
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(little.data()),
                                       qsizetype(count * sizeof(quint32))));
        }
    }
    if (progress) progress(1.0);
    return QStringLiteral("gvpcm1:") + QString::fromLatin1(hash.result().toHex());
}

QString assetFileHash(const QString& path, const WorkProgress& progress)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray bytes = file.read(1 << 20);
        if (bytes.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(bytes);
        if (progress) progress(double(file.pos()) / std::max<qint64>(1, file.size()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool decodeAudioStereo48k(const QString& path, std::vector<float>& pcmOut,
                          QString* error, const WorkProgress& progress)
{
    pcmOut.clear();
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open/decode: %1").arg(path);
        return false;
    }
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, (ma_uint32)kSampleRate);
    ma_decoder dec;
    const auto read = [](ma_decoder* decoder, void* output, size_t bytes,
                         size_t* bytesRead) -> ma_result {
        const qint64 count = static_cast<QFile*>(decoder->pUserData)->read(
            static_cast<char*>(output), static_cast<qint64>(bytes));
        if (bytesRead) *bytesRead = count > 0 ? static_cast<size_t>(count) : 0;
        return count < 0 ? MA_IO_ERROR : count == 0 ? MA_AT_END : MA_SUCCESS;
    };
    const auto seek = [](ma_decoder* decoder, ma_int64 offset,
                         ma_seek_origin origin) -> ma_result {
        auto* file = static_cast<QFile*>(decoder->pUserData);
        const qint64 base = origin == ma_seek_origin_start ? 0
            : origin == ma_seek_origin_end ? file->size() : file->pos();
        return file->seek(base + offset) ? MA_SUCCESS : MA_BAD_SEEK;
    };
    if (ma_decoder_init(read, seek, &source, &cfg, &dec) != MA_SUCCESS) {
        if (error) *error = QStringLiteral("cannot open/decode: %1").arg(path);
        return false;
    }
    struct Cleanup {
        ma_decoder* decoder;
        ~Cleanup() { ma_decoder_uninit(decoder); }
    } cleanup{&dec};
    // The native MP3 length query can decode the ENTIRE file a second time.
    // Measure actual compressed-file read position instead; buffered read-ahead
    // makes this an approximation, capped until decode really reaches EOF.
    const qint64 length = source.size();
    double completed = 0;
    if (progress) progress(0.0);
    constexpr ma_uint64 kChunkFrames = 1 << 16;
    std::vector<float> buf((size_t)kChunkFrames * 2);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result res = ma_decoder_read_pcm_frames(&dec, buf.data(), kChunkFrames, &read);
        if (read > 0)
            pcmOut.insert(pcmOut.end(), buf.data(), buf.data() + read * 2);
        if (progress) {
            completed = std::max(completed, length > 0
                ? std::clamp(double(source.pos()) / length, 0.0, .99) : 0.0);
            progress(completed);
        }
        if (res != MA_SUCCESS || read < kChunkFrames) break;
    }
    if (pcmOut.empty()) {
        if (error) *error = QStringLiteral("no audio frames in: %1").arg(path);
        return false;
    }
    if (progress) progress(1.0);
    return true;
}

void readTags(const QString& path, QString& title, QString& artist,
              QString& album, QString& isrc, QString& musicBrainzRecording)
{
    title.clear(); artist.clear(); album.clear(); isrc.clear();
    musicBrainzRecording.clear();
    const TagLib::FileRef f(path.toUtf8().constData());
    if (!f.isNull() && f.tag()) {
        const TagLib::Tag* t = f.tag();
        title  = QString::fromUtf8(t->title().toCString(true)).trimmed();
        artist = QString::fromUtf8(t->artist().toCString(true)).trimmed();
        album  = QString::fromUtf8(t->album().toCString(true)).trimmed();
        const TagLib::PropertyMap properties = f.properties();
        const auto property = [&properties](const char* key) {
            const TagLib::StringList values = properties.value(
                TagLib::String(key, TagLib::String::Latin1));
            return values.isEmpty()
                       ? QString()
                       : QString::fromUtf8(
                             values.toString().toCString(true)).trimmed();
        };
        isrc = property("ISRC");
        musicBrainzRecording = property("MUSICBRAINZ_TRACKID");
        if (musicBrainzRecording.isEmpty())
            musicBrainzRecording = property("MUSICBRAINZ_RECORDINGID");
    }
    if (title.isEmpty())
        title = QFileInfo(path).completeBaseName();
}

std::vector<float> computeOverviewPeaks(const std::vector<float>& stereoPcm,
                                      const WorkProgress& progress)
{
    constexpr int kBin = 512; // frames per bin
    const int64_t frames = (int64_t)stereoPcm.size() / 2;
    std::vector<float> peaks;
    peaks.reserve((size_t)(frames / kBin + 1));
    for (int64_t start = 0; start < frames; start += kBin) {
        if (progress && start % 65536 == 0) progress(double(start) / frames);
        const int64_t end = std::min<int64_t>(frames, start + kBin);
        float mx = 0.0f;
        for (int64_t i = start; i < end; ++i) {
            const float m = std::fabs(0.5f * (stereoPcm[(size_t)(2 * i)] +
                                              stereoPcm[(size_t)(2 * i + 1)]));
            if (m > mx) mx = m;
        }
        peaks.push_back(std::min(1.0f, mx));
    }
    if (progress) progress(1.0);
    return peaks;
}

void computeBandOverviews(const std::vector<float>& stereoPcm,
                          std::vector<float>& low,
                          std::vector<float>& mid,
                          std::vector<float>& high, const WorkProgress& progress)
{
    constexpr int kBin = 512; // frames per bin — must match computeOverviewPeaks
    const int64_t frames = (int64_t)stereoPcm.size() / 2;
    const size_t nBins = (size_t)((frames + kBin - 1) / kBin);
    low.assign(nBins, 0.0f);
    mid.assign(nBins, 0.0f);
    high.assign(nBins, 0.0f);
    if (nBins == 0) return;

    // Two independent one-pole lowpasses; bands by difference:
    //   low = lp200, mid = lp2000 - lp200, high = mono - lp2000.
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float aLo = 1.0f - std::exp(-kTwoPi * 200.0f  / (float)kSampleRate);
    const float aHi = 1.0f - std::exp(-kTwoPi * 2000.0f / (float)kSampleRate);
    float lp200 = 0.0f, lp2000 = 0.0f;

    for (int64_t i = 0; i < frames; ++i) {
        if (progress && i % 65536 == 0) progress(double(i) / frames);
        const float m = 0.5f * (stereoPcm[(size_t)(2 * i)] +
                                stereoPcm[(size_t)(2 * i + 1)]);
        lp200  += aLo * (m - lp200);
        lp2000 += aHi * (m - lp2000);
        const size_t b = (size_t)(i / kBin);
        const float lo = std::fabs(lp200);
        const float md = std::fabs(lp2000 - lp200);
        const float hi = std::fabs(m - lp2000);
        if (lo > low[b])  low[b]  = lo;
        if (md > mid[b])  mid[b]  = md;
        if (hi > high[b]) high[b] = hi;
    }

    // Normalize all three bands by the same global max so relative band
    // balance is preserved. Silent track -> gmax ~0 -> vectors stay all-zero.
    float gmax = 0.0f;
    for (const auto* v : {&low, &mid, &high})
        for (float x : *v)
            if (x > gmax) gmax = x;
    if (gmax <= 1e-9f) return;
    const float inv = 1.0f / gmax;
    for (auto* v : {&low, &mid, &high})
        for (float& x : *v)
            x = std::min(1.0f, x * inv);
}

std::vector<float> monoMixdown(const std::vector<float>& stereoPcm)
{
    const int64_t frames = (int64_t)stereoPcm.size() / 2;
    std::vector<float> mono((size_t)frames);
    for (int64_t i = 0; i < frames; ++i)
        mono[(size_t)i] = 0.5f * (stereoPcm[(size_t)(2 * i)] + stereoPcm[(size_t)(2 * i + 1)]);
    return mono;
}

} // namespace detail

QString computeFingerprint(const float* stereoPcm, int64_t frames)
{
    // FNV-1a 64 over the first 30 s of mono PCM decimated to 11025 Hz,
    // quantized to int8. See docs/TRANSITION_FORMAT.md.
    constexpr int    kFpRate = 11025;
    constexpr double kFpSecs = 30.0;
    uint64_t h = 14695981039346656037ULL;
    const int64_t nOut = (int64_t)(kFpSecs * kFpRate);
    for (int64_t j = 0; j < nOut; ++j) {
        const int64_t src = (int64_t)((double)j * (double)kSampleRate / (double)kFpRate);
        if (src >= frames) break;
        float m = 0.5f * (stereoPcm[2 * src] + stereoPcm[2 * src + 1]);
        m = std::clamp(m, -1.0f, 1.0f);
        const int8_t q = (int8_t)std::lrintf(m * 127.0f);
        h ^= (uint8_t)q;
        h *= 1099511628211ULL;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "gvfp1:%016llx", (unsigned long long)h);
    return QString::fromLatin1(buf);
}

QString computeStructureFingerprint(const float* stereoPcm, int64_t frames,
                                    double* audibleDurationSec)
{
    // gvsf2 is a compact sequence of normalized block features. Unlike the
    // older 128-bit gvsf1 SimHash, it retains enough absolute spectral shape
    // to distinguish similarly arranged but different audio while remaining
    // tolerant of gain, lossy encoding and a small leading encoder delay.
    constexpr int kBlockFrames = kSampleRate / 4; // 250 ms
    constexpr int kSlots = 64;
    struct Feature { double rms = 0.0, zcr = 0.0, low = 0.0, high = 0.0; };
    const int blocks = static_cast<int>((frames + kBlockFrames - 1) /
                                        kBlockFrames);
    std::vector<Feature> raw(static_cast<size_t>(std::max(0, blocks)));
    double maxRms = 0.0;
    for (int block = 0; block < blocks; ++block) {
        const int64_t begin = static_cast<int64_t>(block) * kBlockFrames;
        const int64_t end = std::min<int64_t>(frames, begin + kBlockFrames);
        double square = 0.0, lowEnergy = 0.0, highEnergy = 0.0;
        double slow = 0.0;
        float previous = 0.0f;
        int crossings = 0;
        for (int64_t frame = begin; frame < end; ++frame) {
            const float mono = 0.5f * (stereoPcm[2 * frame] +
                                        stereoPcm[2 * frame + 1]);
            slow += 0.02 * (static_cast<double>(mono) - slow);
            const double high = static_cast<double>(mono) - slow;
            square += static_cast<double>(mono) * mono;
            lowEnergy += slow * slow;
            highEnergy += high * high;
            if (frame > begin && ((mono >= 0.0f) != (previous >= 0.0f)))
                ++crossings;
            previous = mono;
        }
        const double count = std::max<int64_t>(1, end - begin);
        Feature& feature = raw[static_cast<size_t>(block)];
        feature.rms = std::sqrt(square / count);
        feature.zcr = crossings / count;
        const double total = lowEnergy + highEnergy + 1e-15;
        feature.low = lowEnergy / total;
        feature.high = highEnergy / total;
        maxRms = std::max(maxRms, feature.rms);
    }

    int first = 0, last = blocks;
    const double audibleThreshold = std::max(1e-5, maxRms * 0.015);
    while (first < last && raw[static_cast<size_t>(first)].rms < audibleThreshold)
        ++first;
    while (last > first && raw[static_cast<size_t>(last - 1)].rms < audibleThreshold)
        --last;
    if (audibleDurationSec)
        *audibleDurationSec = (last - first) *
                              (static_cast<double>(kBlockFrames) / kSampleRate);
    if (last - first < 4)
        return QStringLiteral("gvsf2:") + QString(512, QLatin1Char('0'));

    std::array<Feature, kSlots> sampled {};
    for (int slot = 0; slot < kSlots; ++slot) {
        const double position = first +
            (slot + 0.5) * static_cast<double>(last - first) / kSlots;
        const int index = std::clamp(static_cast<int>(position), first, last - 1);
        sampled[static_cast<size_t>(slot)] = raw[static_cast<size_t>(index)];
    }
    double meanRms = 0.0;
    for (const Feature& feature : sampled) meanRms += feature.rms;
    meanRms = std::max(1e-12, meanRms / kSlots);
    for (Feature& feature : sampled) feature.rms /= meanRms;

    QByteArray descriptor;
    descriptor.reserve(kSlots * 4);
    const auto byte = [](double value) {
        return static_cast<char>(std::clamp(
            static_cast<int>(std::lround(value)), 0, 255));
    };
    for (const Feature& feature : sampled) {
        descriptor.append(byte(feature.rms * 64.0));
        descriptor.append(byte(feature.zcr * 12000.0));
        descriptor.append(byte(feature.low * 255.0));
        descriptor.append(byte(feature.high * 255.0));
    }
    return QStringLiteral("gvsf2:") +
           QString::fromLatin1(descriptor.toHex());
}

double structureFingerprintSimilarity(const QString& a, const QString& b)
{
    if (a.startsWith(QStringLiteral("gvsf2:")) &&
        b.startsWith(QStringLiteral("gvsf2:"))) {
        const QByteArray left = QByteArray::fromHex(a.mid(6).toLatin1());
        const QByteArray right = QByteArray::fromHex(b.mid(6).toLatin1());
        if (left.size() != 256 || right.size() != left.size()) return 0.0;
        double score = 0.0;
        constexpr double tolerances[4] = {48.0, 32.0, 80.0, 80.0};
        for (qsizetype index = 0; index < left.size(); ++index) {
            const int x = static_cast<unsigned char>(left[index]);
            const int y = static_cast<unsigned char>(right[index]);
            const double distance = std::fabs(static_cast<double>(x - y));
            score += 1.0 - std::min(1.0, distance /
                tolerances[static_cast<std::size_t>(index % 4)]);
        }
        return score / left.size();
    }
    if (a.startsWith(QStringLiteral("gvsf1:")) &&
        b.startsWith(QStringLiteral("gvsf1:")) && a.size() == 38 &&
        b.size() == 38) {
        int different = 0;
        for (int offset : {6, 22}) {
            bool okA = false, okB = false;
            const uint64_t wordA = a.mid(offset, 16).toULongLong(&okA, 16);
            const uint64_t wordB = b.mid(offset, 16).toULongLong(&okB, 16);
            if (!okA || !okB) return 0.0;
            different += std::popcount(wordA ^ wordB);
        }
        return 1.0 - static_cast<double>(different) / 128.0;
    }
    return 0.0;
}

TrackDataPtr loadAndAnalyzeTrack(const QString& audioPath, QString* error)
{
    return detail::loadAndAnalyzeWithProgress(audioPath, error, {});
}

TrackDataPtr detail::loadAndAnalyzeWithProgress(
    const QString& audioPath, QString* error, const AnalysisProgress& progress)
{
    const auto phase = [&](double start, double weight, const QString& name) {
        return [&, start, weight, name](double fraction) {
            if (progress) progress(start + weight * fraction, name);
        };
    };
    auto t = std::make_shared<TrackData>();
    t->filePath = audioPath;

    if (!detail::decodeAudioStereo48k(audioPath, t->pcm, error,
                                    phase(0, .35, QStringLiteral("Decoding"))))
        return nullptr;

    detail::readTags(audioPath, t->title, t->artist, t->album,
                     t->isrc, t->musicBrainzRecording);
    t->durationSec = (double)t->frameCount() / (double)kSampleRate;
    t->overviewPeaks = detail::computeOverviewPeaks(t->pcm,
        phase(.35, .05, QStringLiteral("Waveform")));
    detail::computeBandOverviews(t->pcm, t->overviewLow, t->overviewMid, t->overviewHigh,
        phase(.40, .15, QStringLiteral("Waveform")));
    if (progress) progress(.55, QStringLiteral("Fingerprint"));
    t->fingerprint = computeFingerprint(t->pcm.data(), t->frameCount());
    t->structureFingerprint = computeStructureFingerprint(
        t->pcm.data(), t->frameCount(), &t->audibleDurationSec);
    t->decodedAudioSha256 = detail::decodedAudioHash(t->pcm,
        phase(.60, .025, QStringLiteral("Checking audio")));
    t->assetSha256 = detail::assetFileHash(audioPath,
        phase(.625, .025, QStringLiteral("Checking audio")));

    if (progress) progress(.65, QStringLiteral("BPM"));
    const std::vector<float> mono = detail::monoMixdown(t->pcm);
    const BeatAnalysis ba = detail::analyzeBeatsWithProgress(
        mono.data(), (int64_t)mono.size(), kSampleRate,
        phase(.65, .20, QStringLiteral("BPM")));
    if (ba.ok) {
        t->bpm = ba.bpm;
        t->firstBeatSec = ba.firstBeatSec;
        t->analyzedBpm = ba.bpm;
        t->analyzedFirstBeatSec = ba.firstBeatSec;
    } // else leave bpm = 0 (track still usable, no grid)

    const KeyResult key = analyzeKey(t->pcm.data(), t->frameCount(), kSampleRate,
        phase(.85, .14, QStringLiteral("Key")));
    if (key.ok) {
        t->camelotKey = key.camelotKey;
        t->keyName = key.keyName;
    } // else leave both empty (unknown key)

    if (progress) progress(.99, QStringLiteral("Finishing"));
    return t;
}

} // namespace gvt
