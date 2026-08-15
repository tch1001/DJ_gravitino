# STATUS — agent handoff board

> Keep this file current. When you finish or abandon work, update your section.
> An agent with zero prior context must be able to resume from this file plus
> docs/ARCHITECTURE.md and docs/TRANSITION_FORMAT.md.

## Current state (update the date/line when you change things)

- 2026-08-15 (claude-ui2): **UI restructured toward Serato DJ Pro parity.**
  New layout (top to bottom): Row 1 = Deck A | Deck B panels at exactly 1:1
  stretch (no center mixer column); each deck has a compact 44 px overview
  waveform (click-to-seek, slot-colored hotcue flags, cue-point notch,
  orange "T" transition-entry marker), title/artist/BPM→effective/elapsed-
  remaining info lines, PLAY/CUE/SYNC transport, 2x4 square hot-cue pads,
  and a vertical tempo slider on the OUTER edge (left for A, right for B).
  Row 2 = new full-width `DetailWaveformView` (src/ui/DetailWaveformView.*,
  added to CMakeLists gravitino sources): two 72 px scrolling zoomed lanes
  (A over B), fixed center playhead, band-colored bars from
  overviewLow/Mid/High (#e0554d/#54c17a/#5a8fe8, low in back; gray
  overviewPeaks fallback), per-beat grid ticks (strong every 4), hot-cue
  flags, cue point, transition-entry marker, lane-click seek, wheel/± zoom
  4..30 s (default 8 s), ~30 Hz repaint while playing. Row 3 = compact
  horizontal MixerWidget (≤110 px: inline TRIM/HI/MID/LOW knobs + 64 px
  fader per channel, 200 px center crossfader) side by side with the
  TransitionPanel. Row 4 = library. Semantics: hot-cue pads and CUE now
  dispatch on pressed() with value 1.0 and released() with 0.0 (engine owns
  hold-to-preview); PLAY stays a checkable toggled() wire; all actions
  still flow through the bus with the QSignalBlocker mirror pattern;
  right-/shift-click still clears a hotcue. New public slot
  `MainWindow::setTransitionEntryMarker(int deck, double sec)` (sec<0 =
  clear) relays to both deck overviews and the detail view — orchestrator:
  call this from the entry-point logic. TransitionPanel.cpp untouched.
  Verified: full build clean, all 4 UI TUs warning-clean (-Wall -Wextra),
  GUI launches. NOTE: `--selftest` currently fails ("player STILL ACTIVE")
  on the fresh build but passes on the pre-existing binary; selftest has
  zero src/ui dependency, so this is the concurrent audio cue-semantics
  work in flight, not the UI restructure.

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
| analysis/{TrackData,BeatAnalyzer}.cpp, library/*.cpp | claude-analysis | DONE — decode/tags/fingerprint/beatgrid + library model/cache + TransitionStore (incl. gvt::matchTrack). Demo Track 1 → 128.0 BPM, Demo Track 2 → 120.0 BPM. tests/test_beats.cpp + tests/test_fingerprint.cpp pass standalone; ctest link pending peers' objects. 2026-08-15 (claude-analysis2): added Serato-style band waveform data — `TrackData::overviewLow/Mid/High` (per-512-frame-bin peak abs of one-pole-filtered mono: low <200 Hz, mid 200–2000, high >2000; all three normalized by one shared global max, always sized like overviewPeaks, silent→zeros/no NaN). Computed in loadAndAnalyzeTrack and recomputed from PCM on library cache hits via `detail::computeBandOverviews` (AnalysisInternal.h) — NOT stored in the JSON cache, cache format unchanged. Verified: TrackData.cpp.o + TrackLibrary.cpp.o compile warning-clean; standalone probe on Demo Track 1 prints 16166 bins per band, non-zero, sizes equal to overviewPeaks. |
| transitions/{GvtFormat,TransitionRecorder,TransitionPlayer}.cpp, tests | claude-transitions | DONE |
| midi/{MidiEngine,Flx4Mapping}.cpp | codex-midi | DONE |
| ui/*.cpp, app/* | claude-ui / claude-ui2 | DONE — Serato-parity restructure 2026-08-15 (see Current state above): equal deck halves + outer tempo sliders, new DetailWaveformView center lanes, horizontal mixer strip, press/release Cue + hot-cue dispatch, MainWindow::setTransitionEntryMarker. Original notes:  ui/{MainWindow,DeckWidget,MixerWidget,LibraryWidget,TransitionPanel,Theme}.h+cpp, app/main.cpp, app/SelfTest.h (declares `gvt::runSelfTest`; SelfTest.cpp is the orchestrator's). All 6 TUs + moc outputs compile warning-clean (-Wall -Wextra). Notes: (1) pinned TrackLibrary.h/TransitionEngine.h pImpl classes (TransitionStore/Recorder/Player) declare no destructor, so any TU destroying them fails to compile — main.cpp heap-allocates them with process lifetime as a workaround; header owners should add declared dtors (moc/mocs_compilation may hit the same issue). (2) `gvt::transitionPlayerSetMode` doesn't exist and `arm()` has no PlayerMode arg, so the TUTORIAL button is disabled ("coming soon"); tutorialPrompt/tutorialScored signals are wired (banner + accuracy toasts) and light up once the player emits them. (3) Hotcue clear writes `track->hotCues[i] = -1` directly (no Deck clear API). (4) UI mirrors Play state by polling `deck.playing` at 30 Hz; continuous controls mirror via bus events with QSignalBlocker. |
| integration/selftest | orchestrator | DONE — see Verification below |

### DDJ-FLX4 MIDI messages mapped

`hh` is velocity, `mm` is the 7-bit MSB, and `ll` is the 7-bit LSB.
The FLX4 sends each 14-bit control MSB first, then LSB; Gravitino dispatches
the completed value on receipt of the LSB. Deck A/B below are ControlBus deck
0/1 respectively.

| Control | Deck | MIDI input (hex) | ControlBus mapping / MIDI LED output |
|---|---:|---|---|
| PLAY/PAUSE | A / B | `90 0B hh` / `91 0B hh` | Nonzero NOTE ON toggles `Play`/`Stop`; velocity-zero NOTE ON and NOTE OFF are ignored; LED `90 0B 7F/00` / `91 0B 7F/00` |
| Deck CUE | A / B | `90 0C hh` / `91 0C hh` | Nonzero NOTE ON → `Cue=1`; velocity-zero NOTE ON or `80/81 0C hh` NOTE OFF → `Cue=0`; LED is on while `cuePointSec >= 0` |
| BEAT SYNC | A / B | `90 58 hh` / `91 58 hh` | Nonzero press → `TempoSync` |
| Hot-cue pads 1–8 (HOT CUE mode) | A | `97 00..07 hh` | Nonzero NOTE ON → `HotCue1..8=1`; velocity-zero NOTE ON or NOTE OFF → `HotCue1..8=0`; LED `97 00..07 7F/00` |
| Hot-cue pads 1–8 (HOT CUE mode) | B | `99 00..07 hh` | Nonzero NOTE ON → `HotCue1..8=1`; velocity-zero NOTE ON or NOTE OFF → `HotCue1..8=0`; LED `99 00..07 7F/00` |
| TEMPO slider, 14-bit | A / B | `B0/B1 00 mm`, then `B0/B1 20 ll` | Raw `0000`/`2000`/`3FFF` → ratio `0.92`/`1.00`/`1.08`; physical `+`/down/toward-user end is faster |
| Channel fader, 14-bit | A / B | `B0/B1 13 mm`, then `B0/B1 33 ll` | `Fader = raw / 3FFF` |
| TRIM, 14-bit | A / B | `B0/B1 04 mm`, then `B0/B1 24 ll` | `Trim = raw / 3FFF` |
| EQ HI, 14-bit | A / B | `B0/B1 07 mm`, then `B0/B1 27 ll` | `EqHigh = raw / 3FFF` |
| EQ MID, 14-bit | A / B | `B0/B1 0B mm`, then `B0/B1 2B ll` | `EqMid = raw / 3FFF` |
| EQ LOW, 14-bit | A / B | `B0/B1 0F mm`, then `B0/B1 2F ll` | `EqLow = raw / 3FFF` |
| Crossfader, 14-bit | Global | `B6 1F mm`, then `B6 3F ll` | `Crossfader = raw / 3FFF`; `0000` = full left/deck A, `3FFF` = full right/deck B |
| Jog side / platter (vinyl on / vinyl off) | A / B | `B0/B1 21/22/23 vv` | `Jog = signed(vv - 40)` ticks; `41` = +1 clockwise, `3F` = -1 counterclockwise, `40` ignored; all modes nudge for MVP |

PLAY and BEAT SYNC releases remain press-only and are ignored. CUE and hot-cue
releases are dispatched so hold-preview and future pad-release behavior reach
the deck. LED state is sent only for non-MIDI-origin bus events to prevent
feedback loops; cached play, cue-point, and hot-cue state is reconciled against
the engine and restored when the output port reconnects.

### Transport semantics

- Loading a track stops it, rewinds it, and initializes the deck cue point to
  `firstBeatSec`.
- CUE while playing stops and returns to the deck cue point (falling back to
  `firstBeatSec` only if no cue is set). While paused, CUE within 50 ms of the
  cue point previews for as long as the button is held, returning on release;
  pressing CUE elsewhere stores the current position without starting playback.
- Pressing an unset hot cue stores the current position. Pressing a set hot cue
  jumps to it and starts playback if the deck was paused. Hot-cue release is a
  dispatched no-op in this pass.

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

## Adversarial review (2026-08-15, Claude + Codex independently)

Both reviewers converged on the same top defects; all fixed and re-verified
(ctest 4/4, --selftest green, realtime repro of the hang now passes):

- FIXED: TransitionPlayer froze forever when the from-deck stopped
  mid-transition (beat clock had no wall-clock fallback; confirmed by repro).
- FIXED: crossfader recorded in physical space broke B→A replays — .gvt
  xfader values are now ROLE space (0 = outgoing, 1 = incoming), mirrored on
  record/replay/glide-start when fromDeck == 1.
- FIXED: Deck track-swap handshake was Dekker's pattern with acq/rel only —
  added seq_cst fences (loadTrack/render/~Deck) to close a PCM use-after-free.
- FIXED: recorder's rolling coalescing window could swallow a slow continuous
  gesture into one snap — now fixed 0.05-beat buckets, negative deltas kept.
- FIXED: jog ticks were recorded as absolute glides (replayed as a stuck
  pitch bend) — jog is no longer recorded.
- FIXED: play-after-end-of-track did nothing (position never rewound).
- FIXED: TempoSync could store a ratio outside the range render() honors.
- FIXED: FLX4 play LED / play-toggle desync after deck self-stop at EOF —
  MidiEngine now reconciles cached transport state against the engine at
  250 ms and toggles against engine truth; RtMidi objects are destroyed
  first in ~Impl so late CoreMIDI callbacks can't touch freed members.
- FIXED: recorder captured the TO-deck anchor late if that deck was already
  playing at record start; TransitionPlayer mode side-table now erased on
  destruction (heap-reuse could inherit Tutorial mode).
- Deferred (documented): fast unplug/replug with identical port name isn't
  re-opened until the next state change (endpoint identity, not name, is the
  right key); full drain-counter handshake for MIDI teardown; EQ "kill" is
  -26 dB not -inf (ARCHITECTURE.md wording).

## TODO backlog (post-MVP)

- Fill the deck dead space (below hot cues) with loop / FX / beat-jump
  sections as we chase Serato feature parity — the layout was deliberately
  densified (11px base font, compact pads/knobs) to leave room for this.
- Keylock (signalsmith-stretch), variable beatgrids, WAV/FLAC/AAC, transition
  marketplace/sharing UI, Windows/Linux builds, .app bundling + macdeployqt.
