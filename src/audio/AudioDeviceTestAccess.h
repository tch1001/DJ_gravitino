// Internal headless device seam: exercise real miniaudio lifecycle/recovery
// using its null backend, never the user's speakers or system audio settings.
#pragma once
namespace gvt {
class AudioEngine;
namespace detail {
struct AudioDeviceTestAccess {
    static bool useNullBackend(AudioEngine& engine);
    static void stopBackend(AudioEngine& engine);
    static void notifyInterruption(AudioEngine& engine);
    static bool backendActive(const AudioEngine& engine);
};
}
}
