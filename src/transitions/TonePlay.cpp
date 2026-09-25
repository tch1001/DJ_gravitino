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
    if (!pattern.notes.empty() && pattern.effects && pattern.effects->enabled &&
        (pattern.effects->echo > 0 || pattern.effects->reverb > 0))
        end += pattern.effects->tailBeats;
    if (pattern.sustain && pattern.sustain->enabled)
        end = std::max(end, pattern.sustain->beat + pattern.sustain->duration);
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
    if (p.effects) {
        const auto &fx = *p.effects;
        for (double v : {fx.echo, fx.reverb, fx.echoBeats, fx.tailBeats, fx.sustainDucking})
            if (!std::isfinite(v)) return fail("Tone effect values must be finite.");
        if (fx.echo < 0 || fx.echo > 1 || fx.reverb < 0 || fx.reverb > 1 ||
            fx.sustainDucking < 0 || fx.sustainDucking > 1 ||
            fx.echoBeats < .25 || fx.echoBeats > 4 || fx.tailBeats < .25 || fx.tailBeats > 16)
            return fail("Tone effect levels must be 0–1, echo 1/4–4 beats and tail 1/4–16 beats.");
    }
    if (p.sustain) {
        const auto &s = *p.sustain;
        for (double v : {s.beat, s.duration, s.loopStartBeat, s.loopEndBeat, s.gain,
                         s.crossfadeMs, s.attackMs, s.releaseMs})
            if (!std::isfinite(v)) return fail("Background sustain values must be finite.");
        if (s.beat < 0 || s.beat > 16384 || s.duration < 1.0 / 64 || s.duration > 256 ||
            s.loopStartBeat < p.sourceStartBeat || s.loopEndBeat > p.sourceEndBeat ||
            s.loopEndBeat - s.loopStartBeat < .001 || s.pitch < 24 || s.pitch > 96 ||
            std::abs(s.pitch - p.rootNote) > 24 || s.gain < 0 || s.gain > 1 ||
            s.crossfadeMs < 1 || s.crossfadeMs > 100 || s.attackMs < 1 || s.attackMs > 2000 ||
            s.releaseMs < 1 || s.releaseMs > 2000)
            return fail("Background sustain needs a vowel region inside the slice, duration "
                        "1/64–256 beats, level 0–1, pitch within two octaves of root, "
                        "crossfade 1–100 ms and attack/release 1–2000 ms.");
    }
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
    if (o.contains("effects")) {
        if (!o.value("effects").isObject()) return fail();
        auto e = o.value("effects").toObject();
        if (!e.value("enabled").isBool()) return fail();
        for (const char *k : {"echo", "reverb", "echo_beats", "tail_beats"})
            if (!e.value(k).isDouble()) return fail();
        TonePlayEffects fx;
        fx.enabled = e.take("enabled").toBool();
        fx.echo = e.take("echo").toDouble(); fx.reverb = e.take("reverb").toDouble();
        fx.echoBeats = e.take("echo_beats").toDouble();
        fx.tailBeats = e.take("tail_beats").toDouble();
        if (e.contains("sustain_ducking") && !e.value("sustain_ducking").isDouble()) return fail();
        fx.sustainDucking = e.take("sustain_ducking").toDouble(0);
        fx.extraYaml = e;
        p.effects = fx;
    }
    if (o.contains("sustain")) {
        if (!o.value("sustain").isObject()) return fail();
        auto s = o.value("sustain").toObject();
        if (!s.value("enabled").isBool()) return fail();
        for (const char *k : {"at_beat", "duration_beats", "loop_start_beat", "loop_end_beat",
                              "pitch", "gain", "crossfade_ms", "attack_ms", "release_ms"})
            if (!s.value(k).isDouble()) return fail();
        if (s.value("pitch").toDouble() != s.value("pitch").toInt(-1)) return fail();
        TonePlaySustain hold;
        hold.enabled = s.take("enabled").toBool();
        hold.beat = s.take("at_beat").toDouble();
        hold.duration = s.take("duration_beats").toDouble();
        hold.loopStartBeat = s.take("loop_start_beat").toDouble();
        hold.loopEndBeat = s.take("loop_end_beat").toDouble();
        hold.pitch = s.take("pitch").toInt();
        hold.gain = s.take("gain").toDouble();
        hold.crossfadeMs = s.take("crossfade_ms").toDouble();
        hold.attackMs = s.take("attack_ms").toDouble();
        hold.releaseMs = s.take("release_ms").toDouble();
        hold.extraYaml = s;
        p.sustain = hold;
    }
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
                          "replace_outgoing", "notes", "sustain", "effects"})
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
    if (p.sustain) {
        const auto &s = *p.sustain;
        auto hold = s.extraYaml;
        hold["enabled"] = s.enabled;
        hold["at_beat"] = s.beat;
        hold["duration_beats"] = s.duration;
        hold["loop_start_beat"] = s.loopStartBeat;
        hold["loop_end_beat"] = s.loopEndBeat;
        hold["pitch"] = s.pitch;
        hold["gain"] = s.gain;
        hold["crossfade_ms"] = s.crossfadeMs;
        hold["attack_ms"] = s.attackMs;
        hold["release_ms"] = s.releaseMs;
        o["sustain"] = hold;
    } else o.remove("sustain");
    if (p.effects) {
        const auto &fx = *p.effects;
        auto e = fx.extraYaml;
        e["enabled"] = fx.enabled;
        e["echo"] = fx.echo; e["reverb"] = fx.reverb;
        e["echo_beats"] = fx.echoBeats; e["tail_beats"] = fx.tailBeats;
        e["sustain_ducking"] = fx.sustainDucking;
        o["effects"] = e;
    } else o.remove("effects");
    return o;
}
} // namespace gvt
