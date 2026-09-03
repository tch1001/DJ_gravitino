// SongCatalog persistence and matching. Asset paths/hashes are local facts;
// transition documents contain only portable endpoint evidence.

#include "SongCatalog.h"
#include "../analysis/TrackData.h"
#include "../transitions/Transition.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <map>

namespace gvt {
namespace {

QString defaultCatalogPath()
{
    const QByteArray overridePath = qgetenv("GRAVITINO_CATALOG_PATH");
    if (!overridePath.isEmpty()) return QFile::decodeName(overridePath);
    return QDir::homePath() + QStringLiteral("/.gravitino/catalog.json");
}

QString normalizedPath(const QString& path)
{
    return QFileInfo(path).absoluteFilePath();
}

bool arrangementCompatible(const QJsonObject& asset, const TrackData& track)
{
    const QString stored = asset.value(
        QStringLiteral("structureFingerprint")).toString();
    if (structureFingerprintSimilarity(stored, track.structureFingerprint) < 0.88)
        return false;
    const double storedBpm = asset.value(QStringLiteral("bpm")).toDouble();
    if (storedBpm > 0.0 && track.bpm > 0.0 &&
        std::fabs(storedBpm - track.bpm) > std::max(0.35, storedBpm * 0.01))
        return false;
    const double storedDuration = asset.value(
        QStringLiteral("audibleDurationSec")).toDouble();
    const double trackDuration = track.audibleDurationSec > 0.0
                                     ? track.audibleDurationSec
                                     : track.durationSec;
    return storedDuration <= 0.0 || trackDuration <= 0.0 ||
           std::fabs(storedDuration - trackDuration) <=
               std::max(2.0, storedDuration * 0.015);
}

TrackData trackFromAsset(const QJsonObject& asset)
{
    TrackData track;
    track.filePath = asset.value(QStringLiteral("path")).toString();
    track.title = asset.value(QStringLiteral("title")).toString();
    track.artist = asset.value(QStringLiteral("artist")).toString();
    track.isrc = asset.value(QStringLiteral("isrc")).toString();
    track.musicBrainzRecording = asset.value(
        QStringLiteral("musicBrainzRecording")).toString();
    track.bpm = asset.value(QStringLiteral("bpm")).toDouble();
    track.durationSec = asset.value(QStringLiteral("durationSec")).toDouble();
    track.audibleDurationSec = asset.value(
        QStringLiteral("audibleDurationSec")).toDouble();
    track.fingerprint = asset.value(QStringLiteral("fingerprint")).toString();
    track.structureFingerprint = asset.value(
        QStringLiteral("structureFingerprint")).toString();
    return track;
}

} // namespace

struct SongCatalog::Impl {
    QString path;
    QJsonObject assets; // absolute path -> local asset record
    QJsonObject bindings; // transition-id:role -> canonical local song id
    std::map<QString, std::vector<CatalogTransitionLink>> graph;

    void load()
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) return;
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("version")).toInt() != 1) return;
        assets = root.value(QStringLiteral("assets")).toObject();
        bindings = root.value(QStringLiteral("bindings")).toObject();
    }

    bool save(QString* error) const
    {
        if (error) error->clear();
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            if (error) *error = QStringLiteral("could not create catalog directory");
            return false;
        }
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("assets"), assets);
        root.insert(QStringLiteral("bindings"), bindings);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            if (error) *error = file.errorString();
            return false;
        }
        return true;
    }
};

SongCatalog::SongCatalog(const QString& path)
    : impl_(std::make_shared<Impl>())
{
    impl_->path = path.isEmpty() ? defaultCatalogPath()
                                 : QFileInfo(path).absoluteFilePath();
    impl_->load();
}

QString SongCatalog::path() const { return impl_->path; }

QString SongCatalog::registerAsset(const TrackData& track, QString* error)
{
    const QString path = normalizedPath(track.filePath);
    QJsonObject existing = impl_->assets.value(path).toObject();
    QString songId = existing.value(QStringLiteral("songId")).toString();
    const bool confirmed = existing.value(QStringLiteral("confirmed")).toBool();
    const double existingOffset = existing.value(
        QStringLiteral("canonicalBeatOffset")).toDouble();
    if (songId.isEmpty() || !confirmed) {
        songId.clear();
        for (auto it = impl_->assets.begin(); it != impl_->assets.end(); ++it) {
            const QJsonObject candidate = it.value().toObject();
            if (arrangementCompatible(candidate, track)) {
                songId = candidate.value(QStringLiteral("songId")).toString();
                if (!songId.isEmpty()) break;
            }
        }
    }
    if (songId.isEmpty())
        songId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QJsonObject asset;
    asset.insert(QStringLiteral("path"), path);
    asset.insert(QStringLiteral("songId"), songId);
    asset.insert(QStringLiteral("confirmed"), confirmed);
    asset.insert(QStringLiteral("canonicalBeatOffset"), existingOffset);
    asset.insert(QStringLiteral("title"), track.title);
    asset.insert(QStringLiteral("artist"), track.artist);
    asset.insert(QStringLiteral("isrc"), track.isrc);
    asset.insert(QStringLiteral("musicBrainzRecording"),
                 track.musicBrainzRecording);
    asset.insert(QStringLiteral("bpm"), track.bpm);
    asset.insert(QStringLiteral("firstBeatSec"), track.firstBeatSec);
    asset.insert(QStringLiteral("durationSec"), track.durationSec);
    asset.insert(QStringLiteral("audibleDurationSec"),
                 track.audibleDurationSec);
    asset.insert(QStringLiteral("fingerprint"), track.fingerprint);
    asset.insert(QStringLiteral("structureFingerprint"),
                 track.structureFingerprint);
    asset.insert(QStringLiteral("assetSha256"), track.assetSha256);
    impl_->assets.insert(path, asset);
    if (!impl_->save(error)) return {};
    return songId;
}

