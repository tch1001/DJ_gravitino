// Standard PCM WAV plus INFO tags, chapter cues and a portable UTF-8 manifest.
// The private gvtm chunk is data only; ordinary WAV players ignore it safely.
#include "RecordingWav.h"
#include "../analysis/TrackData.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace gvt {
namespace {
void u32(QByteArray& b, quint32 n) { const auto le = qToLittleEndian(n); b.append(reinterpret_cast<const char*>(&le), 4); }
void u16(QByteArray& b, quint16 n) { const auto le = qToLittleEndian(n); b.append(reinterpret_cast<const char*>(&le), 2); }
QByteArray chunk(const char* tag, QByteArray body) {
    QByteArray result(tag, 4); u32(result, quint32(body.size())); result += body;
    if (body.size() & 1) result += '\0';
    return result;
}
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

bool RecordingWav::open(const QString& path, QString* error)
{
    if (QFileInfo::exists(path)) return fail(error, "Output already exists. Choose a new WAV filename.");
    file_.setFileName(path);
    file_.setDirectWriteFallback(false);
    if (!file_.open(QIODevice::WriteOnly)) return fail(error, file_.errorString());
    QByteArray header("RIFF", 4); u32(header, 0); header += "WAVEfmt "; u32(header, 16);
    u16(header, 1); u16(header, 2); u32(header, kSampleRate); u32(header, kSampleRate * 4);
    u16(header, 4); u16(header, 16); header += "data"; u32(header, 0);
    frames_ = 0; peak_ = squares_ = 0;
    return file_.write(header) == header.size() || fail(error, file_.errorString());
}

bool RecordingWav::write(const float* stereo, int count, QString* error)
{
    if (count < 0 || frames_ + count > (0xffffffffLL - 32 * 1024 * 1024) / 4)
        return fail(error, "This set exceeds standard WAV's 4 GiB limit. Export a shorter set.");
    QByteArray bytes; bytes.reserve(count * 4);
    for (int i = 0; i < count * 2; ++i) {
        if (!std::isfinite(stereo[i])) return fail(error, "Non-finite audio sample; export stopped.");
        const double sample = std::clamp(double(stereo[i]), -1.0, 1.0);
        peak_ = std::max(peak_, std::fabs(sample)); squares_ += sample * sample;
        u16(bytes, quint16(qint16(std::lrint(sample * 32767.0))));
    }
    if (file_.write(bytes) != bytes.size()) return fail(error, file_.errorString());
    frames_ += count;
    return true;
}

bool RecordingWav::finish(QJsonObject manifest, QString* error)
{
    manifest.insert("sample_rate", kSampleRate);
    manifest.insert("channels", 2);
    manifest.insert("bits_per_sample", 16);
    manifest.insert("frames", double(frames_));
    manifest.insert("duration_seconds", double(frames_) / kSampleRate);
    manifest.insert("peak", peak_);
    manifest.insert("rms", frames_ ? std::sqrt(squares_ / (frames_ * 2)) : 0.0);
    const auto json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (json.size() > 16 * 1024 * 1024) return fail(error, "Embedded set metadata exceeds 16 MiB.");
    QByteArray info("INFO");
    info += chunk("INAM", manifest.value("title").toString().toUtf8() + '\0');
    info += chunk("ISFT", QByteArray("Gravitino offline set renderer") + '\0');
    info += chunk("ICRD", manifest.value("created_at").toString().toUtf8() + '\0');
    info += chunk("ICMT", QByteArray("Transition recipes, timing and set automation are embedded in the gvtm JSON chunk.") + '\0');
    QByteArray extra = chunk("LIST", info) + chunk("gvtm", json);
    const auto transitions = manifest.value("transitions").toArray();
    QByteArray cues; u32(cues, transitions.size());
    QByteArray labels("adtl");
    for (int i = 0; i < transitions.size(); ++i) {
        const auto entry = transitions[i].toObject();
        u32(cues, i + 1); u32(cues, 0); cues += "data";
        u32(cues, 0); u32(cues, 0); u32(cues, quint32(entry.value("start_frame").toDouble()));
        QByteArray label; u32(label, i + 1);
        label += entry.value("name").toString().toUtf8() + '\0';
        labels += chunk("labl", label);
    }
    extra += chunk("cue ", cues) + chunk("LIST", labels);
    if (file_.write(extra) != extra.size()) return fail(error, file_.errorString());
    const auto size = file_.pos();
    if (size > 0xffffffffLL + 8) return fail(error, "WAV size limit exceeded.");
    QByteArray riffSize; u32(riffSize, quint32(size - 8));
    QByteArray dataSize; u32(dataSize, quint32(frames_ * 4));
    if (!file_.seek(4) || file_.write(riffSize) != 4 || !file_.seek(40) || file_.write(dataSize) != 4)
        return fail(error, file_.errorString());
    if (QFileInfo::exists(file_.fileName())) return fail(error, "Output appeared during export; not overwriting it.");
    return file_.commit() || fail(error, file_.errorString());
}

QJsonObject readRecordingManifest(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { fail(error, file.errorString()); return {}; }
    const auto header = file.read(12);
    if (header.left(4) != "RIFF" || header.mid(8, 4) != "WAVE") {
        fail(error, "Not a RIFF WAVE file."); return {};
    }
    while (file.pos() + 8 <= file.size()) {
        const auto h = file.read(8);
        const quint32 size = qFromLittleEndian<quint32>(h.constData() + 4);
        if (file.pos() + size > file.size()) break;
        if (h.left(4) == "gvtm" && size <= 16 * 1024 * 1024) {
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(file.read(size), &parse);
            if (parse.error == QJsonParseError::NoError && doc.isObject()) return doc.object();
            break;
        }
        if (!file.seek(file.pos() + size + (size & 1))) break;
    }
    fail(error, "No valid embedded Gravitino set manifest."); return {};
}
}
