// PINNED INTERFACE — see docs/ARCHITECTURE.md + docs/TRANSITION_FORMAT.md.
#pragma once
#include <QString>
#include <map>
#include <vector>
#include "../control/ControlBus.h"

namespace gvt {

enum class Curve : uint8_t { Step, Linear, SCurve };

// Targets in a .gvt file are roles, not physical decks.
enum class Role : uint8_t { FromDeck, ToDeck, Mixer };

struct GvtEvent {
    double    beat = 0.0;      // beats since transition anchor
    Role      role = Role::Mixer;
    ControlId control = ControlId::Crossfader;
    double    value = 1.0;     // triggers: ignored
    Curve     curve = Curve::Step;
};

struct GvtTrackRef {
    QString title, artist, fingerprint;
    double bpm = 0.0, durationSec = 0.0;
};

struct GvtFile {
    int version = 1;
    // [meta]
    QString name, author, created, description;
    std::map<QString, QString> extraMeta;  // unknown keys, preserved
    // [from] [to] [sync]
    GvtTrackRef from, to;
    double anchorFromBeat = 0.0, anchorToBeat = 0.0, masterBpm = 0.0;
    // [events], sorted by beat
    std::vector<GvtEvent> events;

    QString filePath; // where it was loaded from / last saved ("" if unsaved)
};

// Parse/serialize. Unknown controls are skipped with a warning appended to
// *warnings (never a hard failure). Returns false + *error only on
// structural problems (bad magic, unreadable file).
bool gvtParse(const QString& text, GvtFile& out, QString* error, QStringList* warnings);
QString gvtSerialize(const GvtFile& f);
bool gvtLoadFile(const QString& path, GvtFile& out, QString* error, QStringList* warnings);
bool gvtSaveFile(const GvtFile& f, const QString& path, QString* error);

// Track matching tiers for offering transitions (see format doc).
enum class MatchQuality { None, DurationOnly, TitleArtist, Fingerprint };
MatchQuality matchTrack(const GvtTrackRef& ref, const struct TrackData& t);

} // namespace gvt
