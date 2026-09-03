#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>
#include <cstdint>
#include "TrackData.h"

namespace gvt {

// Asynchronous stem separation via the demucs CLI (htdemucs model), with a
// per-fingerprint WAV cache under ~/.gravitino/stems/<fp-hex>/.
//
// GUI-thread API. requestStems() enqueues a job (FIFO, one demucs process at
// a time). If the cache already holds the four stems, demucs is skipped and
// only the (fast) decode runs. Decode happens on a QtConcurrent worker:
// each 44.1 kHz stereo WAV is decoded+resampled to interleaved stereo int16
// at kSampleRate via miniaudio, padded/truncated to the track's frameCount,
// and delivered back on the GUI thread as a StemSetPtr.
//
// demucs writes {vocals,other,bass,drums}.wav; "other" maps to our "melody".
class StemSeparator : public QObject {
    Q_OBJECT
public:
    explicit StemSeparator(QObject* parent = nullptr);

    // Enqueue separation (or cache decode) for this track. A second request
    // for the same fingerprint while one is queued/in flight just waits for
    // that job's signals. No-op if the track has no fingerprint.
    void requestStems(TrackDataPtr t);

    // True if the four stem WAVs are already cached for this track
    // (decode-only path — cheap, no demucs run).
    bool hasCached(const TrackData& t) const;

signals:
    void progress(QString fingerprint, QString stage);
    void stemsReady(QString fingerprint, gvt::StemSetPtr stems);
    void stemsFailed(QString fingerprint, QString error);

private:
    struct Job {
        QString fingerprint; // full "gvfp1:..." string
        QString fpHex;       // hex part → cache dir name
        QString filePath;    // source audio asset
        int64_t frameCount = 0;
    };

    static QString fingerprintHex(const QString& fingerprint);
    QString cacheDirFor(const QString& fpHex) const;
    static bool cacheComplete(const QString& dir);

    void startNext();
    void runDemucs(const Job& job);
    void onDemucsStderr();
    void onDemucsFinished(int exitCode, QProcess::ExitStatus status);
    void startDecode(const Job& job);
    void finishJob(); // pops current_, starts the next queued job

    QString cacheRoot_;
    QVector<Job> queue_;   // queue_[0] = current job while busy_
    bool busy_ = false;
    QProcess* proc_ = nullptr; // owned; non-null only while demucs runs
    QString tempOutDir_;       // demucs -o target for the current job
    QStringList stderrTail_;   // last lines for error reporting
    int lastPercent_ = -1;
};

} // namespace gvt

Q_DECLARE_METATYPE(gvt::StemSetPtr)
