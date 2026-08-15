# STATUS — agent handoff board

> Keep this file current. When you finish or abandon work, update your section.
> An agent with zero prior context must be able to resume from this file plus
> docs/ARCHITECTURE.md and docs/TRANSITION_FORMAT.md.

## Current state (update the date/line when you change things)

- 2026-08-16 (codex-fx): **Per-deck RT-safe FX insert + DDJ-FLX4 Beat FX
  mapping complete.** `Deck::render` now runs FX after the DJ filter and before
  the channel fader. New preallocated `DeckFx` provides: stereo 0.5-feedback
  beat/BPM-timed echo (1 ms..2 s, 2 s x 48 kHz ring per deck), small stereo
  Schroeder reverb (four detuned ~29.7/37.1/41.1/43.7 ms feedback combs into
  5/1.7 ms allpasses), and beat-period stereo flanger (0.5..8 ms, feedback
  0.6). Wet uses a smoothed equal-power crossfade. Disengaging mutes only the
  effect input and restores unity dry while the stored tail decays; stopped
  decks continue tail-only zero-input renders without advancing position, then
  return to hard bypass. A render gate extends the existing track-swap drain
  handshake to tail-only callbacks. `AudioEngine::applyEvent` clamps/stores all
  four pinned Fx controls. FLX4 shared Beat FX controls map from the published
  messages: assign notes target deck A/B (both until the controller first
  reports assignment), SELECT cycles echo/reverb/flanger, BEAT </> halves or
  doubles 0.25..4 beats, 14-bit LEVEL/DEPTH sets wet, ON/OFF toggles each
  assigned deck against its atomic state, and `94/95 47` drives the ON LED.
  Verified all requested objects and the full link warning-clean with
  `-Wall -Wextra`; ctest 5/5 and `--selftest` pass; raw MIDI parser/LED proof
  passed. Standalone real-track echo proof (Demo Track 1,
  engaged 2 s at wet 0.6, then off + transport stop) tail RMS per 0.25 s:
  `0.39826874, 0.19868438, 0.09976300, 0.04935670, 0.02389289, 0.01145023,
  0.00551588, 0.00254754` (asserted non-silent through 0.5 s and final <10% of
  initial). Touched only assigned audio/MIDI files, CMake add-only, and STATUS.

