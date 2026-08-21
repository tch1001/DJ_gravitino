// PINNED INTERFACE — see docs/ARCHITECTURE.md + docs/TRANSITION_FORMAT.md.
#pragma once
#include <QString>
#include <array>
#include <cmath>
#include <limits>
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
    // Optional physical input that produced this state change. Replay still
    // executes `control`; Tutorial uses this hint to teach the actual button
    // (for example CUSTOM pad 3 rather than the resulting generic PLAY state).
    ControlId gestureControl = ControlId::Count;
    int       gesturePadMode = -1; // PerformancePadMode value, when relevant
};

struct GvtTrackRef {
    QString title, artist, fingerprint;
    double bpm = 0.0, durationSec = 0.0;
};

// Snapshot of one deck when recording begins.  The core gain/EQ fields are
// also used by older partial [initial] sections; the transport, loop, FX, and
// stem fields are authoritative when GvtFile::initialComplete is true.
struct GvtInitialState {
    bool captured = false;
    bool playing = false;
    double positionBeat = 0.0;
    double cueBeat = 0.0;
    double tempoRatio = 1.0;
    double fader = 1.0;
    // Legacy files may carry TRIM. New recordings deliberately omit it:
    // gain staging is local to the deck and is not a transition gesture.
    bool trimCaptured = false;
    double trim = 0.5;
    double eqLow = 0.5, eqMid = 0.5, eqHigh = 0.5;
    double filter = 0.5;
    // Optional so legacy complete snapshots do not unexpectedly force the
    // modern per-deck Quantize default during replay.
    bool quantizeCaptured = false;
    bool quantize = true;
    bool loopActive = false;
    double loopStartBeat = 0.0, loopEndBeat = 0.0;
    int fxType = 0;
    bool fxOn = false;
    double fxWet = 0.5, fxBeats = 0.5;
    double stemVocals = 1.0, stemMelody = 1.0;
    double stemBass = 1.0, stemDrums = 1.0;
};

// A user-editable label at a beat relative to the transition anchor.  Cues are
// annotations only: they appear in the event preview and on both waveforms,
// and do not change replay behavior.
struct GvtCue {
    double beat = 0.0;
    QString label;
};

// Missing hot-cue mappings need a non-numeric sentinel because valid beat
// grids can extend below zero when a track has audio before its first downbeat.
inline constexpr double kUnmappedHotCueBeat =
    std::numeric_limits<double>::quiet_NaN();
inline bool hotCueBeatIsMapped(double beat) noexcept
{
    return std::isfinite(beat);
}

struct GvtFile {
    int version = 1;
    // [meta]
    QString name, author, created, description;
    std::map<QString, QString> extraMeta;  // unknown keys, preserved
    // [from] [to] [sync]
    GvtTrackRef from, to;
    double anchorFromBeat = 0.0, anchorToBeat = 0.0, masterBpm = 0.0;
    // New recordings capture a complete role-based pre-transition snapshot.
    // Legacy files may contain only initialFrom's gain/EQ fields.
    bool initialComplete = false;
    GvtInitialState initialFrom;
    GvtInitialState initialTo;
    bool initialMixerCaptured = false;
    double initialCrossfader = 0.0; // role space: 0 = outgoing, 1 = incoming
    // Track-relative beat positions required by recorded hot-cue events.
    // NaN means that the transition does not contain a verified mapping for
    // that role/pad. Finite values, including negative beats, are valid.
    std::array<double, 8> fromHotCueBeats {
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat};
    std::array<double, 8> toHotCueBeats {
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat};
    // [cues], sorted by beat
    std::vector<GvtCue> cues;
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
constexpr bool isReliableTrackMatch(MatchQuality quality) noexcept
{
    return quality == MatchQuality::Fingerprint ||
           quality == MatchQuality::TitleArtist;
}

} // namespace gvt
