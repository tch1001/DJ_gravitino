// Lossless offline master sink: synchronous writes cannot drop frames, and a
// failed/cancelled export never replaces a previously completed recording.
#pragma once
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

namespace gvt {
class RecordingWav {
public:
    bool open(const QString& path, QString* error);
    bool write(const float* stereo, int frames, QString* error);
    bool finish(QJsonObject manifest, QString* error);
    qint64 frames() const { return frames_; }
private:
    QSaveFile file_;
    qint64 frames_ = 0;
    double peak_ = 0.0, squares_ = 0.0;
};
QJsonObject readRecordingManifest(const QString& path, QString* error = nullptr);
}
