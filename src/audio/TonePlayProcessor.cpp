// Pitch a non-destructive outgoing slice with a band-limited resampler. Notes
// are one-shots; a separate, pre-crossfaded vowel loop can sustain beneath them.
#include "TonePlayProcessor.h"
#include "ScratchDsp.h"
#include "Fx.h"
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
    std::vector<float> sustainPcm;
    std::optional<TonePlaySustain> sustain;
    double sustainPosition = 0, sustainAge = 0, sustainRate = 1;
    bool sustainStarted = false;
    std::optional<TonePlayEffects> effects;
    std::unique_ptr<DeckFx> echo, reverb;
    double effectsEndBeat = 0;
    double holdDuck = 1;
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
    if (p.effects && p.effects->enabled && !p.notes.empty()) {
        next->effects = p.effects;
        for (const auto &n : p.notes)
            next->effectsEndBeat = std::max(next->effectsEndBeat, n.beat + n.duration);
        next->effectsEndBeat += p.effects->tailBeats;
        const auto prepareReturn = [&](int type) {
            auto fx = std::make_unique<DeckFx>();
            // Select/clear storage and settle the wet-only mix off the callback.
            std::array<float, 8192> silence{};
            fx->process(silence.data(), 4096, type, true, 1, p.effects->echoBeats, source.bpm);
            return fx;
        };
        if (p.effects->echo > 0) next->echo = prepareReturn(0);
        if (p.effects->reverb > 0) next->reverb = prepareReturn(1);
    }
    if (p.sustain && p.sustain->enabled) {
        const auto &h = *p.sustain;
        const auto loopStart = std::llround((h.loopStartBeat - p.sourceStartBeat) *
                                           60 / source.bpm * kSampleRate);
        const auto loopEnd = std::llround((h.loopEndBeat - p.sourceStartBeat) *
                                         60 / source.bpm * kSampleRate);
        if (loopStart < 0 || loopEnd > static_cast<long long>(next->pcm.size() / 2) ||
            loopEnd - loopStart < 96) {
            if (error) *error = "Background vowel region must contain at least 2 ms inside the slice.";
            return false;
        }
        // Crossfade into the beginning, then skip that consumed beginning on wrap.
        // Linear weights preserve correlated vocal level without a +3 dB seam.
        const auto fade = std::clamp(std::llround(h.crossfadeMs * kSampleRate / 1000),
                                     1LL, (loopEnd - loopStart) / 2);
        const auto period = loopEnd - loopStart - fade;
        next->sustainPcm.resize(period * 2);
        for (long long i = 0; i < period; ++i) {
            const auto at = loopStart + fade + i;
            const auto blend = std::max(0LL, at - (loopEnd - fade));
            const double mix = double(blend) / fade;
            for (int ch = 0; ch < 2; ++ch)
                next->sustainPcm[i * 2 + ch] = static_cast<float>(
                    next->pcm[at * 2 + ch] * (1 - mix) +
                    next->pcm[(loopStart + blend) * 2 + ch] * mix);
        }
        next->sustain = h;
        next->sustainRate = std::exp2((h.pitch - p.rootNote) / 12.0);
        next->first = next->notes.empty() ? h.beat : std::min(next->first, h.beat);
    }
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
        bool foregroundActive = false;
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
            foregroundActive = foregroundActive || v.velocity > 0;
            for (int ch = 0; ch < 2; ++ch)
                out[frame * 2 + ch] +=
                    static_cast<float>(sample[ch] * s.gain * v.velocity * envelope);
            v.position += v.rate;
            v.age += 1;
        }
        if (s.effects && s.beat < s.effectsEndBeat) {
            const std::array<float, 2> dry {out[frame * 2], out[frame * 2 + 1]};
            // Preserve the direct sound. Only the short notes feed these returns,
            // so neither the sustained voice nor deck audio becomes an FX wash.
            const double fade = std::clamp((s.effectsEndBeat - s.beat) /
                                           std::min(.25, s.effects->tailBeats), 0.0, 1.0);
            const auto send = [&](DeckFx *fx, int type, double level) {
                if (!fx) return;
                auto wet = dry;
                fx->process(wet.data(), 1, type, true, 1, s.effects->echoBeats, s.nativeBpm * tempo);
                for (int ch = 0; ch < 2; ++ch)
                    out[frame * 2 + ch] += static_cast<float>(wet[ch] * level * fade);
            };
            send(s.echo.get(), 0, s.effects->echo);
            send(s.reverb.get(), 1, s.effects->reverb);
        }
        const double duckTarget = s.effects && foregroundActive ? 1 - s.effects->sustainDucking : 1;
        s.holdDuck += (duckTarget - s.holdDuck) * (duckTarget < s.holdDuck ? .006920388 : .00041658);
        if (s.sustain && s.beat >= s.sustain->beat &&
            s.beat < s.sustain->beat + s.sustain->duration) {
            const auto &h = *s.sustain;
            const auto count = static_cast<long long>(s.sustainPcm.size() / 2);
            if (!s.sustainStarted) {
                s.sustainStarted = true;
                s.sustainAge = std::max(0.0, (s.beat - h.beat) / increment);
                s.sustainPosition = std::fmod(s.sustainAge * s.sustainRate, double(count));
            }
            const auto sample = s.resampler.sample(s.sustainPosition, s.sustainRate,
                [&](double at) {
                    const auto i = (static_cast<long long>(at) % count + count) % count;
                    return std::array<float, 2>{s.sustainPcm[i * 2], s.sustainPcm[i * 2 + 1]};
                });
            const double envelope = std::clamp(std::min(
                s.sustainAge / (h.attackMs * kSampleRate / 1000),
                (h.beat + h.duration - s.beat) / increment /
                    (h.releaseMs * kSampleRate / 1000)), 0.0, 1.0);
            for (int ch = 0; ch < 2; ++ch)
                out[frame * 2 + ch] += static_cast<float>(sample[ch] * s.gain * h.gain * envelope * s.holdDuck);
            s.sustainPosition = std::fmod(s.sustainPosition + s.sustainRate, double(count));
            s.sustainAge += 1;
        }
        s.beat += increment;
    }
    publishedBeat_.store(s.started ? s.beat : -1.0);
}
} // namespace gvt
