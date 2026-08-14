// TrackData — decode + tags + fingerprint + beat analysis.
// Owned by claude-analysis. See docs/ARCHITECTURE.md and
// docs/TRANSITION_FORMAT.md (fingerprint spec).

#include "TrackData.h"
#include "AnalysisInternal.h"

#include "../../third_party/miniaudio.h"  // impl compiled in third_party/miniaudio_impl.c

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gvt {

namespace detail {

bool decodeMp3Stereo48k(const QString& path, std::vector<float>& pcmOut, QString* error)
{
    pcmOut.clear();
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, (ma_uint32)kSampleRate);
    ma_decoder dec;
    if (ma_decoder_init_file(path.toUtf8().constData(), &cfg, &dec) != MA_SUCCESS) {
        if (error) *error = QStringLiteral("cannot open/decode: %1").arg(path);
        return false;
    }
    constexpr ma_uint64 kChunkFrames = 1 << 16;
    std::vector<float> buf((size_t)kChunkFrames * 2);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result res = ma_decoder_read_pcm_frames(&dec, buf.data(), kChunkFrames, &read);
        if (read > 0)
            pcmOut.insert(pcmOut.end(), buf.data(), buf.data() + read * 2);
        if (res != MA_SUCCESS || read < kChunkFrames) break;
    }
    ma_decoder_uninit(&dec);
    if (pcmOut.empty()) {
        if (error) *error = QStringLiteral("no audio frames in: %1").arg(path);
        return false;
    }
    return true;
}

void readTags(const QString& path, QString& title, QString& artist, QString& album)
{
    title.clear(); artist.clear(); album.clear();
    const TagLib::FileRef f(path.toUtf8().constData());
    if (!f.isNull() && f.tag()) {
        const TagLib::Tag* t = f.tag();
        title  = QString::fromUtf8(t->title().toCString(true)).trimmed();
        artist = QString::fromUtf8(t->artist().toCString(true)).trimmed();
        album  = QString::fromUtf8(t->album().toCString(true)).trimmed();
    }
    if (title.isEmpty())
        title = QFileInfo(path).completeBaseName();
}

std::vector<float> computeOverviewPeaks(const std::vector<float>& stereoPcm)
{
    constexpr int kBin = 512; // frames per bin
    const int64_t frames = (int64_t)stereoPcm.size() / 2;
    std::vector<float> peaks;
    peaks.reserve((size_t)(frames / kBin + 1));
    for (int64_t start = 0; start < frames; start += kBin) {
        const int64_t end = std::min<int64_t>(frames, start + kBin);
        float mx = 0.0f;
        for (int64_t i = start; i < end; ++i) {
            const float m = std::fabs(0.5f * (stereoPcm[(size_t)(2 * i)] +
                                              stereoPcm[(size_t)(2 * i + 1)]));
            if (m > mx) mx = m;
        }
        peaks.push_back(std::min(1.0f, mx));
    }
    return peaks;
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

TrackDataPtr loadAndAnalyzeTrack(const QString& mp3Path, QString* error)
{
    auto t = std::make_shared<TrackData>();
    t->filePath = mp3Path;

    if (!detail::decodeMp3Stereo48k(mp3Path, t->pcm, error))
        return nullptr;

    detail::readTags(mp3Path, t->title, t->artist, t->album);
    t->durationSec = (double)t->frameCount() / (double)kSampleRate;
    t->overviewPeaks = detail::computeOverviewPeaks(t->pcm);
    t->fingerprint = computeFingerprint(t->pcm.data(), t->frameCount());

    const std::vector<float> mono = detail::monoMixdown(t->pcm);
    const BeatAnalysis ba = analyzeBeats(mono.data(), (int64_t)mono.size(), kSampleRate);
    if (ba.ok) {
        t->bpm = ba.bpm;
        t->firstBeatSec = ba.firstBeatSec;
    } // else leave bpm = 0 (track still usable, no grid)

    return t;
}

} // namespace gvt
