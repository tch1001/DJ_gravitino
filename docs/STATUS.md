# STATUS — agent handoff board

> Keep this file current. When you finish or abandon work, update your section.
> An agent with zero prior context must be able to resume from this file plus
> docs/ARCHITECTURE.md and docs/TRANSITION_FORMAT.md.

## Current state (update the date/line when you change things)

- 2026-08-15 ~05:00: **MVP complete and verified end-to-end.** All modules
  implemented, app builds/links clean, 4/4 unit tests pass, `--selftest` green
  (decode ▸ BPM ▸ .gvt parse/serialize ▸ offline transition render ▸ recorder
  round-trip). Live GUI verified by screenshot + accessibility automation:
  tracks load with waveforms, decks play on the real output device, a recorded
  transition PERFORMs live (deck B auto-starts, crossfader glides), and
  TUTORIAL mode overlays beat-countdown prompts. FLX4 mapping is implemented
  but NOT yet verified against hardware (controller was unplugged all night).
  A sample transition is installed at ~/Music/Gravitino/Transitions/demo-blend.gvt.
- 2026-08-15 (claude-transitions): transitions module implemented. .gvt
  parse/serialize round-trips losslessly and re-emits the exact aligned style
  of the spec example; recorder coalesces (<0.05 beat) + thins linear runs;
  player is a 5 ms beat-clock scheduler with linear/scurve glides, ToDeck
  play-seek-to-anchor, and Tutorial prompt/score mode. tests/test_gvt.cpp and
  tests/test_player.cpp pass (verified standalone against QtCore; full ctest
  link still waits on the other modules). Notes:
  - gvt::matchTrack() is NOT in GvtFormat.cpp — it belongs to
    library/TransitionStore.cpp (claude-analysis).
  - Tutorial mode selection: the pinned TransitionEngine.h has no setter, so
    call gvt::transitionPlayerSetMode(player, PlayerMode::Tutorial) from
    src/transitions/TransitionPlayerExt.h BEFORE arm() (static side-table
    inside TransitionPlayer.cpp; default Perform).
  - TransitionEngine.h has no same-named .cpp so AUTOMOC skips it; its moc is
    pulled in via `#include "moc_TransitionEngine.cpp"` at the bottom of
    TransitionRecorder.cpp, and both Impl structs live in the internal header
    src/transitions/TransitionImpls.h so that moc TU sees complete types.
  - Pure glide/schedule math is header-only in src/transitions/PlayerMath.h
    (used by tests without linking the audio engine).
  - extraMeta convention: unknown [meta] keys stored as-is; unknown keys from
    other sections stored as "<section>.<key>" and re-emitted in place.

## Build & test quickstart

```sh
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
./build/gravitino --selftest   # offline render check, writes selftest_out.wav
./build/gravitino              # GUI
```

Test tracks: `~/Music/PioneerDJ/Demo Tracks/Demo Track {1,2}.mp3`.
FLX4 may or may not be plugged in — MidiEngine must never crash without it.

## Module ownership / progress

