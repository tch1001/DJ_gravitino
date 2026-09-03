// SongCatalog groups local audio assets into canonical arrangements and
// derives reverse transition edges without ever modifying audio-file tags.
#pragma once

#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace gvt {

struct TrackData;
struct GvtFile;

struct CatalogTransitionLink {
    QString transitionId;
    QString filePath;
    bool outgoing = false;
};

class SongCatalog {
public:
    explicit SongCatalog(const QString& path = {});

    QString path() const;
    QString registerAsset(const TrackData& track, QString* error = nullptr);
    bool confirmBinding(const QString& assetPath, const QString& songId,
                        double canonicalBeatOffset,
                        QString* error = nullptr);
    bool confirmEndpointBinding(const QString& transitionId, bool outgoing,
                                const QString& songId,
                                QString* error = nullptr);
    QString songIdForEndpoint(const QString& transitionId,
                              bool outgoing) const;
    QString songIdForAsset(const QString& assetPath) const;
    QStringList assetsForSong(const QString& songId) const;
    double canonicalBeatOffsetForAsset(const QString& assetPath) const;

    void rebuildTransitionGraph(const std::vector<GvtFile>& transitions);
    std::vector<CatalogTransitionLink> transitionsForSong(
        const QString& songId) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace gvt
