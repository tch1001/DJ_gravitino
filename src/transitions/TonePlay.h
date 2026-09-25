// A portable, non-destructive sampler phrase. Source coordinates are canonical
// song beats; piano-roll coordinates are beats on the transition timeline.
#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>
#include <vector>

namespace gvt {
struct TonePlayNote {
    double beat = 0.0, duration = 0.5;
    int pitch = 60;
    double velocity = 0.8;
    QJsonObject extraYaml;
};
// A single continuous background voice, independent of the foreground notes.
// Only the steady vowel region repeats; deck transport and the slice are untouched.
struct TonePlaySustain {
    bool enabled = true;
    double beat = 0, duration = 8;
    double loopStartBeat = 0, loopEndBeat = .25;
    int pitch = 60;
    double gain = .3;
    double crossfadeMs = 15, attackMs = 50, releaseMs = 150;
    QJsonObject extraYaml;
};
// Parallel, foreground-only sends preserve the dry attacks and the held vowel.
struct TonePlayEffects {
    bool enabled = true;
    double echo = .06, reverb = .10, echoBeats = .25, tailBeats = 4;
    double sustainDucking = 0;
    QJsonObject extraYaml;
};
struct TonePlayPattern {
    bool enabled = true;
    double sourceStartBeat = 0.0, sourceEndBeat = 0.5;
    int rootNote = 60;
    double gain = 0.7;
    bool replaceOutgoing = false;
    std::vector<TonePlayNote> notes;
    QJsonObject extraYaml;
    std::optional<TonePlaySustain> sustain;
    std::optional<TonePlayEffects> effects;
};
bool validateTonePlay(const TonePlayPattern &, QString *error);
bool parseTonePlay(const QJsonValue &, std::optional<TonePlayPattern> &, QString *error);
QJsonObject serializeTonePlay(const TonePlayPattern &);
double tonePlayEndBeat(const TonePlayPattern &);
QString tonePlayPitchName(int pitch);
} // namespace gvt
