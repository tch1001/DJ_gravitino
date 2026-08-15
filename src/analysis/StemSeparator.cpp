#include "StemSeparator.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

#include "../../third_party/miniaudio.h"

namespace gvt {

static const char* kDemucsBinary = "/Users/fish/.local/bin/demucs";
// Cached file names keep the demucs originals; "other" is our "melody".
static const char* kStemFiles[4] = {"vocals.wav", "other.wav", "bass.wav",
                                    "drums.wav"};

StemSeparator::StemSeparator(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<StemSetPtr>();
    cacheRoot_ = QDir::homePath() + QStringLiteral("/.gravitino/stems");
    QDir().mkpath(cacheRoot_);
}

QString StemSeparator::fingerprintHex(const QString& fingerprint)
{
    const int colon = fingerprint.indexOf(QLatin1Char(':'));
    return colon >= 0 ? fingerprint.mid(colon + 1) : fingerprint;
}

QString StemSeparator::cacheDirFor(const QString& fpHex) const
{
    return cacheRoot_ + QLatin1Char('/') + fpHex;
}

bool StemSeparator::cacheComplete(const QString& dir)
{
    for (const char* f : kStemFiles)
        if (!QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(f)))
            return false;
    return true;
}

bool StemSeparator::hasCached(const TrackData& t) const
{
    if (t.fingerprint.isEmpty()) return false;
    return cacheComplete(cacheDirFor(fingerprintHex(t.fingerprint)));
}

void StemSeparator::requestStems(TrackDataPtr t)
{
    if (!t || t->fingerprint.isEmpty() || t->frameCount() <= 0) return;
    const QString fpHex = fingerprintHex(t->fingerprint);
    for (const Job& j : queue_)
        if (j.fpHex == fpHex) return; // already queued / in flight — wait
    queue_.push_back(
        Job{t->fingerprint, fpHex, t->filePath, t->frameCount()});
    if (!busy_) startNext();
}

void StemSeparator::startNext()
{
    if (busy_ || queue_.isEmpty()) return;
    busy_ = true;
    const Job job = queue_.front();
    if (cacheComplete(cacheDirFor(job.fpHex))) {
        emit progress(job.fingerprint, tr("decoding cached stems…"));
        startDecode(job);
    } else {
        runDemucs(job);
    }
}

void StemSeparator::finishJob()
{
    if (!queue_.isEmpty()) queue_.removeFirst();
    busy_ = false;
    startNext();
}

// ---------------------------------------------------------------- demucs run

void StemSeparator::runDemucs(const Job& job)
{
    if (!QFileInfo::exists(QLatin1String(kDemucsBinary))) {
        emit stemsFailed(job.fingerprint,
                         tr("demucs not found at %1")
                             .arg(QLatin1String(kDemucsBinary)));
        finishJob();
        return;
    }

    tempOutDir_ = cacheRoot_ + QStringLiteral("/tmp-") + job.fpHex;
    QDir(tempOutDir_).removeRecursively(); // stale partial output
    QDir().mkpath(tempOutDir_);
    stderrTail_.clear();
    lastPercent_ = -1;

    proc_ = new QProcess(this);
    proc_->setProgram(QLatin1String(kDemucsBinary));
    proc_->setArguments({QStringLiteral("-n"), QStringLiteral("htdemucs"),
                         QStringLiteral("-d"), QStringLiteral("mps"),
                         QStringLiteral("-o"), tempOutDir_, job.filePath});
    connect(proc_, &QProcess::readyReadStandardError, this,
            &StemSeparator::onDemucsStderr);
    connect(proc_, &QProcess::finished, this,
            &StemSeparator::onDemucsFinished);
    connect(proc_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError err) {
                if (err != QProcess::FailedToStart || queue_.isEmpty())
                    return; // crashes also reach finished(); only handle
                            // failed-to-start, which never does
                const Job job = queue_.front();
                emit stemsFailed(job.fingerprint,
                                 tr("failed to start demucs: %1")
                                     .arg(proc_->errorString()));
                proc_->deleteLater();
                proc_ = nullptr;
                finishJob();
            });
    emit progress(job.fingerprint, tr("separating… starting demucs"));
    proc_->start(); // no timeout: htdemucs takes minutes per track
}

