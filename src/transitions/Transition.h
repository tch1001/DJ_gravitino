// PINNED INTERFACE — see docs/ARCHITECTURE.md + docs/TRANSITION_FORMAT.md.
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
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
    // Portable files can address a semantic transition-owned cue instead of
    // baking a physical pad number into the musical timeline.
    QString   cueId;
    // Saved loops use the same temporary CUSTOM bank as transition cues, but
    // carry their own semantic ID and an IN/OUT range in the document.
    QString   loopId;
    QJsonObject extraYaml;
    QJsonObject inputExtraYaml;
};

struct TransitionFingerprint {
    QString algorithm;
    QString value;
    QJsonObject extraYaml;
};

struct GvtTrackRef {
    QString title, artist, fingerprint;
    double bpm = 0.0, durationSec = 0.0;
    QStringList artists;
    QString versionName;
    QString isrc;
    QString musicBrainzRecording;
    std::map<QString, QString> providerIds;
    double durationBeats = 0.0;
    QString meter = QStringLiteral("4/4");
    double referenceDownbeatSec = 0.0;
    std::vector<TransitionFingerprint> fingerprints;
    QString notes;
    QJsonObject extraYaml;
    QJsonObject identityExtraYaml;
    QJsonObject identifiersExtraYaml;
    QJsonObject providersExtraYaml;
    QJsonObject assumptionsExtraYaml;
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
    QJsonObject extraYaml;
};

// A user-editable label at a beat relative to the transition anchor.  Cues are
// annotations only: they appear in the event preview and on both waveforms,
// and do not change replay behavior.
struct GvtCue {
    double beat = 0.0;
    QString label;
    QJsonObject extraYaml;
};

struct TransitionHotCue {
    QString id;
    Role role = Role::FromDeck;
    double trackBeat = 0.0;
    QString label;
    QString purpose;
    QString color;
    QString pairingGroup;
    QString preferredBank = QStringLiteral("custom");
    int preferredPad = -1; // zero based; -1 lets the host allocate a slot
    QString preferredKey;
    QJsonObject extraYaml;
    QJsonObject inputExtraYaml;
};

struct TransitionSavedLoop {
    QString id;
    Role role = Role::FromDeck;
    double startTrackBeat = 0.0;
    double endTrackBeat = 0.0;
    QString label;
    QString purpose;
    QString color;
    QString pairingGroup;
    QString preferredBank = QStringLiteral("custom");
    int preferredPad = -1; // zero based; -1 lets the host allocate a slot
    QString preferredKey;
    QJsonObject extraYaml;
    QJsonObject inputExtraYaml;
};

// One allocated entry in the isolated, transition-owned CUSTOM bank. A slot
// contains exactly one cue or saved loop; permanent track metadata is never
// consulted or mutated when this bank is active.
struct TransitionPerformanceSlot {
    const TransitionHotCue* cue = nullptr;
    const TransitionSavedLoop* loop = nullptr;
};

