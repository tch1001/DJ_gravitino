// Pure hot-plug decisions: retain explicit routing and recover dead streams
// without restarting healthy devices or accidentally opening laptop speakers.
#pragma once
#include "AudioEngine.h"
#include <cstdint>

namespace gvt {
inline int audioOutputIndex(const QList<AudioOutputDevice>& devices,
                            const QString& preference)
{
    for (qsizetype i = 0; i < devices.size(); ++i)
        if (preference.isEmpty() ? devices[i].isDefault
                                 : devices[i].name == preference)
            return int(i);
    return -1;
}

enum class AudioOutputRecovery { None, Close, Reopen };
inline AudioOutputRecovery audioOutputRecovery(bool wanted, bool available,
    bool initialized, bool healthy, bool sameEndpoint)
{
    if (!wanted) return AudioOutputRecovery::None;
    if (!available)
        return initialized ? AudioOutputRecovery::Close : AudioOutputRecovery::None;
    return initialized && healthy && sameEndpoint ? AudioOutputRecovery::None
                                                   : AudioOutputRecovery::Reopen;
}

// A backend may still report "started" after callbacks cease. Heartbeats are
// sampled on the GUI thread; the audio callback only increments an atomic.
class AudioCallbackWatchdog {
public:
    void reset(std::uint64_t frames, qint64 nowMs) { frames_ = frames; lastMs_ = nowMs; }
    bool stalled(std::uint64_t frames, qint64 nowMs) {
        if (frames != frames_) reset(frames, nowMs);
        return nowMs - lastMs_ >= 3000;
    }
private:
    std::uint64_t frames_ = 0;
    qint64 lastMs_ = 0;
};
}
