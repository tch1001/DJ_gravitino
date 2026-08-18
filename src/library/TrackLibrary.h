// PINNED INTERFACE — library model + transition store.
#pragma once
#include <QAbstractTableModel>
#include "../analysis/TrackData.h"
#include "../transitions/Transition.h"

namespace gvt {

// Scans a folder for .mp3, analyzes in background threads, caches analysis
// (bpm/grid/fingerprint/hotcues) as JSON in ~/.gravitino/cache/.
// Columns: Title, Artist, BPM, Key, Duration, Status.
class TrackLibrary : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit TrackLibrary(QObject* parent = nullptr);
    void scanFolder(const QString& dir);     // async; default ~/Music
    int  trackCount() const;
    TrackDataPtr trackAt(int row) const;     // null while still analyzing
    QString pathAt(int row) const;

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
};

// All .gvt files in ~/Music/Gravitino/Transitions/.
class TransitionStore : public QObject {
    Q_OBJECT
public:
    explicit TransitionStore(QObject* parent = nullptr);
    ~TransitionStore() override;
    void reload();
    QString directory() const;
    const std::vector<GvtFile>& all() const;
    // Transitions matching an ordered (from, to) pair, best match first.
    std::vector<const GvtFile*> matching(const TrackData& from, const TrackData& to) const;
    QString save(GvtFile& f, QString* error);  // names file from f.name; returns path
    // Update metadata/cues without changing the managed file name.
    bool update(const GvtFile& f, QString* error);
    // Rename updates both [meta].name and the sanitized .gvt filename.
    QString renameTransition(const GvtFile& f, const QString& newName,
                             QString* error);
    bool deleteTransition(const GvtFile& f, QString* error);
signals:
    void changed();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
