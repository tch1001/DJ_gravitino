// PINNED INTERFACE — library model + transition store.
#pragma once
#include <QAbstractTableModel>
#include "../analysis/TrackData.h"
#include "../transitions/Transition.h"
#include "SongCatalog.h"

namespace gvt {

// Scans MP3/FLAC/WAV/AIFF audio, analyzes in background threads, and caches
// derived analysis separately from the effective grid and authored cue/loop
// state in ~/.gravitino/cache/.
// Columns: Title, Artist, BPM, Key, Duration, Status.
class TrackLibrary : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit TrackLibrary(QObject* parent = nullptr);
    ~TrackLibrary() override;
    void scanFolder(const QString& dir);     // async; default ~/Music
    // Add crowd-request files to this session without resetting existing rows,
    // deck references or in-flight work. New files get interactive priority.
    int addAudioFiles(const QStringList& paths, QStringList* errors=nullptr);
    // Promote an unfinished track (or retry an error). At most four workers,
    // with one slot reserved for interactive load requests. GUI thread only.
    bool prioritizeAnalysis(int row);
    enum AnalysisRole {
        AnalysisProgressRole = Qt::UserRole + 100, // fraction 0..1
        AnalysisActiveRole,                       // false while queued/done
        AnalysisErrorRole                        // full diagnostic string
    };
    int  trackCount() const;
    TrackDataPtr trackAt(int row) const;     // null while still analyzing
    // Ready track or cached identity, for discovery/queuing only. Playback must
    // use trackAt() and revalidate matching after analysis succeeds.
    TrackDataPtr profileAt(int row) const;
    QString pathAt(int row) const;
    QStringList compatibleAssetPaths(int row) const;
    std::vector<CatalogTransitionLink> transitionsForTrack(int row) const;
    void rebuildTransitionGraph(const class TransitionStore& transitions);
    SongCatalog* songCatalog();

    // Persist corrected bpm/firstBeatSec into the existing analysis cache.
    // `corrected` must be a ready track in this library. The shared live
    // TrackData is updated and the BPM model cell emits dataChanged.
    bool persistBeatGrid(const TrackData& corrected, QString* error = nullptr);

    // Persist user-authored hot cues and saved-loop slots without replacing
    // newer analysis/grid metadata held by the library after a rescan.
    bool persistPerformanceMetadata(
        const TrackData& updated, QString* error = nullptr);

    int rowCount(const QModelIndex& = {}) const override;
    int columnCount(const QModelIndex& = {}) const override;
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
signals:
    void trackReady(int row);
    void scanProgress(int analyzed, int total);
    // Emitted after the rebuildable song -> transition reverse index changes.
    // Smart library views use this to refresh transition coverage.
    void transitionGraphChanged();
private:
    void dispatchAnalysis();
};

// Portable .transition files and readable legacy .gvt files.
class TransitionStore : public QObject {
    Q_OBJECT
public:
    explicit TransitionStore(QObject* parent = nullptr);
    ~TransitionStore() override;
    void reload();
    QString directory() const;
    const std::vector<GvtFile>& all() const;
    void setSongCatalog(SongCatalog* catalog);
    bool matchesEndpoint(const GvtFile& file, bool outgoing,
                         const TrackData& track) const;
    // Transitions matching an ordered (from, to) pair, best match first.
    std::vector<const GvtFile*> matching(const TrackData& from, const TrackData& to) const;
    QString save(GvtFile& f, QString* error);  // names file from f.name; returns path
    // Update metadata/cues without changing the managed file name.
    bool update(const GvtFile& f, QString* error);
    // Rename updates metadata and the sanitized portable filename. Renaming a
    // legacy file creates a portable copy and leaves the source untouched.
    QString renameTransition(const GvtFile& f, const QString& newName,
                             QString* error);
    bool deleteTransition(const GvtFile& f, QString* error);
    // Create one portable copy for every legacy source that does not already
    // have a migrated counterpart. Legacy files are never overwritten.
    int convertAllLegacy(QStringList* convertedPaths = nullptr,
                         QStringList* errors = nullptr);
    // Promote recoverable raw saved-loop events in existing portable files to
    // transition-owned loop definitions. Files are updated atomically.
    int upgradePortableSavedLoops(QStringList* upgradedPaths = nullptr,
                                  QStringList* errors = nullptr);
signals:
    // Emitted before all()/matching() pointers are invalidated by a reload.
    void aboutToChange();
    void changed();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
