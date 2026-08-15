// Play/load history — one entry per track loaded onto a deck.
// Persists as JSON-lines at ~/.gravitino/history.jsonl (one object per line,
// appended on log); the last 500 entries are loaded on construction and
// exposed newest-first.
#pragma once
#include <QDateTime>
#include <QObject>
#include <QString>
#include <vector>
#include "../analysis/TrackData.h"

namespace gvt {

class History : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QDateTime startedAt;   // when the track was loaded
        QString   title;
        QString   artist;
        double    bpm = 0.0;
        QString   key;         // Camelot key ("8A"); empty if unknown
        int       deck = 0;    // 0 = A, 1 = B
    };

    explicit History(QObject* parent = nullptr);

    // Append an entry for `t` loaded onto `deck` (now) and persist it.
    void logLoad(int deck, const TrackData& t);

    // Newest first. entryAdded() fires after a new entry lands at index 0.
    const std::vector<Entry>& entries() const { return entries_; }

    QString filePath() const { return filePath_; }

signals:
    void entryAdded();

private:
    void loadFromDisk();

    QString filePath_;
    std::vector<Entry> entries_; // newest first
};

} // namespace gvt
