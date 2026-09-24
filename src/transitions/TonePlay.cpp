// Validate sampler documents before any audio allocation or piano-roll editing.
#include "TonePlay.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace gvt {
double tonePlayEndBeat(const TonePlayPattern &pattern) {
    double end = 0;
    for (const auto &note : pattern.notes)
        end = std::max(end, note.beat + note.duration);
    return end;
}
QString tonePlayPitchName(int pitch) {
    const char *names[] = {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
    pitch = std::clamp(pitch, 0, 127);
    return QString::fromUtf8(names[pitch % 12]) + QString::number(pitch / 12 - 1);
}
bool validateTonePlay(const TonePlayPattern &p, QString *error) {
    const auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (!std::isfinite(p.sourceStartBeat) || !std::isfinite(p.sourceEndBeat) ||
        std::abs(p.sourceStartBeat) > 1000000 || p.sourceEndBeat <= p.sourceStartBeat ||
        p.sourceEndBeat - p.sourceStartBeat > 32)
        return fail("Tone play needs an outgoing snippet with a positive length of at most 32 "
                    "beats (8 seconds of audio).");
    if (p.rootNote < 24 || p.rootNote > 96 || !std::isfinite(p.gain) || p.gain < 0 || p.gain > 1)
        return fail("Tone play root must be C1–C7 and gain must be 0–1.");
    if (p.notes.size() > 512)
        return fail("Tone play supports at most 512 notes.");
    std::vector<std::pair<double, int>> edges;
    for (const auto &n : p.notes) {
        if (!std::isfinite(n.beat) || !std::isfinite(n.duration) || !std::isfinite(n.velocity) ||
            n.beat < 0 || n.beat > 16384 || n.duration < 1.0 / 64 || n.duration > 64 ||
            n.pitch < 24 || n.pitch > 96 || std::abs(n.pitch - p.rootNote) > 24 || n.velocity < 0 ||
            n.velocity > 1)
            return fail("Tone notes need finite timing, duration 1/64–64 beats, velocity 0–1, and "
                        "pitch within two octaves of the root.");
        edges.emplace_back(n.beat, 1);
        edges.emplace_back(n.beat + n.duration, -1);
    }
    std::sort(edges.begin(), edges.end());
    int voices = 0;
    for (const auto &e : edges)
        if ((voices += e.second) > 16)
            return fail("Tone play supports at most 16 simultaneous notes.");
    return true;
}
bool parseTonePlay(const QJsonValue &value, std::optional<TonePlayPattern> &result,
                   QString *error) {
    result.reset();
    if (value.isUndefined())
        return true;
    const auto fail = [&] {
        if (error)
            *error = "performance.tone_play has invalid or missing fields";
        return false;
    };
    if (!value.isObject())
        return fail();
    auto o = value.toObject();
    for (const char *k : {"source_start_beat", "source_end_beat", "root_note", "gain"})
        if (!o.value(k).isDouble())
            return fail();
    for (const char *k : {"enabled", "replace_outgoing"})
        if (!o.value(k).isBool())
            return fail();
    if (!o.value("notes").isArray() ||
        o.value("root_note").toDouble() != o.value("root_note").toInt(-1))
        return fail();
    TonePlayPattern p;
    p.enabled = o.value("enabled").toBool();
    p.sourceStartBeat = o.value("source_start_beat").toDouble();
    p.sourceEndBeat = o.value("source_end_beat").toDouble();
    p.rootNote = o.value("root_note").toInt();
    p.gain = o.value("gain").toDouble();
    p.replaceOutgoing = o.value("replace_outgoing").toBool();
    const auto notes = o.value("notes").toArray();
    if (notes.size() > 512)
        return fail();
    for (const auto &v : notes) {
        if (!v.isObject())
            return fail();
        auto n = v.toObject();
        for (const char *k : {"at_beat", "duration_beats", "pitch", "velocity"})
            if (!n.value(k).isDouble())
                return fail();
        if (n.value("pitch").toDouble() != n.value("pitch").toInt(-1))
            return fail();
        TonePlayNote note{n.value("at_beat").toDouble(), n.value("duration_beats").toDouble(),
                          n.value("pitch").toInt(), n.value("velocity").toDouble()};
        for (const char *k : {"at_beat", "duration_beats", "pitch", "velocity"})
            n.remove(k);
        note.extraYaml = n;
        p.notes.push_back(note);
    }
    for (const char *k : {"enabled", "source_start_beat", "source_end_beat", "root_note", "gain",
                          "replace_outgoing", "notes"})
        o.remove(k);
    p.extraYaml = o;
    if (!validateTonePlay(p, error))
        return false;
    result = std::move(p);
    return true;
}
QJsonObject serializeTonePlay(const TonePlayPattern &p) {
    auto o = p.extraYaml;
    o["enabled"] = p.enabled;
    o["source_start_beat"] = p.sourceStartBeat;
    o["source_end_beat"] = p.sourceEndBeat;
    o["root_note"] = p.rootNote;
    o["gain"] = p.gain;
    o["replace_outgoing"] = p.replaceOutgoing;
    QJsonArray notes;
    for (const auto &n : p.notes) {
        auto v = n.extraYaml;
        v["at_beat"] = n.beat;
        v["duration_beats"] = n.duration;
        v["pitch"] = n.pitch;
        v["velocity"] = n.velocity;
        notes.append(v);
    }
    o["notes"] = notes;
    return o;
}
} // namespace gvt