- 2026-08-15 (claude-lib4): **Per-deck FX strip UI + Serato-style library
  crates sidebar + History tab.** DeckWidget gains a compact ~20 px FX row
  below the loop/jump row: FX [ECHO|REVERB|FLANGER] QComboBox (dispatches
  FxType index on activation), checkable ON button (FxOn 1/0 on toggled,
  deck-accent when engaged), 20 px WET dial (FxWet 0..1), and BEATS
  [<][label][>] halving/doubling the deck's fxBeats within 0.25..4
  (dispatches FxBeats with the new value; label shows "1/4".."4"). All
  Origin::Ui through the bus; the 30 Hz refresh mirrors fxType/fxOn/fxWet/
  fxBeats atomics under QSignalBlocker (syncFxControls, restyle only on
  change). LibraryWidget restructured: left crate sidebar in a collapsible
  QSplitter (~170 px start, min 90) — "All Tracks (N)" plus one crate per
  immediate subdirectory (recursive track counts) of the crate ROOT, which
  is derived as the deepest common directory of all pathAt() values (so no
  TrackLibrary change; tracks sitting directly in the root appear only
  under All Tracks); rebuilds on modelReset/rowsInserted/rowsRemoved,
  preserving the selected crate. Selecting a crate path-prefix-filters the
  table via the new CrateFilterProxy (QSortFilterProxyModel: crate prefix
  AND title/artist search compose; uses begin/endFilterChange — Qt >= 6.10).
  Right-aligned segmented [Library][History] tabs sit in the chrome row
  above the table and switch a QStackedWidget; the History page is a
  QTableView over HistoryModel (in LibraryWidget.cpp, newest first;
  columns Loaded/Title/Artist/BPM/Key/Deck) fed by new gvt::History
  (src/library/History.{h,cpp}, added to gvtcore in CMakeLists —
  add-only). History persists JSON-lines at ~/.gravitino/history.jsonl
  (one compact object per line: startedAt ISO-8601 local, deck 0/1, title,
  artist, bpm, key; appended on each load, last 500 loaded on start,
  malformed lines skipped). MainWindow constructs History as a child and
  logs from notifyTrackLoaded (covers both library loads and --autoload;
  main.cpp untouched); LibraryWidget ctor gained `History* history =
  nullptr` before `parent` — when null the History tab is hidden (old
  callers compile unchanged). Verified: full build + link warning-clean in
  build-lib4, ctest 5/5, GUI launch screenshot shows FX rows / sidebar /
  tabs, --autoload writes two history lines end-to-end. `--selftest` still
  shows the pre-existing concurrent "player STILL ACTIVE" failure (no
  src/ui or library dependency). Touched only src/ui/*, new
  src/library/History.{h,cpp}, CMakeLists (add-only), this file.

- 2026-08-15 (codex-audio3): **Loops, beat jump, DJ filter, master tap, and
  FLX4 controls implemented in the engine.** Auto loops floor-snap to the
  current native-grid beat (1/8..64 beats); manual IN/OUT snap to 1/8 beat;
  active loops wrap per rendered frame under tempo resampling; half/double use
  native track BPM. Beat jump rounds the current grid beat before applying the
  signed offset and preserves play state. External seeks outside an active
  loop deactivate it but retain its bounds. The post-EQ bipolar filter is a
  second-order log-swept LPF/HPF with a 0.47..0.53 true-bypass dead-zone.
  MasterRecorder is fed once per completed mix buffer after soft clipping.
  FLX4 loop-section notes, Beat Jump pads, channel filter CCs, and active-loop
  LEDs are mapped below.

- 2026-08-15 (claude-ui3): **Loops/beat-jump UI, filter knobs, colored
  overview, key display, master-record button.** DeckWidget gains a compact
  single-row loop/beat-jump section below the hot cues (LOOP [1/2][1][2][4][8]
  auto · [IN][OUT][EXIT] · [<½][2×>] · JUMP [◀8][◀4][◀1][1▶][4▶][8▶]); all
  fire on pressed() via bus->dispatch(Origin::Ui) using
  LoopAuto/LoopIn/LoopOut/LoopExit/LoopHalve/LoopDouble/BeatJump. The 30 Hz
  refresh highlights (deck-accent) the matching auto-loop length button
  (beats ≈ (end−start)·bpm/60, ±20% match) plus IN (pending start set) and
  OUT (loop active). Loop region [loopStartSec,loopEndSec] is shaded in the
  deck accent (~25% alpha active / dimmer when stored-but-inactive, brighter
  edge lines) in BOTH the deck overview and the DetailWaveformView lanes;
  the detail view's dirty check now watches the loop atomics so edits
  repaint while paused. Deck overview waveform is now band-colored from
  overviewLow/Mid/High (Theme.h waveLow/Mid/HighColor, unplayed part drawn
  darker(190); gray/accent fallback when band vectors are empty). MixerWidget
  strips gain a FILTER QDial after LOW (0..1, starts 0.5) dispatching
  ControlId::Filter, mirrored from the bus under QSignalBlocker like the
  other knobs; caption turns cyan <0.47 (LPF) / orange >0.53 (HPF) / gray at
  center; double-click re-centers to 0.5 (event filter — the reset
  dispatches like a user turn). Deck info line shows TrackData::camelotKey
  before BPM ("8A · BPM 128.00 → 128.00"), re-checked each refresh tick so
  async key analysis appears when ready. MainWindow now takes an ADDITIONAL
  `MasterRecorder* rec = nullptr` parameter (before `parent`; existing
  callers compile unchanged): when non-null, a "● REC MASTER" toggle button
  sits on the status-bar LEFT — click starts rec->start("", &err) (error
  shown in the status bar) / stops; state follows recordingChanged (red
  button, text becomes "● MM:SS" elapsed via a 1 s timer using
  recordedSec()); when rec == nullptr the button is never created.
  Orchestrator: pass the recorder as the 8th MainWindow ctor arg and set
  engine->masterTap. Verified: full build + link clean in build-ui3, all UI
  TUs warning-clean; `--selftest` still shows the pre-existing concurrent
  "player STILL ACTIVE" failure (no src/ui dependency). Only src/ui/* +
  this file touched.

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
| analysis/{TrackData,BeatAnalyzer}.cpp, library/*.cpp | claude-analysis | DONE — decode/tags/fingerprint/beatgrid + library model/cache + TransitionStore (incl. gvt::matchTrack). Demo Track 1 → 128.0 BPM, Demo Track 2 → 120.0 BPM. tests/test_beats.cpp + tests/test_fingerprint.cpp pass standalone; ctest link pending peers' objects. 2026-08-15 (claude-analysis2): added Serato-style band waveform data — `TrackData::overviewLow/Mid/High` (per-512-frame-bin peak abs of one-pole-filtered mono: low <200 Hz, mid 200–2000, high >2000; all three normalized by one shared global max, always sized like overviewPeaks, silent→zeros/no NaN). Computed in loadAndAnalyzeTrack and recomputed from PCM on library cache hits via `detail::computeBandOverviews` (AnalysisInternal.h) — NOT stored in the JSON cache, cache format unchanged. Verified: TrackData.cpp.o + TrackLibrary.cpp.o compile warning-clean; standalone probe on Demo Track 1 prints 16166 bins per band, non-zero, sizes equal to overviewPeaks. 2026-08-15 (claude-key): musical key detection added — new src/analysis/KeyAnalyzer.{h,cpp} (`gvt::analyzeKey`: mono downmix, 4x decimate to 12 kHz, Hann+Goertzel chromagram C3..B6 with log compression + per-frame L1 norm over first 120 s, Krumhansl-Schmuckler major/minor correlation, Camelot mapping; ~0.4 s/track; empty fields if corr < 0.5 or degenerate). loadAndAnalyzeTrack fills `TrackData::camelotKey/keyName`; TrackLibrary caches both in the JSON and treats a missing "camelotKey" field as a cache MISS (pre-feature entries re-analyze once); model gained a Key column between BPM and Duration (shows camelotKey, right-aligned). KeyAnalyzer.cpp added to gvtcore in CMakeLists. Verified: all 4 TUs compile warning-clean (-Wall -Wextra); standalone probe detects — Can't Stop the Feeling: 8B/C (matches known C major), Demo Track 1: 7B/F, Demo Track 2: 4A/Fm, Pink Venom: 12B/E (published key is 1A/G#m — detector picks the neighboring E major, which shares 6 of 7 scale tones; known limitation). |
| transitions/{GvtFormat,TransitionRecorder,TransitionPlayer}.cpp, tests | claude-transitions | DONE |
| midi/{MidiEngine,Flx4Mapping}.cpp | codex-midi | DONE |
| ui/*.cpp, app/*, library/History.* | claude-ui / claude-ui2 / claude-ui3 / claude-lib4 | DONE — 2026-08-15 (claude-lib4): per-deck FX strip, crate sidebar + Library/History tabs, gvt::History JSONL log (see Current state above). 2026-08-15 (claude-ui3): loop/beat-jump row, loop-region shading (overview + detail lanes), FILTER knobs, band-colored deck overview, camelot key in the info line, MainWindow MasterRecorder* param + status-bar "● REC MASTER" button (see Current state above). Serato-parity restructure 2026-08-15 (see Current state above): equal deck halves + outer tempo sliders, new DetailWaveformView center lanes, horizontal mixer strip, press/release Cue + hot-cue dispatch, MainWindow::setTransitionEntryMarker. Original notes:  ui/{MainWindow,DeckWidget,MixerWidget,LibraryWidget,TransitionPanel,Theme}.h+cpp, app/main.cpp, app/SelfTest.h (declares `gvt::runSelfTest`; SelfTest.cpp is the orchestrator's). All 6 TUs + moc outputs compile warning-clean (-Wall -Wextra). Notes: (1) pinned TrackLibrary.h/TransitionEngine.h pImpl classes (TransitionStore/Recorder/Player) declare no destructor, so any TU destroying them fails to compile — main.cpp heap-allocates them with process lifetime as a workaround; header owners should add declared dtors (moc/mocs_compilation may hit the same issue). (2) `gvt::transitionPlayerSetMode` doesn't exist and `arm()` has no PlayerMode arg, so the TUTORIAL button is disabled ("coming soon"); tutorialPrompt/tutorialScored signals are wired (banner + accuracy toasts) and light up once the player emits them. (3) Hotcue clear writes `track->hotCues[i] = -1` directly (no Deck clear API). (4) UI mirrors Play state by polling `deck.playing` at 30 Hz; continuous controls mirror via bus events with QSignalBlocker. |
| integration/selftest | orchestrator | DONE — see Verification below |
| audio/MasterRecorder.cpp (+ pinned .h), tests/test_masterrec.cpp | claude-recmaster | DONE 2026-08-15 — master WAV recorder: feed() is audio-thread lock-free (SPSC ring, 2^18 floats ≈ 2.7 s, atomic head/tail acq/rel; full ring drops the whole chunk + atomic counter, qWarning once on stop). Worker std::thread drains every 10 ms → 16-bit LE PCM; start("") resolves ~/Music/Gravitino/Recordings/gravitino-YYYYMMDD-HHMMSS.wav (dirs created), placeholder 44-byte header (16-bit/2ch/48k) patched with RIFF/data sizes on stop(). stop() clears the atomic 'active' gate BEFORE joining, so concurrent feed() at worst writes into the still-allocated ring. recordedSec() from atomic frame counter; recordingChanged(bool,path) emitted on start/stop; dtor stops cleanly. Added to gvtcore in CMakeLists (add-only). Verified: MasterRecorder.cpp.o compiles clean in build-rec; test_masterrec built standalone (MasterRecorder.cpp + moc + QtCore, -Wall -Wextra clean) and passes: "test_masterrec OK: 2.000 s recorded, 384000 data bytes, peak 16384" (2 s of 440 Hz sine, header validated 16-bit/2ch/48k, data size exact, 0 drops). ctest picks it up via the tests glob once peers' objects link. NOT yet wired into AudioEngine/UI — orchestrator: call feed() from the master render output after the limiter. |

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
| FILTER, 14-bit | A | `B6 17 mm`, then `B6 37 ll` | `Filter = raw / 3FFF`; center `2000` is bypass, left is LPF, right is HPF |
| FILTER, 14-bit | B | `B6 18 mm`, then `B6 38 ll` | `Filter = raw / 3FFF`; center `2000` is bypass, left is LPF, right is HPF |
| Crossfader, 14-bit | Global | `B6 1F mm`, then `B6 3F ll` | `Crossfader = raw / 3FFF`; `0000` = full left/deck A, `3FFF` = full right/deck B |
| Jog side / platter (vinyl on / vinyl off) | A / B | `B0/B1 21/22/23 vv` | `Jog = signed(vv - 40)` ticks; `41` = +1 clockwise, `3F` = -1 counterclockwise, `40` ignored; all modes nudge for MVP |
| LOOP IN | A / B | `90/91 10 hh` | Nonzero press → `LoopIn`; active-loop LED output `90/91 10 7F/00` |
| LOOP OUT | A / B | `90/91 11 hh` | Nonzero press → `LoopOut`; active-loop LED output `90/91 11 7F/00` |
| 4 BEAT / EXIT | A / B | `90/91 4D hh` | Nonzero press → `LoopAuto=4.0` when inactive, `LoopExit` when active; LED `90/91 4D 7F/00` |
| SHIFT + 4 BEAT / EXIT | A / B | `90/91 50 hh` | Nonzero press → `LoopExit`; modifier-state LED mirrors active state |
| CUE/LOOP CALL left / right | A / B | `90/91 51 hh` / `90/91 53 hh` | Nonzero press → `LoopHalve` / `LoopDouble` |
| SHIFT + LOOP IN / OUT (1/2X / 2X alternate) | A / B | `90/91 4C hh` / `90/91 4E hh` | Nonzero press → `LoopHalve` / `LoopDouble`; modifier-state LEDs mirror active state |
| Beat-jump pads 1–8 (BEAT JUMP mode) | A / B | `97/99 20..27 hh` | Nonzero presses → `BeatJump=-1,+1,-2,+2,-4,+4,-8,+8`; releases ignored |
| BEAT FX channel assign | A / B | `94 10 hh` / `95 11 hh` | ON/OFF assignment state selects which deck(s) receive the shared Beat FX controls; before the first assignment report, controls target both decks |
| BEAT FX SELECT / SHIFT+SELECT | Assigned | `94 63 hh` / `94 64 hh` | Nonzero press cycles `FxType` next / previous modulo echo, reverb, flanger |
| BEAT FX BEAT left / right | Assigned | `94 4A hh` / `94 4B hh` | Nonzero press dispatches current `FxBeats * 0.5` / `* 2.0`, clamped to 0.25..4 |
| BEAT FX LEVEL/DEPTH, 14-bit | Assigned | `B4 02 mm`, then `B4 22 ll` | `FxWet = raw / 3FFF` for every assigned deck |
| BEAT FX ON/OFF | Assigned | `94/95 47 hh` | Nonzero press reads each assigned deck's `fxOn` atomic and dispatches the inverse; LED output `94 47 7F/00` for A and `95 47 7F/00` for B |

PLAY and BEAT SYNC releases remain press-only and are ignored. CUE and hot-cue
releases are dispatched so hold-preview and future pad-release behavior reach
the deck. Play/cue/hot-cue LED state is sent for non-MIDI-origin bus events;
loop LEDs also update after MIDI-origin state changes because the combined
4 BEAT/EXIT action is host-resolved. Cached play, cue-point, loop-active, and
hot-cue state is reconciled against the engine and restored on output reconnect.
FX ON state is likewise reconciled and restored using the assignment-specific
Beat FX LED messages.

### Per-deck FX semantics

- Insert order is trim → EQ → DJ filter → FX → channel fader. Master
  crossfader/limiter remain downstream in `AudioEngine`.
- Echo delay is `fxBeats * 60 / effectiveBpm`, recomputed per render chunk and
  smoothed when tempo or beats changes. Reverb uses beats only as a stored UI/
  transition parameter; flanger LFO period is `fxBeats * 4` beats.
- `fxWet` is a smoothed equal-power dry/wet crossfade while engaged. On
  disengage, effect input becomes zero immediately, dry returns smoothly to
  unity, and echo/reverb/flanger state continues until its energy is silent.
- FX type changes clear all effect state. All delay storage is fixed-size in
  `Deck::Impl`; `render()` performs no allocation or locking.

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
- Deck DSP uses linear tempo/jog resampling, RBJ three-band EQ, a post-filter
  per-deck FX insert, equal-power master crossfade, and a shared
  realtime/offline `tanh` soft-clip mix path.
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

## Serato-parity roadmap (user goal: match Serato DJ Pro)

Round 2 (2026-08-15 afternoon) DONE: 50/50 deck halves with mirrored outer
tempo sliders, full-width stacked zoomed band-colored waveforms (fixed center
playhead, wheel/buttons zoom 4–30 s), horizontal mixer strip, Serato cue
semantics (set/return/hold-preview), hot cues fire on press with engine-side
set/jump, FLX4 note-off releases, transition ENTRY POINT: PERFORM/TUTORIAL
seek to the recorded anchor beat, new ⚡ PRIME arms loop-style firing when
playback crosses the anchor, orange "T" markers on all waveforms.

Round 4 (2026-08-16) DONE: per-deck FX insert (beat-synced ECHO with
ring-out tail, Schroeder REVERB, FLANGER; post-filter pre-fader; fully
recordable into .gvt via fx_* controls), FX strip UI per deck, FLX4 BEAT FX
mapping, library crate sidebar (one crate per music subfolder), [Library]
[History] tabs with persistent play log (~/.gravitino/history.jsonl), and a
deterministic selftest (the old "player STILL ACTIVE" flake was the offline
renderer outrunning the player's wall-clock finish window).

Next parity targets (not started): sampler pads, pitch-fader value
readouts, 4-deck mode, iTunes import, playlist editing (crates are
currently folder-derived, read-only). DONE since: loops, beat jump, FX
section (engine + per-deck UI strip), colored overview waveforms, key
detection/display, master recording, folder-crate sidebar, history panel.

## TODO backlog (post-MVP)

- Fill the deck dead space (below hot cues) with loop / FX / beat-jump
  sections as we chase Serato feature parity — the layout was deliberately
  densified (11px base font, compact pads/knobs) to leave room for this.
- Keylock (signalsmith-stretch), variable beatgrids, WAV/FLAC/AAC, transition
  marketplace/sharing UI, Windows/Linux builds, .app bundling + macdeployqt.