| Module (files) | Owner | State |
|---|---|---|
| control/ControlBus.* | orchestrator | DONE |
| audio/{AudioEngine,Deck,Eq}.cpp | codex-audio | DONE |
| analysis/{TrackData,BeatAnalyzer}.cpp, library/*.cpp | claude-analysis | DONE — decode/tags/fingerprint/beatgrid + library model/cache + TransitionStore (incl. gvt::matchTrack). Demo Track 1 → 128.0 BPM, Demo Track 2 → 120.0 BPM. tests/test_beats.cpp + tests/test_fingerprint.cpp pass standalone; ctest link pending peers' objects. |
| transitions/{GvtFormat,TransitionRecorder,TransitionPlayer}.cpp, tests | claude-transitions | DONE |
| midi/{MidiEngine,Flx4Mapping}.cpp | codex-midi | DONE |
| ui/*.cpp, app/* | claude-ui | DONE — ui/{MainWindow,DeckWidget,MixerWidget,LibraryWidget,TransitionPanel,Theme}.h+cpp, app/main.cpp, app/SelfTest.h (declares `gvt::runSelfTest`; SelfTest.cpp is the orchestrator's). All 6 TUs + moc outputs compile warning-clean (-Wall -Wextra). Notes: (1) pinned TrackLibrary.h/TransitionEngine.h pImpl classes (TransitionStore/Recorder/Player) declare no destructor, so any TU destroying them fails to compile — main.cpp heap-allocates them with process lifetime as a workaround; header owners should add declared dtors (moc/mocs_compilation may hit the same issue). (2) `gvt::transitionPlayerSetMode` doesn't exist and `arm()` has no PlayerMode arg, so the TUTORIAL button is disabled ("coming soon"); tutorialPrompt/tutorialScored signals are wired (banner + accuracy toasts) and light up once the player emits them. (3) Hotcue clear writes `track->hotCues[i] = -1` directly (no Deck clear API). (4) UI mirrors Play state by polling `deck.playing` at 30 Hz; continuous controls mirror via bus events with QSignalBlocker. |
| integration/selftest | orchestrator | DONE — see Verification below |

### DDJ-FLX4 MIDI messages mapped

`hh` is velocity, `mm` is the 7-bit MSB, and `ll` is the 7-bit LSB.
The FLX4 sends each 14-bit control MSB first, then LSB; Gravitino dispatches
the completed value on receipt of the LSB. Deck A/B below are ControlBus deck
0/1 respectively.

| Control | Deck | MIDI input (hex) | ControlBus mapping / MIDI LED output |
|---|---:|---|---|
| PLAY/PAUSE | A / B | `90 0B hh` / `91 0B hh` | Nonzero press toggles `Play`/`Stop`; LED `90 0B 7F/00` / `91 0B 7F/00` |
| Deck CUE | A / B | `90 0C hh` / `91 0C hh` | Nonzero press → `Cue`; LED `90 0C 7F/00` / `91 0C 7F/00` |
| BEAT SYNC | A / B | `90 58 hh` / `91 58 hh` | Nonzero press → `TempoSync` |
| Hot-cue pads 1–8 (HOT CUE mode) | A | `97 00..07 hh` | Nonzero press → `HotCue1..8`; LED `97 00..07 7F/00` |
| Hot-cue pads 1–8 (HOT CUE mode) | B | `99 00..07 hh` | Nonzero press → `HotCue1..8`; LED `99 00..07 7F/00` |
| TEMPO slider, 14-bit | A / B | `B0/B1 00 mm`, then `B0/B1 20 ll` | Raw `0000`/`2000`/`3FFF` → ratio `0.92`/`1.00`/`1.08`; physical `+`/down/toward-user end is faster |
| Channel fader, 14-bit | A / B | `B0/B1 13 mm`, then `B0/B1 33 ll` | `Fader = raw / 3FFF` |
| TRIM, 14-bit | A / B | `B0/B1 04 mm`, then `B0/B1 24 ll` | `Trim = raw / 3FFF` |
| EQ HI, 14-bit | A / B | `B0/B1 07 mm`, then `B0/B1 27 ll` | `EqHigh = raw / 3FFF` |
| EQ MID, 14-bit | A / B | `B0/B1 0B mm`, then `B0/B1 2B ll` | `EqMid = raw / 3FFF` |
| EQ LOW, 14-bit | A / B | `B0/B1 0F mm`, then `B0/B1 2F ll` | `EqLow = raw / 3FFF` |
| Crossfader, 14-bit | Global | `B6 1F mm`, then `B6 3F ll` | `Crossfader = raw / 3FFF`; `0000` = full left/deck A, `3FFF` = full right/deck B |
| Jog side / platter (vinyl on / vinyl off) | A / B | `B0/B1 21/22/23 vv` | `Jog = signed(vv - 40)` ticks; `41` = +1 clockwise, `3F` = -1 counterclockwise, `40` ignored; all modes nudge for MVP |

Button NOTE ON messages with velocity `00` (release), and explicit NOTE OFF
messages, are ignored because these MVP controls are actions. LED state is
sent only for non-MIDI-origin bus events to prevent feedback loops; cached
play, cue, and hot-cue state is restored when the output port reconnects.

## Known decisions & gotchas

- Engine sample rate fixed at 48 kHz (`kSampleRate`); all decode resamples to it.
- Audio track replacement uses a stop-and-drain swap; the callback uses only
  atomics, fixed 256-frame scratch buffers, and allocation-free DSP.
- Deck DSP uses linear tempo/jog resampling, RBJ three-band EQ, equal-power
  crossfade, and a shared realtime/offline `tanh` soft-clip mix path.
- No keylock in MVP — tempo change shifts pitch (documented TODO).
- Beatgrid is fixed-tempo (bpm + firstBeatSec) — fine for the demo tracks.
- ControlBus dispatch is synchronous, GUI thread only. MIDI thread must post
  via QMetaObject::invokeMethod(..., Qt::QueuedConnection).
- Audio-thread params are std::atomic on Deck/AudioEngine — no locks in render.
- .gvt parsers skip unknown controls with warnings, never hard-fail.
- Qt is keg-only at /opt/homebrew/opt/qt (CMAKE_PREFIX_PATH already set in
  CMakeLists.txt).

## Verification summary (2026-08-15)

- `ctest`: test_beats, test_fingerprint, test_gvt, test_player — all pass.
- `./build/gravitino --selftest`: BPMs 127.99/119.98 on the demo tracks,
  scripted 16-beat blend renders with sane RMS/no NaN, recorder captures 5/5
  events and round-trips through a .gvt file.
- Live: PLAY/PERFORM/TUTORIAL exercised via accessibility automation.
- BPM sanity on real pop: Pink Venom 90.03, Can't Stop The Feeling 113.00.

## Integration decisions added after the parallel phase

- Checkable buttons wire `toggled` (not `clicked`) so programmatic/AX toggles
  dispatch too; `refresh()` mirrors engine state under QSignalBlocker.
- PERFORM/TUTORIAL auto-start the from-deck if stopped.
- Recorder quantizes saved files (0.01 BPM/sec, 0.001 beats) for clean diffs.
- BPM octave preference window is 80..160 (threshold 0.65× best score).
- Library scan is fully recursive under the chosen folder, hidden dirs skipped.
- Dev flags: `--selftest`, `--autoload [substrA substrB]` (loads matching
  library tracks onto decks once analyzed; defaults to the Pioneer demo pair).

## Morning checklist (FLX4 hardware bring-up)

1. Plug in the DDJ-FLX4, launch `./build/gravitino` — status bar should flip
   to "DDJ-FLX4 connected" within ~2 s (hot-plug poll).
2. Check: play/cue per deck, faders/EQ/trim, crossfader, tempo sliders
   (polarity!), pads = hot cues, SYNC, jog nudge, LED echo on play/pads.
3. Mapping table (message-level) is in the MIDI section below; fix constants
   in src/midi/Flx4Mapping.cpp if any control is off.

## TODO backlog (post-MVP)

- Fill the deck dead space (below hot cues) with loop / FX / beat-jump
  sections as we chase Serato feature parity — the layout was deliberately
  densified (11px base font, compact pads/knobs) to leave room for this.
- Keylock (signalsmith-stretch), variable beatgrids, WAV/FLAC/AAC, transition
  marketplace/sharing UI, Windows/Linux builds, .app bundling + macdeployqt.
