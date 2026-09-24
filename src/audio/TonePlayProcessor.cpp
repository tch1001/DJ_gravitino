// Pitch a non-destructive outgoing slice with a band-limited resampler. Notes
// are one-shots: gates can shorten them; higher pitches naturally finish sooner.
#include "TonePlayProcessor.h"
#include "ScratchDsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace gvt {
struct TonePlayProcessor::State {
    struct Voice {
        bool active = false;
        double position = 0, rate = 1, endBeat = 0, age = 0, velocity = 0;
    };
    std::vector<float> pcm;
    std::vector<TonePlayNote> notes;
    std::array<Voice, 16> voices{};
    ScratchResampler resampler;
    double anchor = 0, nativeBpm = 120, beat = 0, first = 0, last = 0, gain = 0.7;
    int root = 60;
    bool replace = false, started = false;
    size_t next = 0;
};
TonePlayProcessor::TonePlayProcessor() = default;
TonePlayProcessor::~TonePlayProcessor() { clear(); }
void TonePlayProcessor::clear() {
    enabled_.store(false);
    swapping_.store(true);
    while (readers_.load() != 0)
        std::this_thread::yield();
    state_.reset();
    publishedBeat_.store(-1);
    swapping_.store(false);
}
bool TonePlayProcessor::prepare(const TrackData &source, const TonePlayPattern &p, double start,
                                double end, double anchor, QString *error) {
    if (!validateTonePlay(p, error))
        return false;
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(anchor) || start < 0 ||
        end <= start || end > source.durationSec || end - start > 8.0 + 1e-9 ||
        end > double(source.pcm.size() / 2) / kSampleRate || !std::isfinite(source.bpm) ||
        source.bpm <= 0) {
        if (error)
            *error = "Tone-play snippet must lie inside the outgoing audio and be no longer than 8 "
                     "seconds.";
        return false;
    }
    const auto begin = static_cast<size_t>(std::llround(start * kSampleRate));
    const auto finish = static_cast<size_t>(std::llround(end * kSampleRate));
    if (finish <= begin || finish * 2 > source.pcm.size()) {
        if (error)
            *error = "Tone-play source audio is incomplete.";
        return false;
    }
    auto next = std::make_unique<State>();
    next->pcm.assign(source.pcm.begin() + begin * 2, source.pcm.begin() + finish * 2);
    next->notes = p.notes;
    std::stable_sort(next->notes.begin(), next->notes.end(),
                     [](const auto &a, const auto &b) { return a.beat < b.beat; });
    next->anchor = anchor;
    next->nativeBpm = source.bpm;
    next->gain = p.gain;
    next->root = p.rootNote;
    next->replace = p.replaceOutgoing;
    next->first = next->notes.empty() ? 0 : next->notes.front().beat;
    next->last = tonePlayEndBeat(p);
    clear();
    swapping_.store(true);
    while (readers_.load() != 0)
        std::this_thread::yield();
    state_ = std::move(next);
    swapping_.store(false);
    return true;
}
void TonePlayProcessor::render(float *out, float *original, int frames, double position,
                               bool playing, double tempo) noexcept {
    std::fill(out, out + frames * 2, 0.0f);
    std::fill(original, original + frames, 1.0f);
    readers_.fetch_add(1);
    struct Guard {
        std::atomic<int> &readers;
        ~Guard() { readers.fetch_sub(1); }
    } guard{readers_};
    if (swapping_.load() || !enabled_.load() || !state_)
        return;
    State &s = *state_;
    if (!std::isfinite(tempo) || tempo <= 0)
        return;
    const double increment = s.nativeBpm * tempo / (60.0 * kSampleRate);
    const auto sourceFrames = static_cast<double>(s.pcm.size() / 2);
    for (int frame = 0; frame < frames; ++frame) {
        if (!s.started) {
            const double at = position + frame * tempo / kSampleRate;
            if (!playing || at + 1e-10 < s.anchor)
                continue;
            s.started = true;
            s.beat = std::max(0.0, (at - s.anchor) * s.nativeBpm / 60.0);
        }
        while (s.next < s.notes.size() && s.notes[s.next].beat <= s.beat + 1e-10) {
            const auto &n = s.notes[s.next++];
            if (n.beat + n.duration <= s.beat)
                continue;
            auto voice = std::find_if(s.voices.begin(), s.voices.end(), [&](const auto &v) {
                return !v.active || s.beat >= v.endBeat;
            });
            if (voice == s.voices.end())
                continue; // validated maximum polyphony
            const double rate = std::exp2((n.pitch - s.root) / 12.0);
            const double age = std::max(0.0, (s.beat - n.beat) / increment);
            *voice = {true, age * rate, rate, n.beat + n.duration, age, n.velocity};
        }
        if (s.replace && s.beat >= s.first && s.beat < s.last) {
            const double fade =
                std::min((s.beat - s.first) / increment, (s.last - s.beat) / increment) / 240.0;
            original[frame] = 1.0f - static_cast<float>(std::clamp(fade, 0.0, 1.0));
        }
        for (auto &v : s.voices) {
            if (!v.active)
                continue;
            if (s.beat >= v.endBeat || v.position >= sourceFrames) {
                v.active = false;
                continue;
            }
            const auto sample = s.resampler.sample(v.position, v.rate, [&](double at) {
                if (at < 0 || at >= sourceFrames)
                    return std::array<float, 2>{};
                const auto i = static_cast<size_t>(at) * 2;
                return std::array<float, 2>{s.pcm[i], s.pcm[i + 1]};
            });
            const double envelope =
                std::clamp(std::min({v.age / 48.0, (v.endBeat - s.beat) / increment / 240.0,
                                     (sourceFrames - v.position) / v.rate / 240.0}),
                           0.0, 1.0);
            for (int ch = 0; ch < 2; ++ch)
                out[frame * 2 + ch] +=
                    static_cast<float>(sample[ch] * s.gain * v.velocity * envelope);
            v.position += v.rate;
            v.age += 1;
        }
        s.beat += increment;
    }
    publishedBeat_.store(s.started ? s.beat : -1.0);
}
} // namespace gvt