void StemSeparator::onDemucsStderr()
{
    if (!proc_ || queue_.isEmpty()) return;
    const QString chunk = QString::fromUtf8(proc_->readAllStandardError());
    // demucs redraws a tqdm progress bar with \r; split on both.
    static const QRegularExpression lineSep(QStringLiteral("[\r\n]"));
    const QStringList lines = chunk.split(lineSep, Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        stderrTail_.append(line);
        while (stderrTail_.size() > 12) stderrTail_.removeFirst();
    }
    // Forward the latest "NN%" as a progress stage.
    static const QRegularExpression pct(QStringLiteral("(\\d{1,3})%"));
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QRegularExpressionMatch m = pct.match(*it);
        if (!m.hasMatch()) continue;
        const int percent = m.captured(1).toInt();
        if (percent != lastPercent_) {
            lastPercent_ = percent;
            emit progress(queue_.front().fingerprint,
                          tr("separating… %1%").arg(percent));
        }
        break;
    }
}

void StemSeparator::onDemucsFinished(int exitCode,
                                     QProcess::ExitStatus status)
{
    if (queue_.isEmpty()) return;
    const Job job = queue_.front();
    proc_->deleteLater();
    QProcess* proc = proc_;
    proc_ = nullptr;

    if (status != QProcess::NormalExit || exitCode != 0) {
        QString tail = stderrTail_.join(QLatin1Char('\n'));
        if (tail.isEmpty())
            tail = QString::fromUtf8(proc->readAllStandardError()).right(400);
        emit stemsFailed(job.fingerprint,
                         tr("demucs exited with code %1: %2")
                             .arg(exitCode)
                             .arg(tail.right(400)));
        QDir(tempOutDir_).removeRecursively();
        finishJob();
        return;
    }

    // demucs wrote <tempOut>/htdemucs/<input-stem>/{vocals,other,bass,drums}
    // .wav — move the four into the cache dir, then drop the temp tree.
    const QString modelDir =
        tempOutDir_ + QStringLiteral("/htdemucs/") +
        QFileInfo(job.filePath).completeBaseName();
    const QString cacheDir = cacheDirFor(job.fpHex);
    QDir().mkpath(cacheDir);
    bool moved = true;
    for (const char* f : kStemFiles) {
        const QString src = modelDir + QLatin1Char('/') + QLatin1String(f);
        const QString dst = cacheDir + QLatin1Char('/') + QLatin1String(f);
        QFile::remove(dst);
        if (!QFile::rename(src, dst)) {
            moved = false;
            emit stemsFailed(job.fingerprint,
                             tr("demucs output missing: %1").arg(src));
            break;
        }
    }
    QDir(tempOutDir_).removeRecursively();
    if (!moved) {
        QDir(cacheDir).removeRecursively(); // don't leave a partial cache
        finishJob();
        return;
    }
    emit progress(job.fingerprint, tr("decoding stems…"));
    startDecode(job);
}

// -------------------------------------------------------------------- decode

// Decode one WAV to interleaved stereo int16 at kSampleRate (miniaudio
// resamples 44.1k -> 48k during decode), padded/truncated to frameCount.
static bool decodeStemWav(const QString& path, int64_t frameCount,
                          std::vector<int16_t>& out, QString* error)
{
    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_s16, 2, (ma_uint32)kSampleRate);
    ma_decoder dec;
    if (ma_decoder_init_file(path.toUtf8().constData(), &cfg, &dec) !=
        MA_SUCCESS) {
        *error = QStringLiteral("cannot decode %1").arg(path);
        return false;
    }
    out.assign((size_t)frameCount * 2, 0); // zero padding beyond the wav
    ma_uint64 read = 0;
    const ma_result r = ma_decoder_read_pcm_frames(
        &dec, out.data(), (ma_uint64)frameCount, &read); // truncates extra
    ma_decoder_uninit(&dec);
    if (r != MA_SUCCESS && r != MA_AT_END) {
        *error = QStringLiteral("decode error in %1").arg(path);
        return false;
    }
    return true;
}

void StemSeparator::startDecode(const Job& job)
{
    const QString dir = cacheDirFor(job.fpHex);
    const QString fingerprint = job.fingerprint;
    const int64_t frames = job.frameCount;
    (void)QtConcurrent::run([this, dir, fingerprint, frames] {
        auto stems = std::make_shared<StemSet>();
        std::vector<int16_t>* dst[4] = {&stems->vocals, &stems->melody,
                                        &stems->bass, &stems->drums};
        QString error;
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i)
            ok = decodeStemWav(
                dir + QLatin1Char('/') + QLatin1String(kStemFiles[i]),
                frames, *dst[i], &error);
        // Deliver on the GUI thread; then advance the queue.
        QMetaObject::invokeMethod(
            this,
            [this, fingerprint, stems, ok, error] {
                if (ok)
                    emit stemsReady(fingerprint, stems);
                else
                    emit stemsFailed(fingerprint, error);
                finishJob();
            },
            Qt::QueuedConnection);
    });
}

} // namespace gvt