bool SongCatalog::confirmBinding(const QString& assetPath,
                                 const QString& songId,
                                 double canonicalBeatOffset, QString* error)
{
    const QString path = normalizedPath(assetPath);
    if (songId.trimmed().isEmpty() || !std::isfinite(canonicalBeatOffset)) {
        if (error) *error = QStringLiteral("invalid song binding");
        return false;
    }
    QJsonObject asset = impl_->assets.value(path).toObject();
    if (asset.isEmpty()) {
        if (error) *error = QStringLiteral("asset is not registered");
        return false;
    }
    asset.insert(QStringLiteral("songId"), songId);
    asset.insert(QStringLiteral("confirmed"), true);
    asset.insert(QStringLiteral("canonicalBeatOffset"), canonicalBeatOffset);
    impl_->assets.insert(path, asset);
    return impl_->save(error);
}

bool SongCatalog::confirmEndpointBinding(const QString& transitionId,
                                         bool outgoing,
                                         const QString& songId,
                                         QString* error)
{
    if (transitionId.trimmed().isEmpty() || songId.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("invalid transition endpoint binding");
        return false;
    }
    const QString key = transitionId +
        (outgoing ? QStringLiteral(":outgoing") : QStringLiteral(":incoming"));
    impl_->bindings.insert(key, songId);
    return impl_->save(error);
}

QString SongCatalog::songIdForEndpoint(const QString& transitionId,
                                       bool outgoing) const
{
    const QString key = transitionId +
        (outgoing ? QStringLiteral(":outgoing") : QStringLiteral(":incoming"));
    return impl_->bindings.value(key).toString();
}

QString SongCatalog::songIdForAsset(const QString& assetPath) const
{
    return impl_->assets.value(normalizedPath(assetPath)).toObject()
        .value(QStringLiteral("songId")).toString();
}

QStringList SongCatalog::assetsForSong(const QString& songId) const
{
    QStringList paths;
    for (auto it = impl_->assets.begin(); it != impl_->assets.end(); ++it)
        if (it.value().toObject().value(QStringLiteral("songId")).toString() == songId &&
            QFileInfo::exists(it.key()))
            paths.append(it.key());
    paths.sort();
    return paths;
}

double SongCatalog::canonicalBeatOffsetForAsset(const QString& assetPath) const
{
    return impl_->assets.value(normalizedPath(assetPath)).toObject()
        .value(QStringLiteral("canonicalBeatOffset")).toDouble();
}

void SongCatalog::rebuildTransitionGraph(
    const std::vector<GvtFile>& transitions)
{
    impl_->graph.clear();
    for (const GvtFile& transition : transitions) {
        for (const auto& endpoint :
             {std::pair<const GvtTrackRef*, bool>{&transition.from, true},
              std::pair<const GvtTrackRef*, bool>{&transition.to, false}}) {
            QString matchedSong = songIdForEndpoint(
                transition.id, endpoint.second);
            if (matchedSong.isEmpty() && !transition.legacySourceId.isEmpty())
                matchedSong = songIdForEndpoint(
                    transition.legacySourceId, endpoint.second);
            MatchQuality best = MatchQuality::None;
            for (auto it = impl_->assets.begin(); matchedSong.isEmpty() &&
                 it != impl_->assets.end(); ++it) {
                const QJsonObject asset = it.value().toObject();
                const TrackData track = trackFromAsset(asset);
                const MatchQuality quality = matchTrack(*endpoint.first, track);
                if (quality > best &&
                    transitionTrackMatchReliable(transition, *endpoint.first,
                                                 track)) {
                    best = quality;
                    matchedSong = asset.value(QStringLiteral("songId")).toString();
                }
            }
            if (matchedSong.isEmpty()) continue;
            auto& links = impl_->graph[matchedSong];
            const CatalogTransitionLink link {
                transition.id, transition.filePath, endpoint.second};
            const bool duplicate = std::any_of(
                links.begin(), links.end(), [&link](const auto& existing) {
                    return existing.transitionId == link.transitionId &&
                           existing.outgoing == link.outgoing;
                });
            if (!duplicate) links.push_back(link);
        }
    }
}

std::vector<CatalogTransitionLink> SongCatalog::transitionsForSong(
    const QString& songId) const
{
    const auto found = impl_->graph.find(songId);
    return found == impl_->graph.end() ? std::vector<CatalogTransitionLink>{}
                                      : found->second;
}

} // namespace gvt
