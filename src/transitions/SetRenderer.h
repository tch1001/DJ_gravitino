// Plan and render a whole DJ set through the existing private two-deck engine.
// Inputs are snapshots; neither the catalog, source audio nor recipes are saved.
#pragma once
#include "Transition.h"
#include "../analysis/TrackData.h"
#include <QJsonObject>
#include <array>
#include <functional>

namespace gvt {
struct SetRenderRequest {
    QString title = QStringLiteral("Gravitino set");
    std::vector<GvtFile> transitions;
    std::vector<TrackDataPtr> assets; // metadata only: PCM decoded two decks at a time
    QStringList assetPaths; // optional explicit N+1 choices
    QString catalogPath;
    QString stemsRoot;
    bool keyLock = true;
};
struct SetRenderPlan {
    std::vector<TrackDataPtr> tracks; // N+1 metadata snapshots in playback order
    QStringList errors, warnings;
    double estimatedSeconds = 0;
    bool valid() const { return errors.isEmpty() && tracks.size() > 1; }
};
struct SetRenderResult {
    bool completed = false, cancelled = false;
    QString error, outputPath;
    QJsonObject manifest;
};
using SetRenderProgress = std::function<void(double, const QString&)>;
using SetRenderCancel = std::function<bool()>;

std::vector<TrackDataPtr> readSetCatalog(const QString& path, QString* error);
TrackDataPtr setAssetProfile(const TrackData& source);
std::vector<GvtFile> transitionsForSetSongs(const QStringList& songs,
    const std::vector<GvtFile>& files, QStringList* errors);
SetRenderPlan planTransitionSet(const SetRenderRequest& request);
bool setSongNeedsStems(const SetRenderRequest& request, size_t songIndex);
std::vector<TrackDataPtr> setAssetCandidates(const SetRenderRequest& request, size_t songIndex);
SetRenderResult renderTransitionSet(const SetRenderRequest& request, const QString& output,
    SetRenderProgress progress = {}, SetRenderCancel cancelled = {});
// A linear-in-time BPM ramp covering exactly this many beats.
double setTempoRampSeconds(double beats, double fromBpm, double toBpm);
double setTempoRampBpm(double elapsed, double duration, double fromBpm, double toBpm);
// LOW, MID, HIGH knob positions, derived only for gaps between authored edges.
using SetEqValues = std::array<double, 3>;
SetEqValues setEqRampValues(double elapsed, double duration,
    const SetEqValues& from, const SetEqValues& to);
QJsonObject setRequestJson(const SetRenderRequest& request);
bool readSetRequest(const QString& path, SetRenderRequest& request, QString* error);
}
