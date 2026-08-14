# Gravitino Architecture

Read this before touching code. The interfaces in `src/*/... .h` marked
`// PINNED INTERFACE` are the contracts between modules — change them only by
updating this doc and every consumer in the same commit.

## The one invariant that makes everything work

**Every control action flows through the ControlBus** (`src/control/ControlBus.h`).

UI knob turned, FLX4 fader moved, transition replay tick — all of them emit the
same `ControlEvent {deck, ControlId, value}` onto the bus. Consumers (audio
engine, UI mirrors, MIDI LED feedback, transition recorder) subscribe to the bus.

This is what makes transition record/replay trivial and lossless:
- **Recording** = subscribing to the bus and timestamping events in *beats*.
- **Replay** = a beat-clock scheduler emitting the same events back onto the bus.
- Tutorial mode = replaying events as *prompts* (UI/LED hints) instead of actions,
  scoring the human's live events against the recording.

Never let a module reach around the bus to poke the audio engine directly
(exception: high-rate audio-thread-internal state like the render position).

## Modules

```
src/control/      ControlBus, ControlId enum, ControlEvent      (pure, no deps)
src/audio/        AudioEngine (miniaudio device), Deck, Mixer   (depends: control)
src/analysis/     TrackData decode, BeatAnalyzer, Metadata      (depends: -)
src/library/      TrackLibrary (QAbstractTableModel), scan+cache(depends: analysis)
src/transitions/  GvtFile parse/serialize, Recorder, Player     (depends: control, analysis)
src/midi/         MidiEngine (RtMidi), Flx4Mapping, LEDs        (depends: control)
src/ui/           MainWindow, DeckWidget, MixerWidget,
                  LibraryWidget, TransitionPanel                (depends: everything)
src/app/          main.cpp wiring, --selftest harness
third_party/      miniaudio.h (vendored)
```

Qt is used in: library, ui, app, and for signals in ControlBus (QObject).
audio/analysis/transitions core logic must stay Qt-light (QString/QObject OK,
no widgets) so they stay testable headless.

## Threading model

- **Audio thread** (miniaudio callback): lock-free. Reads deck/mixer parameters
  from `std::atomic<float>` members. Never allocates, never takes the GUI mutex.
- **GUI thread**: everything else, including ControlBus dispatch (Qt signals,
  direct connections). Parameter changes = GUI thread writes atomics.
- **MIDI thread** (RtMidi callback): converts raw MIDI → ControlEvent, posts to
  GUI thread via queued signal. LEDs written directly from GUI thread.
- **Analysis**: QtConcurrent / std::thread per track, results delivered via
  queued signal.
- Transition Player runs on a GUI-thread QTimer (~5 ms) reading the master
  deck's beat position from the audio engine (atomic double).

## Audio pipeline (per render callback, 48 kHz stereo f32)

```
Deck A PCM ──resample(tempo)──▶ trim ─▶ 3-band EQ ─▶ channel fader ─┐
                                                                    ├─▶ xfader ─▶ limiter ─▶ out
Deck B PCM ──resample(tempo)──▶ trim ─▶ 3-band EQ ─▶ channel fader ─┘
```

- Tracks are fully decoded to memory (`TrackData`, mono-summed peaks for UI +
  stereo f32 PCM). MP3 decode via miniaudio's built-in dr_mp3.
- Tempo: linear-interpolation resampler, ratio = targetBPM/trackBPM. (No keylock
  in MVP; pitch shifts with tempo like turntables. Keylock is a documented TODO.)
- EQ: RBJ biquad low-shelf 250 Hz / peak 1 kHz / high-shelf 4 kHz, ±26 dB with
  full-kill at slider bottom.
- Limiter: soft-clip tanh on master to avoid inter-deck clipping.

## Beatgrid & sync

`BeatAnalyzer` produces `{bpm, firstBeatSec}` — a *fixed-tempo* grid (fine for
electronic music; variable grids are a TODO). Beat position of a deck at sample
position `p` = `(p/rate - firstBeatSec) * bpm/60`. "Sync" sets the follower
deck's tempo ratio so its BPM matches the master and phase-aligns to the
nearest beat.

## Transition record/replay

See `docs/TRANSITION_FORMAT.md` for the file format. Runtime flow:

1. User loads track A (playing) and track B, arms **Record Transition**.
2. Recorder notes the *anchor*: beat position in A when recording starts, and
   the first beat position at which B is playing.
3. Every ControlEvent is logged with a timestamp in **beats relative to anchor**
   (master-deck beats). Beats, not seconds — so a transition recorded at 120 BPM
   replays correctly at 128.
4. Stop recording → normalize (dedupe, quantize option) → save `.gvt`.
5. Replay: user loads the same pair (matched by audio fingerprint/title), picks
   a transition, hits **Go**. Player waits until deck A crosses the anchor beat
   (or starts immediately, offsetting), then fires events on schedule with
   linear/s-curve interpolation between sparse values.

## Testing

- `ctest` unit tests in `tests/` (gvt round-trip, BPM on synthetic clicks, EQ
  response, scheduler timing).
- `./build/gravitino --selftest`: headless offline render — loads two demo MP3s,
  runs a scripted `.gvt` transition through the *real* engine (offline mode),
  writes `selftest_out.wav` + prints RMS/beat stats. This is how agents verify
  audio behavior without ears.
- Test MP3s on this machine: `~/Music/PioneerDJ/Demo Tracks/Demo Track 1.mp3`
  and `Demo Track 2.mp3` (both ~130 BPM electronic, ideal).

## DDJ-FLX4 notes

Appears as MIDI device "DDJ-FLX4". Mapping constants in
`src/midi/Flx4Mapping.h` (derived from the public MIDI spec / Mixxx mapping).
Channels: deck1 = ch0, deck2 = ch1, mixer = ch6. Jog: CC with relative ticks;
platter touch = note. LEDs: send the same note/CC back with velocity 0/127.
Hot-plug: MidiEngine polls port list every 2 s; connect/disconnect anytime.