enum class TransitionSourceFormat : uint8_t { Unsaved, LegacyGvt, PortableYaml };

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
    QString id, license;
    QStringList tags;
    QStringList requirements;
    QStringList unsupportedRequirements;
    std::map<QString, QString> extraMeta;  // unknown keys, preserved
    // [from] [to] [sync]
    GvtTrackRef from, to;
    double anchorFromBeat = 0.0, anchorToBeat = 0.0, masterBpm = 0.0;
    // Optional authored end of the transition, in master beats from the
    // transition anchor. Files without it retain the legacy last-event plus
    // one-beat completion grace used by TransitionPlayer.
    std::optional<double> endBeat;
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
    // Transition-owned performance cues. These are loaded into the temporary
    // CUSTOM bank and never replace a track's permanent hot cues.
    std::vector<TransitionHotCue> transitionCues;
    // Transition-owned saved loops, also allocated into the temporary CUSTOM
    // bank. Their canonical beat ranges survive changes to track loop slots.
    std::vector<TransitionSavedLoop> transitionLoops;
    // [events], sorted by beat
    std::vector<GvtEvent> events;

    QString filePath; // where it was loaded from / last saved ("" if unsaved)
    TransitionSourceFormat sourceFormat = TransitionSourceFormat::Unsaved;
    // Set on a portable copy migrated from legacy. The copy has a UUID, while
    // this stable source identity suppresses a duplicate UI entry as the
    // original .gvt remains untouched.
    QString legacySourceId;

    // Unknown YAML fields are retained at their containing object so a newer
    // producer can safely round-trip through this version of Gravitino.
    QJsonObject extraYaml;
    QJsonObject metadataExtraYaml;
    QJsonObject endpointsExtraYaml;
    QJsonObject performanceExtraYaml;
    QJsonObject anchorsExtraYaml;
    QJsonObject outgoingAnchorExtraYaml;
    QJsonObject incomingAnchorExtraYaml;
    QJsonObject initialStateExtraYaml;
    QJsonObject mixerInitialExtraYaml;
    QJsonObject extensions;
};

// Parse/serialize. Unknown controls are skipped with a warning appended to
// *warnings (never a hard failure). Returns false + *error only on
// structural problems (bad magic, unreadable file).
bool gvtParse(const QString& text, GvtFile& out, QString* error, QStringList* warnings);
QString gvtSerialize(const GvtFile& f);
bool gvtLoadFile(const QString& path, GvtFile& out, QString* error, QStringList* warnings);
bool gvtSaveFile(const GvtFile& f, const QString& path, QString* error);

// Portable `.transition` YAML. The parser accepts a deliberately small,
// deterministic and safe YAML subset and preserves unknown mapping fields.
bool transitionParse(const QString& text, GvtFile& out, QString* error,
                     QStringList* warnings = nullptr);
QString transitionSerialize(const GvtFile& f);
bool transitionLoadFile(const QString& path, GvtFile& out, QString* error,
                        QStringList* warnings = nullptr);
bool transitionSaveFile(const GvtFile& f, const QString& path, QString* error);

// Format-neutral file entry points. Saving always writes portable YAML;
// loading selects legacy or portable syntax from the extension/content.
bool loadTransitionFile(const QString& path, GvtFile& out, QString* error,
                        QStringList* warnings = nullptr);
bool saveTransitionFile(const GvtFile& f, const QString& path, QString* error);
QString stableLegacyTransitionId(const GvtFile& f);
// Upgrade raw saved-loop pad events when a legacy recording captured exactly
// one loop slot for that endpoint and its initial snapshot contains the loop
// range. Returns the number of semantic loop definitions added.
int migrateSavedLoopsFromInitialState(GvtFile& file,
                                      QStringList* warnings = nullptr);
std::array<const TransitionHotCue*, 8>
transitionCueSlots(const GvtFile& file, Role role);
std::array<TransitionPerformanceSlot, 8>
transitionPerformanceSlots(const GvtFile& file, Role role);
// Portable coordinates are canonical arrangement beats. Legacy coordinates
// remain tied to the original analyzed grid for permanent compatibility.
double transitionBeatAtSec(const GvtFile& file, const struct TrackData& track,
                           double seconds);
double transitionSecAtBeat(const GvtFile& file, const struct TrackData& track,
                           double beat);

// Track matching tiers for offering transitions (see format doc).
enum class MatchQuality {
    None, DurationOnly, TitleArtist, Identifier, Structure, Fingerprint
};
MatchQuality matchTrack(const GvtTrackRef& ref, const struct TrackData& t);
bool transitionTrackMatchReliable(const GvtFile& file,
                                  const GvtTrackRef& ref,
                                  const struct TrackData& track);
constexpr bool isReliableTrackMatch(MatchQuality quality) noexcept
{
    return quality == MatchQuality::Fingerprint ||
           quality == MatchQuality::Structure ||
           quality == MatchQuality::Identifier ||
           quality == MatchQuality::TitleArtist;
}

} // namespace gvt
