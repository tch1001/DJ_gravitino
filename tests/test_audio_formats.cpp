// Audio-format portability test — the same decoded arrangement remains
// structurally compatible across WAV, FLAC, MP3, and AIFF containers.

#include "analysis/TrackData.h"
#include "../third_party/miniaudio.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

// The progress-aware QFile reader must decode identically to the original
// miniaudio file reader, including encoder padding and sample-rate conversion.
bool matchesOriginalDecoder(const QString& path, const std::vector<float>& pcm)
{
    ma_decoder decoder;
    const auto config = ma_decoder_config_init(ma_format_f32, 2, gvt::kSampleRate);
    if (ma_decoder_init_file(path.toUtf8().constData(), &config, &decoder) != MA_SUCCESS)
        return false;
    std::vector<float> original;
    std::vector<float> chunk(65536 * 2);
    for (;;) {
        ma_uint64 count = 0;
        const auto result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 65536, &count);
        original.insert(original.end(), chunk.data(), chunk.data() + count * 2);
        if (result != MA_SUCCESS || count < 65536) break;
    }
    ma_decoder_uninit(&decoder);
    return original == pcm;
}

void append16(QByteArray& bytes, uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void append32(QByteArray& bytes, uint32_t value)
{
    append16(bytes, static_cast<uint16_t>(value & 0xffff));
    append16(bytes, static_cast<uint16_t>(value >> 16));
}

bool writeSongWav(const QString& path)
{
    constexpr int seconds = 16;
    constexpr int channels = 2;
    constexpr int bits = 16;
    const int frames = seconds * gvt::kSampleRate;
    QByteArray pcm;
    pcm.reserve(frames * channels * 2);
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / gvt::kSampleRate;
        const int phrase = static_cast<int>(t / 2.0) % 4;
        const double frequency = 120.0 + phrase * 81.0;
        const double pulse = 0.2 + 0.8 * std::pow(
            std::max(0.0, std::sin(2.0 * M_PI * 2.0 * t)), 6.0);
        const double sample = pulse * 0.45 *
            (std::sin(2.0 * M_PI * frequency * t) +
             0.25 * std::sin(2.0 * M_PI * frequency * 3.0 * t));
        const int16_t value = static_cast<int16_t>(
            std::clamp(sample, -1.0, 1.0) * 32767.0);
        append16(pcm, static_cast<uint16_t>(value));
        append16(pcm, static_cast<uint16_t>(value));
    }
    QByteArray wav("RIFF", 4);
    append32(wav, static_cast<uint32_t>(36 + pcm.size()));
    wav.append("WAVEfmt ", 8);
    append32(wav, 16);
    append16(wav, 1);
    append16(wav, channels);
    append32(wav, gvt::kSampleRate);
    append32(wav, gvt::kSampleRate * channels * bits / 8);
    append16(wav, channels * bits / 8);
    append16(wav, bits);
    wav.append("data", 4);
    append32(wav, static_cast<uint32_t>(pcm.size()));
    wav.append(pcm);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

bool transcode(const QString& ffmpeg, const QString& source,
               const QString& destination, const QStringList& codecArgs = {})
{
    QProcess process;
    QStringList args {QStringLiteral("-y"), QStringLiteral("-loglevel"),
                      QStringLiteral("error"), QStringLiteral("-i"), source};
    args.append(codecArgs);
    args.append(destination);
    process.start(ffmpeg, args);
    return process.waitForFinished(30000) && process.exitCode() == 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString wav = directory.filePath(QStringLiteral("song.wav"));
    if (!writeSongWav(wav)) return 1;

    QString error;
    const gvt::TrackDataPtr reference = gvt::loadAndAnalyzeTrack(wav, &error);
    if (!reference) {
        std::fprintf(stderr, "FAIL WAV: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (!matchesOriginalDecoder(wav, reference->pcm)) return 1;
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        std::printf("test_audio_formats: WAV passed; ffmpeg unavailable, codecs skipped\n");
        return 0;
    }

    const struct Variant {
        const char* name;
        const char* suffix;
        QStringList args;
        bool checkArrangement = true;
    } variants[] = {
        {"FLAC", "flac", {}},
        {"MP3", "mp3", {QStringLiteral("-b:a"), QStringLiteral("96k")}},
        {"MP3-VBR", "mp3", {QStringLiteral("-q:a"), QStringLiteral("4")}},
        {"MP3-no-length-header", "mp3", {QStringLiteral("-q:a"), QStringLiteral("4"),
            QStringLiteral("-write_xing"), QStringLiteral("0")}, false},
        {"AIFF", "aiff", {QStringLiteral("-c:a"), QStringLiteral("pcm_s16be")}},
        {"gain/silence/resample", "flac",
         {QStringLiteral("-af"), QStringLiteral("adelay=750|750,volume=0.42"),
          QStringLiteral("-ar"), QStringLiteral("44100")}},
    };
    for (const Variant& variant : variants) {
        const QString output = directory.filePath(
            QString::fromLatin1(variant.name).toLower().replace('/', '-') +
            QLatin1Char('.') + QLatin1String(variant.suffix));
        if (!transcode(ffmpeg, wav, output, variant.args)) {
            std::fprintf(stderr, "FAIL transcode %s\n", variant.name);
            return 1;
        }
        const gvt::TrackDataPtr decoded = gvt::loadAndAnalyzeTrack(output, &error);
        if (!decoded) {
            std::fprintf(stderr, "FAIL decode %s: %s\n", variant.name,
                         qUtf8Printable(error));
            return 1;
        }
        if (!matchesOriginalDecoder(output, decoded->pcm)) {
            std::fprintf(stderr, "FAIL PCM changed from original decoder: %s\n", variant.name);
            return 1;
        }
        // Without Xing/LAME metadata both decoders retain encoder padding.
        // This extra case protects decode/progress parity, not a new guarantee
        // about the existing structural matcher across missing gapless metadata.
        if (!variant.checkArrangement) {
            std::printf("%s original-decoder PCM parity passed\n", variant.name);
            continue;
        }
        const double similarity = gvt::structureFingerprintSimilarity(
            reference->structureFingerprint, decoded->structureFingerprint);
        std::printf("%s similarity: %.3f\n", variant.name, similarity);
        if (similarity < 0.88 || decoded->assetSha256 == reference->assetSha256) {
            std::fprintf(stderr, "FAIL compatibility %s: %.3f\n",
                         variant.name, similarity);
            return 1;
        }
    }
    std::printf("test_audio_formats: WAV/FLAC/MP3/AIFF passed\n");
    return 0;
}
