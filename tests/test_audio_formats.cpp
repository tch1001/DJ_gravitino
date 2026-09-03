// Audio-format portability test — the same decoded arrangement remains
// structurally compatible across WAV, FLAC, MP3, and AIFF containers.

#include "analysis/TrackData.h"

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
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        std::printf("test_audio_formats: WAV passed; ffmpeg unavailable, codecs skipped\n");
        return 0;
    }

    const struct Variant {
        const char* name;
        const char* suffix;
        QStringList args;
    } variants[] = {
        {"FLAC", "flac", {}},
        {"MP3", "mp3", {QStringLiteral("-b:a"), QStringLiteral("96k")}},
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
