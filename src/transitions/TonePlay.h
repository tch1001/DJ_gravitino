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
struct TonePlayPattern {
    bool enabled = true;
    double sourceStartBeat = 0.0, sourceEndBeat = 0.5;
    int rootNote = 60;
    double gain = 0.7;
    bool replaceOutgoing = false;
    std::vector<TonePlayNote> notes;
    QJsonObject extraYaml;
};
bool validateTonePlay(const TonePlayPattern &, QString *error);
bool parseTonePlay(const QJsonValue &, std::optional<TonePlayPattern> &, QString *error);
QJsonObject serializeTonePlay(const TonePlayPattern &);
double tonePlayEndBeat(const TonePlayPattern &);
QString tonePlayPitchName(int pitch);
} // namespace gvt
