// PINNED INTERFACE — record the master output to a WAV file.
// Audio thread pushes into a lock-free ring via feed(); a worker thread
// drains to disk. Owner: claude-recmaster.
#pragma once
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

namespace gvt {

class MasterRecorder : public QObject {
    Q_OBJECT
public:
    explicit MasterRecorder(QObject* parent = nullptr);
    ~MasterRecorder() override;

    // GUI thread. start() truncates/creates a 16-bit stereo 48 kHz WAV at
    // path ("" = default ~/Music/Gravitino/Recordings/<timestamp>.wav) and
    // returns false + error on I/O failure. stop() finalizes the header.
    bool start(const QString& path, QString* error);
    void stop();
    bool isRecording() const;
    QString currentPath() const;
    double recordedSec() const;

    // AUDIO THREAD. Lock-free, never blocks or allocates; drops (and counts)
    // frames if the ring is full.
    void feed(const float* interleavedStereo, int frames);

signals:
    void recordingChanged(bool active, const QString& path);

private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt
