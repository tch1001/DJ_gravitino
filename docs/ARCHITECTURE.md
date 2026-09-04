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
src/library/      TrackLibrary, SongCatalog, TransitionStore    (depends: analysis)
src/transitions/  typed model, YAML/legacy readers, Recorder,
                  Player                                        (depends: control, analysis)
src/midi/         MidiEngine (RtMidi), Flx4Mapping, LEDs        (depends: control)
src/ui/           MainWindow, DeckWidget, MixerWidget,
                  LibraryWidget, TransitionPanel, typed visual
                  TransitionEditor                              (depends: everything)
src/app/          main.cpp wiring, --selftest harness
third_party/      miniaudio.h (vendored)
```

GUI startup takes a per-user `QLockFile` before constructing `AudioEngine`.
Only one Gravitino GUI process may own a CoreAudio stream at a time; headless
`--selftest` runs before that guard and remains independently runnable.

Qt is used in: library, ui, app, and for signals in ControlBus (QObject).
audio/analysis/transitions core logic must stay Qt-light (QString/QObject OK,
no widgets) so they stay testable headless.

## Threading model

- **Audio thread** (miniaudio callback): lock-free. Reads deck/mixer parameters
  from `std::atomic<float>` members. Never allocates, never takes the GUI mutex.
- **GUI thread**: everything else, including ControlBus dispatch (Qt signals,
  direct connections). Parameter changes = GUI thread writes atomics.
- **Track replacement**: `Deck::loadTrack()` closes the render gate, drains
  any active callback, clears the old PCM/stems/FX state, publishes the new
  source, and returns stopped at frame zero. It must never crossfade or layer
  the old source with the replacement.
- **Transition-editor preview**: the GUI thread renders a private two-deck
  `AudioEngine` into a bounded lock-free stereo ring. While its exclusive
  preview lease is held, the live callback reads that ring into MASTER and
  neither renders nor advances the live decks. All live ControlBus origins and
  deck-loading UI are blocked for the lease. Lease release drains any
  in-flight callback before the private source can be destroyed. The callback
  remains allocation- and lock-free; preview bypasses the live master tap.
- **MIDI thread** (RtMidi callback): converts raw MIDI → ControlEvent, posts to
  GUI thread via queued signal. LEDs written directly from GUI thread.
- **Analysis**: QtConcurrent / std::thread per track, results delivered via
  queued signal.
- Transition Player runs on a GUI-thread QTimer (~5 ms) reading the master
  deck's beat position from the audio engine (atomic double).

## Audio pipeline (per render callback, 48 kHz stereo f32)

```
Deck A PCM ─▶ tempo/trim/EQ/filter/FX ─┬─▶ channel fader ─┐
                                      │                    ├─▶ xfader/limiter ─▶ MASTER 1/2
Deck B PCM ─▶ tempo/trim/EQ/filter/FX ─┤─▶ channel fader ─┘
                                      └─▶ selected PFL + master mix ─▶ PHONES 3/4
```

- Tracks are fully decoded to memory (`TrackData`, mono-summed peaks for UI +
  stereo f32 PCM). Miniaudio decodes MP3, FLAC, WAV, and AIFF into the engine's
  common 48 kHz stereo representation.
- Tempo: ratio = targetBPM/trackBPM. With KEY LOCK off, linear-interpolation
  resampling changes pitch like a turntable. With it on, per-deck Signalsmith
  Stretch time-stretching preserves musical pitch; scratch remains direct.
  The pitch fader has persisted Serato-style ±8%, ±16%, and ±50% ranges;
  selecting a different range never changes the current ratio by itself.
- EQ: RBJ biquad low-shelf 250 Hz / peak 1 kHz / high-shelf 4 kHz, ±26 dB with
  full-kill at slider bottom.
- Limiter: soft-clip tanh on master to avoid inter-deck clipping.
- Settings > Audio Output selects the persisted CoreAudio master device; the
  initial default follows macOS, so MacBook and Bluetooth speakers work.
  Selecting DDJ-FLX4 uses one four-channel stream (master 1/2, phones 3/4).
  Selecting another master device opens a second four-channel FLX4 stream and
  feeds only its phones 3/4 from a bounded lock-free cue ring. This prevents a
  second audio callback from advancing/rendering either deck again.
  Because CoreMIDI may enumerate slightly before the USB CoreAudio endpoint,
  the UI retries that secondary phones stream while the controller is present
  and reopens a stopped endpoint after hot-plug. The Audio Output menu includes
  a low-volume phones-only test tone for end-to-end channel 3/4 diagnosis.
  Channel CUE monitors the post-EQ/filter/FX, pre-fader deck signal, unaffected
  by channel faders or the crossfader. HEADPHONES MIX balances that PFL bus
  against master CUE. Without a connected FLX4, master output stays stereo and
  the UI reports that physical headphone cue is unavailable.

## Beatgrid & sync

`BeatAnalyzer` produces `{bpm, firstBeatSec}` — a *fixed-tempo* grid (fine for
electronic music; variable grids are a TODO). Beat position of a deck at sample
position `p` = `(p/rate - firstBeatSec) * bpm/60`. "Sync" sets the follower
deck's tempo ratio so its BPM matches the master and phase-aligns to the
nearest beat. Portable transitions address canonical arrangement beats. A
confirmed local catalog binding may add one constant offset to this analyzed
asset beat; legacy `.gvt` coordinates retain their historical asset-local
interpretation.

The library cache keeps the analyzer's latest BPM/anchor separately from the
effective beat grid and records whether that grid came from analysis, a user
edit, or a protected migration. Adding fingerprints, tags, or another derived
analysis field therefore performs a merge migration: an approved/legacy grid,
permanent hot cues, and saved loops survive unchanged while only the derived
fields refresh. Records predating the source marker are conservatively treated
as protected because they may contain manual work. The tempo candidate search
includes octave, 3:2, and 4:3 ratios so prominent pop subdivisions do not force
a harmonic BPM alias.

## Transition record/replay

See `docs/TRANSITION_FORMAT.md` for the file format. Runtime flow:

1. User loads track A (playing) and track B, arms **Record Transition**.
2. Recorder notes the *anchor*: beat position in A when recording starts, and
   the first beat position at which B is playing. It also captures a complete
   role-based pre-transition snapshot: both decks' transport/cue, tempo,
   channel/EQ/filter, loop, FX and stem state, plus the mixer crossfader. TRIM
   is deliberately excluded because input gain remains live DJ state.
3. Every ControlEvent is logged with a timestamp in **beats relative to anchor**
   (master-deck beats). Beats, not seconds — so a transition recorded at 120 BPM
   replays correctly at 128. When a performance pad caused the audible event,
   the recorder also stores an optional physical input hint. Replay
   remains state-based; Tutorial can therefore say “CUSTOM pad 3” instead of
   misleadingly reducing the gesture to its resulting `play` event.
4. Stop recording → capture the exact authored completion beat,
   normalize/thin continuous streams → save a typed v1 YAML
   `.transition`. Semantic hot cues and saved loops (canonical IN/OUT beats)
   are allocated together into an isolated temporary CUSTOM bank without
   changing permanent per-track cues or loop slots.
5. Replay: user loads the same pair (confirmed catalog binding, encode-tolerant
   structural fingerprint, checked recording ID, or explicit manual confirmation), picks
   a transition, then uses **Perform** to reconstruct the recorded pre-state
   and roll from the anchor, or **Prime** to prepare it while retaining A's live
   position and wait for A to cross the anchor. The player reasserts incoming
   transport at that boundary, then fires events on schedule with linear/
   s-curve interpolation between sparse values.
6. Perform records which FLX4 absolute controls were changed by setup/replay.
   At the final event, `SoftTakeover` compares their last known physical
   positions with engine truth. If any differ, all controller input is frozen
   while only TEMPO/channel fader/HIGH/MID/LOW/FILTER/crossfader pickup moves
   are consumed. Only controls whose virtual value actually changed are armed.
   Originally mismatched targets stay monitored until every one is
   simultaneously inside tolerance, so an aligned knob that is overshot lights
   up again. The final pickup re-enables the controller without ever applying a
   discontinuous hardware value to audio.

The top workspace is `Deck A | compact FLX4 mixer | Deck B`; the centered mixer
starts below the overview-waveform baseline, followed by the full-width detail
waveform. Each deck includes a position-driven rotating platter. A mouse drag
sends `PlatterScratch` without `PlatterTouch`, selecting the engine's direct
position-adjustment path without changing PLAY state. This permits
millisecond-scale beat-matching during automated replay (roughly 45 ms per
quarter turn). The FLX4's touch-gated mapping is unchanged. A mapped
HOT CUE pad also remains held while dragged to PLAY; dropping there dispatches
PLAY before releasing the cue, reusing the engine's hardware latch semantics.
TUTOR VIEW is persistent UI state, not a replay command: opening it
does not change either of those upper regions. In the lower workspace it adds a
large virtual FLX4 at left and narrows only the stacked transition controls,
event sequence, and library at right. The library remains independently
showable through a permanent status-bar toggle immediately left of the
controller connection text; transient messages cannot cover it. Event Sequence
opens in Human mode: independent role/control streams become start-to-end actions,
overlapping outgoing/incoming moves share a two-lane row, and common hot-cue
launch gestures become one instruction. Raw mode retains the prior recorded
event table; both views derive from the same typed timeline and never alter
serialized checkpoints.
The Library's Transitions tab keeps legacy and portable files as distinct rows
and exposes independent `.gvt` and `.transition` checkboxes, both enabled by
default. This makes migration comparisons explicit instead of hiding the
legacy source behind its portable counterpart. `--convert-transitions` creates
only missing portable counterparts and is safe to run repeatedly.
During Perform and Tutorial, a translucent bar fills across the most recently
reached row until the next distinct action beat, so the row change itself is
the timing cue. A derived, non-actionable beat-zero “Transition starts” row
provides the first countdown interval; simultaneous actions do not create
zero-length visual countdowns. Neither the marker nor its progress is written
to `.transition` or legacy `.gvt`.
The full-size Transition Editor is the creation/editing surface for the same
typed model consumed by replay. A single undoable working copy drives two
waveforms, an action lane, independent `(role, control)` automation lanes,
semantic cue/loop and initial-state inspectors, and an advanced safe-YAML
view. Timeline points, cue markers, loop edges, labels, and the explicit END
marker are directly draggable; grid snapping never changes stored precision.
Starting preview reconstructs cursor state by rendering the private graph from
beat zero, then routes only that graph to MASTER. A Write Automation take
punch-replaces touched streams as one undo command. Autosave drafts, source
hash conflict detection, forced Save As for legacy/endpoint edits, and
schema-level validation keep library files non-destructive and reopenable.
The virtual FLX4 mirrors live controls, pad state, LEDs, and channel meters;
prose/countdown/reset guidance lives in the transition control panel above
CLOSE ENOUGH. Perform gives the guided run up to eight beats of pre-anchor
countdown; Prime arms the same guidance against the live outgoing deck.

## Testing

- `ctest` unit tests in `tests/` (portable/legacy round trips and hostile YAML,
  multi-format identity, catalog/cue behavior, BPM, EQ, scheduler timing).
- `./build/gravitino --selftest`: headless offline render — loads two demo tracks,
  migrates a scripted legacy transition through portable YAML, replays it via
  the *real* engine (offline mode), and writes `selftest_out.wav` plus beat/RMS stats. This is how agents verify
  audio behavior without ears.
- Test MP3s on this machine: `~/Music/PioneerDJ/Demo Tracks/Demo Track 1.mp3`
  and `Demo Track 2.mp3` (both ~130 BPM electronic, ideal).

## DDJ-FLX4 notes

Appears as MIDI device "DDJ-FLX4". Mapping constants in
`src/midi/Flx4Mapping.h` (derived from the public MIDI spec / Mixxx mapping).
Channels: deck1 = ch0, deck2 = ch1, mixer = ch6. Jog: CC with relative ticks;
platter touch = note. LEDs: send the same note/CC back with velocity 0/127.
The two five-segment channel meters use the documented `B0/B1 02` output and
receive dB-scaled post-EQ/filter/FX, pre-fader peaks every 40 ms.
Hot-plug: MidiEngine polls port list every 2 s; connect/disconnect anytime.
Controller disconnect clears any pending soft-takeover gate. Tutorial mode
does not arm takeover: its FLX4 diagram instead animates continuous controls
toward their recorded targets and keeps the prompt alive until the value is
actually reached. Recorded performance-pad `via=` hints illuminate the actual
pad layer and pad number, while transition-critical hot-cue mappings are
checked before a cue can be deleted.
