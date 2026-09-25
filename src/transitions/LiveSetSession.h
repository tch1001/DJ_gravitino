// Owns an isolated set producer and bounded SPSC audio ring. The normal decks
// remain a preparation workspace; none of their controls reach this graph.
#pragma once
#include "SetRenderer.h"
#include "../audio/AudioEngine.h"
#include <QFutureWatcher>
#include <QTimer>
#include <QThreadPool>
#include <mutex>

namespace gvt {
class LiveSetSession final : public QObject, public AudioPreviewSource {
    Q_OBJECT
public:
    explicit LiveSetSession(AudioEngine* output, QObject* parent=nullptr);
    ~LiveSetSession() override;
    bool start(const SetRenderRequest&, QString* error);
    bool append(const SetRenderRequest&, QString* error);
    void pause(bool paused);
    void stop(); // stays in protected preparation routing (silent MASTER)
    bool leave(QString* error); // explicit return, requires stopped prep
    bool protectedRouting() const;
    bool running() const { return running_; }
    int acceptedTransitions() const { return acceptedTransitions_; }
    bool paused() const { return paused_.load(); }
    double elapsedSeconds() const;
    double bufferedSeconds() const;
    QString status() const;
    void read(float*, int) noexcept override;
signals:
    void changed();
    void routingChanged(bool active);
    void appendFinished(bool accepted, const QString& message);
private:
    bool write(const float*, int);
    AudioEngine* output_;
    // Thirty seconds absorbs asset/stem decoding without touching callbacks.
    static constexpr uint64_t capacity_ = uint64_t(kSampleRate)*30;
    std::vector<float> ring_;
    std::atomic<uint64_t> written_{0}, read_{0}, underruns_{0};
    std::atomic<bool> cancelled_{false}, paused_{true}, buffering_{true}, done_{false};
    bool running_=false, appendPending_=false, producerActive_=false;
    int acceptedTransitions_=0, requestedCount_=0;
    bool failed_=false;
    QString resultMessage_;
    struct Section { qint64 frame; QString name; int index; };
    std::vector<Section> sections_;
    std::mutex appendMutex_;
    std::optional<SetRenderRequest> appendRequest_;
    QFutureWatcher<SetRenderResult> worker_;
    QThreadPool producerPool_;
    QTimer timer_;
};
}
