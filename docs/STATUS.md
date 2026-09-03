# STATUS — agent handoff board

> Keep this file current. When you finish or abandon work, update your section.
> An agent with zero prior context must be able to resume from this file plus
> docs/ARCHITECTURE.md and docs/TRANSITION_FORMAT.md.

## Current state (update the date/line when you change things)

- 2026-09-04 (codex): **Non-destructive analysis-cache migration and harmonic
  BPM fix.** Cached detector output is now stored separately from the effective
  beat grid with an explicit source marker. Schema/analysis upgrades merge
  around protected grids, hot cues, and saved loops; unmarked historical
  records are protected conservatively. Beat analysis now evaluates 3:2 and
  4:3 candidates in addition to octave aliases, correcting the observed Baby,
  Turn Down for What, Party In The U.S.A., and Yeah! detections against the
  actual local audio. A regression test forces an old-cache upgrade and checks
  that its manual grid and performance metadata survive. No transition-file
  schema or replay semantics changed.

- 2026-09-04 (codex): **Side-by-side legacy/portable migration testing.** The
  Transitions tab now has independent `.gvt` and `.transition` checkboxes,
  both enabled by default, and retains each source as a distinct filterable
  row. `--convert-transitions` atomically creates only missing portable copies,
  records their stable legacy source identities, never overwrites `.gvt`, and
  is idempotent. All 28 existing legacy transitions in
  `~/Music/Gravitino/Transitions` were converted successfully: the directory
  now contains 28 `.gvt` and 28 `.transition` files. Verified with all 27
  CTest targets, the full offline `--selftest`, `git diff --check`, an
  idempotence rerun that created zero additional files, and a live compact-view
  check confirming the bottom filter row remains visible.

- 2026-09-04 (codex): **Portable `.transition` YAML, arrangement catalog, and
  semantic cues.** New recordings use deterministic safe-subset YAML with
  typed endpoint evidence, canonical fractional beat coordinates, exact
  timeline/initial state, capability requirements, unknown-field preservation,
  and transition-owned cues. Legacy `.gvt` remains readable; editing creates a
  separate UUID-backed portable copy. The local catalog groups MP3/FLAC/WAV/
  AIFF assets by encode-tolerant structure plus musical assumptions, retains
  confirmed bindings/constant beat offsets, and rebuilds song↔transition edges
  without touching audio tags. Portable matching prioritizes confirmed
  bindings, structural fingerprints, and checked ISRC/MusicBrainz IDs;
  metadata-only matches require confirmation. Semantic cues occupy an isolated
  temporary CUSTOM bank and never overwrite permanent track cues. Verification
  passed with a clean Release configure/build, all 27 CTest targets, the full
  offline `--selftest`, and `git diff --check`. The codec test covers WAV,
  FLAC, 96 kbps MP3, AIFF, plus a gain-reduced 44.1 kHz FLAC with 750 ms of
  leading silence (0.996 structural similarity). A live macOS smoke check
  confirmed the Human event table and status-bar library-toggle placement.

- 2026-09-03 (codex): **Mouse-only hot-cue latch and platter.** Every deck now
  shows a rotating 33⅓-RPM-positioned disc. Dragging it in either direction
  emits the same touch-gated scratch controls as the FLX4, allowing precise
  positioning and resuming playback on release when it was previously
  running. A mapped HOT CUE can be held with the left mouse button, dragged to
  the highlighted PLAY target, and released to reproduce the controller's
  HOT CUE + PLAY latch. Ordinary release over the pad remains momentary. The
  UI regression test sends real mouse events through both interactions and
  checks preview, latch, scratch movement, and transport restoration.
  Verified: clean build, ctest 23/23, full `--selftest`, and
  `git diff --check`.
  Follow-up: mouse platter sensitivity is intentionally fine-only. On a
  playing deck it now applies direct millisecond-scale phase correction without
  pausing, including during automated transition replay. It adjusts about
  45 ms per quarter turn / 180 ms per full turn and leaves paused decks paused.
  FLX4 touch/scratch behavior is unchanged.

- 2026-09-03 (codex): **Permanent right-side library toggle.** The Show/Hide
  Library button now sits flush right immediately before the controller
  connection status. It is a permanent status-bar widget, so opening Tutor
  View or any other transient message cannot cover it. The offscreen UI test
  checks both its ordering and its visibility while a message is active.

- 2026-09-03 (codex): **Row-level transition timing guidance.** During both
  Human and Raw Perform/Tutorial runs, the Event Sequence overlays a
  translucent progress fill on the most recently reached row and completes it
  exactly when the next distinct action is due. Equal-beat actions are treated
  as simultaneous instead of receiving fake countdown time. Every sequence
  now begins with a derived beat-zero `Transition starts — no action` row,
  giving the first real action the same visual runway; it cannot be labeled or
  replayed and is never persisted to `.gvt`. Focused tests cover timing math,
  both table modes, the synthetic row, and live mode switching. Verified:
  clean build, ctest 23/23, full `--selftest`, and `git diff --check`.

- 2026-09-03 (codex): **Human event sequence and compact library control.**
  The library Show/Hide button now shares the status bar with REC MASTER,
  reclaiming the lower workspace footer. Event Sequence defaults to a
  five-column Human view with track-relative beats, role-separated actions,
  values folded into readable instructions, stream-aware continuous ranges,
  overlap pairing, and HOT CUE → PLAY → release launch recognition. HUMAN/RAW
  switches views without changing replay data; Raw preserves the previous
  table and every Human row retains its source indices for tutorial
  highlighting and cue selection. The `.gvt` v1 format is unchanged. Verified:
  clean build, ctest 23/23 (including new summary and offscreen widget-layout
  coverage), full `--selftest`, and `git diff --check`. The macOS Computer Use
  bridge still times out while reading the live Qt hierarchy, so the automated
  widget assertions replace—not claim—a screenshot-level visual inspection.

- 2026-09-03 (codex): **Corrected Tutor placement, library toggle, and `.gvt`
  editing.** TUTOR VIEW now opens a large virtual FLX4 only in the lower-left
  workspace, below the full-width detailed waveform. The deck/mixer and
  waveform regions remain unchanged; only the right-hand stack of transition
  controls, event sequence, and library narrows. The library stays available
  during Tutor and has a persistent Show/Hide control, subsequently moved into
  the status bar to save space. Transition rows
  expose Edit/Rename/Delete on right click with confirmation before deletion,
  while the event-sequence header has a matching Edit Transition button. The
  editor validates the plain UTF-8 source before using the existing managed
  store update/rename paths. `TRANSITION_FORMAT.md` now documents the v1
  sharing boundary: tracks are not embedded, HOT CUE/CUSTOM setup can remain
  machine-local, and beatgrid/stem differences matter. Verified: clean build,
  ctest 21/21, full `--selftest`, and `git diff --check`. The macOS Computer
  Use bridge timed out while reading the large live Qt hierarchy, so a final
  human visual smoke test remains appropriate.

- 2026-08-26 (codex): **Compact transition sequence and large Tutor layout.**
  PRIME no longer checks or prepares the live crossfader; the recorded mixer
  value remains a beat-zero replay action. Selecting a transition now leaves
  yellow waveform event markers clear until the DJ explicitly selects an
  event row. The visible sequence summarizes each outgoing/incoming EQ band
  and crossfader move to start/end while retaining every checkpoint for exact
  replay. Library > Transitions is the canonical edge picker, with Rename and
  Delete beside `Search transition edges…`; the redundant matching list is no
  longer rendered, and the event sequence is a permanent full-width lower
  panel. This first placement was superseded on 2026-09-03: Tutor now occupies
  the lower-left workspace without changing the deck row or waveform. Its next
  summarized row is highlighted gold and scrolled into view. Normal deck
  controls place the narrowed pads/mode column beside compact
  Loop/Jump, FX, and Stems rows. Verified: clean build, ctest 21/21, full
  `--selftest`, and `git diff --check`. Live Computer Use inspection remains
  pending because the Mac was locked.

- 2026-08-22 (codex): **PRIME reachability and takeover lifecycle fix.** A
  future outgoing loop no longer blocks PRIME merely because the transition
  entry lies before LOOP IN; readiness now asks whether the live transport can
  actually reach the entry. This fixes the saved Titanium → Don't You Worry
  Child topology (current beat before entry, loop at beats 448–464). Failed or
  aborted PRIME/PERFORM attempts now cancel provisional FLX4 tracking instead
  of falsely entering `FLX4 INPUT FROZEN`; pickup is finalized only after a
  genuinely completed transition. PRIME failures also remain visible as a
  persistent, actionable `PRIME not armed` reason instead of silently
  disappearing. Verified: clean build, ctest 21/21, full `--selftest`, and
  `git diff --check`.

- 2026-08-22 (codex): **Focused beat grid and synchronized overview cursor.**
  Stacked/detail waveforms now draw ordinary beat lines at substantially
  higher opacity and distinguish every four-beat downbeat with a brighter
  1.8 px line. Compact overview waveforms intentionally contain no beat lines.
  Their playhead now follows every position change at 30 Hz even while paused,
  including Beat Jump and detailed-waveform drag/scratch navigation. Verified:
  clean build, ctest 21/21, `git diff --check`, and restarted the app.

- 2026-08-21 (codex): **Direction-aware transition lifecycle.** The selected
  transition no longer falls back to pre-transition readiness after successful
  replay. The panel retains the exact transition file plus its physical FROM
  deck, reports armed/running/done phases, and finishes with an explicit
  `FROM “track” on Deck X → TO “track” on Deck Y` handoff. Successful
  completion clears pre-state mismatch highlights; abort, selecting another
  edge, recording, Match Setup, or starting another replay returns to preflight
  intentionally. Verified: clean build, ctest 21/21, full self-test, and
  `git diff --check`.

- 2026-08-21 (codex): **Serato tempo ranges and visual setup mismatch
  guidance.** Replaced the fixed ±8% tempo mapping with persisted per-deck
  ±8/±16/±50% choices, preserving the current effective BPM when changing
  range. The on-screen tempo column provides all three choices and FLX4 SHIFT
  + BEAT SYNC (`90/91 60`) cycles them; physical fader input uses the selected
  range. Selected-transition readiness now emits exact mismatched controls and
  overlays the corresponding tempo, fader, EQ/filter, crossfader, FX,
  Quantize, and stem widgets in pulsing amber. Highlights follow CLOSE ENOUGH
  tolerances live, clear when corrected, and reappear after overshoot. Verified:
  clean full build, ctest 21/21, full audio/transition/loop/stem self-test,
  `git diff --check`, visual inspection of both ±8% range buttons in the live
  layout, and a single-process restart with the FLX4 connected.

- 2026-08-21 (codex): **Manual PRIME BPM, armed CUSTOM loops, and per-deck key
  lock.** Checkpointed and pushed all prior work as `05df27f`. PRIME now leaves
  the playing/outgoing deck's effective BPM untouched and, when it is outside
  strict or CLOSE ENOUGH tolerance, asks for the exact manual BPM target and
  accepted margin. Its synthetic beat-zero outgoing tempo restore is also
  suppressed after arming, without dropping genuine recorded tempo moves.
  A populated CUSTOM loop pressed during normal playback now arms in place and
  wraps only after transport reaches OUT; paused decks retain hold-preview and
  PLAY-latch behavior. Added a persisted KEY LOCK checkbox per deck backed by
  vendored MIT Signalsmith Stretch DSP; it preserves pitch under tempo change
  while coarse scratch remains vinyl-like. Focused tests cover transition tempo
  preservation, both CUSTOM paths, and a 440 Hz source at +8% measuring 475 Hz
  unlocked versus 440 Hz locked. Verified: clean full build, ctest 20/20,
  full audio/transition/loop/stem self-test, `git diff --check`, and one running
  instance of the verified binary.

- 2026-08-19 (codex): **Transition identity, negative hot-cue, and replay-latch
  fixes.** Operational transition matching now requires a fingerprint or
  title+artist match; duration within 1.5 seconds remains diagnostic only and
  can no longer select/auto-load a transition, prioritize an unrelated FROM
  track, or trigger another song's hot-cue deletion warning. Missing recorded
  hot-cue mappings now use NaN internally, so finite negative beats before the
  detected first downbeat serialize, parse, and validate normally. Incoming
  PLAY still restores the recorded anchor when standalone, but when pressed
  during a held CUE/HOT CUE/CUSTOM preview it latches the already-advanced
  position instead of rewinding to the preview start. Regressions cover a
  0.7-second duration collision, `b2 = -17`, the recorder's negative-grid path,
  and delayed hot-cue→PLAY replay. Verified: clean full build, ctest 19/19,
  full self-test, and `git diff --check`.

- 2026-08-19 (codex): **FLX4 cue-output hot-plug recovery and headphone test.**
  Diagnosed silence with live CoreAudio state: the four-output FLX4 was alive
  but its audio device had never started while System Default/MacBook speakers
  owned master. Gravitino now retries the separate phones 3/4 stream every two
  seconds while MIDI is connected, reinitializes stale/stopped USB endpoints,
  and logs actual init/start errors plus client/device channel maps. Settings >
  Audio Output now includes a quiet, phones-only two-second test tone. The live
  probe verified CoreAudio's named MASTER L/R and PHONES L/R channels, an
  identical miniaudio channel map, active 48 kHz output, and 0.12 signal at the
  FLX4 callback; the user heard the tone. Restored System Default for MacBook
  master + FLX4 cue and restarted one verified process. Build, ctest 19/19,
  full self-test, and `git diff --check` pass.

- 2026-08-19 (codex): **Hold-preview CUSTOM loops, trim-free transitions,
  FLX4 meters, and live full-board tutor.** Saved-loop pads now press-to-preview,
  release-to-return, and latch when PLAY is pressed during the hold, matching
  hot-cue/CUE semantics; explicit `saved_loop_1..8` press/release events make
  that reproducible in transitions. New transition snapshots/events exclude
  TRIM, and replay ignores legacy trim automation. The controller's documented
  five-segment channel meters now receive post-EQ/filter/FX, pre-fader peaks.
  Tutorial renders a substantially larger, beginner-facing FLX4 surface with
  circular CUE/PLAY, all pad/loop/browser/mixer/cue/Smart/Beat-FX controls,
  live LEDs and meters; its prose/countdown/reset guidance moved to the right
  panel above CLOSE ENOUGH. Post-transition pickup compares the actual last
  hardware state, arms only absolute controls the replay changed, and keeps all
  original targets provisional until they are simultaneously in tolerance, so
  an overshot knob is highlighted again. Verified: clean build, ctest 19/19,
  full audio/transition/loop/stem self-test, and `git diff --check`. Computer
  Use timed out reading the large Qt window, so a physical visual/controller
  smoke test remains appropriate.

- 2026-08-19 (codex): **Centered mixer, compact CUSTOM pads, and persistent
  tutorial view.** Checkpointed all prior work as commit `ecdb7c2`, then moved
  the FLX4-ordered channel volume/EQ/filter mixer out of the transition
  workspace and into a compact center panel below the deck overview-waveform
  baseline. PLAY/CUE/SYNC/QUANT/GRID are fixed compact widths; the normal pad
  modes are a tight 2×2 grid; the combined bank is now presented simply as
  CUSTOM with no Saved Loops menu label. TUTOR VIEW now opens without seeking
  or playing and covers only the transition list + event sequence horizontally,
  extending over the library while leaving Perform/Prime visible. With the
  view open, Perform starts up to eight beats before the anchor for a real
  countdown, while Prime arms the same guided mode against live playback.
  New recordings retain a human-readable `via=performance_pad_N@mode` gesture
  beside the resulting state event, so captured-loop starts are taught as the
  actual CUSTOM pad rather than PLAY. The event sequence exposes the gesture,
  virtual pad clicks execute it, and missing CUSTOM loop mappings warn. Every
  UI/MIDI hot-cue deletion path now warns before removing a cue referenced by
  any transition. Verified: clean build, ctest 19/19, full
  audio/transition/loop/stem self-test, and `git diff --check`; a physical
  FLX4 layout/tutorial smoke test remains appropriate after restart.

- 2026-08-18 (codex): **Post-transition FLX4 pickup gate and tutorial motion
  coaching.** Perform captures every automatic absolute-control change and,
  at the final event, compares Gravitino with the FLX4's last physical TEMPO,
  channel-fader, TRIM, HIGH/MID/LOW, FILTER, and crossfader positions. Any
  mismatch freezes the complete controller input link; only target pickup
  moves are consumed until all controls reach/cross software truth, preventing
  the next touch from causing an audible jump. A blinking white status alert
  shows hardware→target values while affected virtual controls receive a
  pulsing white-static veil. Software edits retarget safely and disconnect
  clears the gate. Tutorial's virtual FLX4 now animates continuous controls
  from the last observed hardware position toward a white target marker and
  keeps the prompt until the target is reached. Verified: build, ctest 19/19,
  full audio/transition/loop/stem self-test, and `git diff --check`; physical
  FLX4 feel/visibility smoke test remains appropriate after restart.

- 2026-08-18 (codex): **FLX4-ordered virtual mixer.** Replaced the unusual
  side-by-side TRIM / stacked-EQ / FILTER arrangement with one uninterrupted
  `TRIM → HIGH → MID → LOW → FILTER` vertical stack per channel. Deck B now
  mirrors deck A, placing both channel faders next to the center crossfader,
  and symmetric spacing keeps the mixer centered without reclaiming horizontal
  room from transitions. Verified visually in the running app, clean build,
  ctest 18/18, and `git diff --check`.

- 2026-08-18 (codex): **Loop- and tempo-exact transition replay.** The
  transition timeline is now a monotonic musical clock, independent of the
  outgoing track position that wraps backward inside a loop. New recordings
  retain six-decimal BPM, tempo-ratio, anchor, loop/hot-cue, and event-time
  precision instead of rounding tempo to 0.001. Perform/Tutorial rebase every
  recorded tempo event against the currently loaded native BPM, preserving the
  actual recorded effective BPM after regrid/rescan changes. Complete
  snapshots persist and restore each deck's Quantize state, and mid-transition
  Quantize toggles are recorded, so manual loop boundaries reproduce their
  original snapping behavior. Regression coverage simulates a loop wrap,
  verifies exact incoming anchor seek, Quantize restoration, later tempo-event
  rebasing, and recorder precision. The legacy Clarity → Party Rock file still
  requires re-recording: its old missing incoming PLAY time and rounded ratio
  cannot be reconstructed from data that was never saved. Verified: clean
  build, ctest 18/18, full audio/transition/loop/stem self-test,
  `git diff --check`, and a single-process restart of the verified binary.

- 2026-08-18 (codex): **Deterministic replay and draggable compact mixer /
  transition layout.** PERFORM now treats incoming PLAY as authoritative:
  when its recorded beat arrives it seeks to `anchor_to` before starting,
  regardless of live deck drift. An incoming deck recorded as already rolling
  is restored and started from its initial beat at transition beat zero.
  Recordings missing any incoming start gesture are rejected with a clear
  re-record message instead of silently omitting the new track. Integration
  coverage proves deck B stays stopped before the due outgoing beat, then
  starts at the exact TO position, and also covers initially rolling pre-state.
  Mixer HI/MID/LOW knobs now stack vertically. Persistent draggable splitters
  resize mixer/transition, transition list/event sequence/controls, and the
  transition/library height. Verified: clean build, ctest 18/18, full
  `--selftest`, and `git diff --check`.

- 2026-08-18 (codex): **Saved-loop starts are transition-recordable.** The
  “clarity to party rock” file proved that its incoming deck began audibly but
  contained no `play` event: its direct saved-loop retrigger bypassed the
  recorder, and `anchor_to` was inferred only when LOOP EXIT arrived 39.740
  beats later. A successful combined sampler/saved-loop retrigger now publishes
  an idempotent PLAY after seeking, capturing the event time and exact loop IN
  beat as the incoming replay anchor. Focused coverage checks the incoming
  PLAY event, anchor, and initial loop state. Existing recordings with a
  missing event are not silently rewritten because their original start time
  is not stored. Verified: clean build, ctest 17/17, full `--selftest`,
  `git diff --check`, and a single-process restart.

- 2026-08-18 (codex): **Close Enough transition readiness; combined sampler /
  saved-loop bank.** Transition setup matching now has a persisted, default-off
  CLOSE ENOUGH checkbox and independently configurable BPM, volume, and EQ
  margins (defaults ±0.5 BPM / ±5% / ±5%). Discrete/identity/transport,
  loop, FX, filter, stem, and timing checks remain strict, and the status label
  identifies readiness accepted only by the relaxed margins. The separate
  SAVED LOOP layer is consolidated into SAMPLER / SAVED LOOPS using the FLX4's
  actual sampler bank; every press of a filled saved-loop pad jumps to its
  start and plays, retriggering an already-active loop like a one-shot hot cue.
  LOOP EXIT remains the explicit way to disengage it. Loop and sampler editing
  coexist in one context menu and their occupancy drives pad LEDs. Verified:
  clean build, ctest 17/17, full
  `--selftest`, and `git diff --check`.

- 2026-08-18 (codex): **Playing FROM transitions first; crossfader starts
  centered.** Library > Transitions now pins rows whose FROM track matches any
  currently playing deck above all other edges, while retaining the selected
  column/direction as the secondary sort. The priority refreshes as transport
  state changes. Fresh audio engines and the visible mixer now both start the
  crossfader at exact `0.5` (equal-power 50/50); explicit controller or
  transition setup state still overrides it normally. Verified: clean build,
  ctest 16/16 (including startup-center assertion), full `--selftest`, and
  `git diff --check`.

- 2026-08-18 (codex): **Compact button labels auto-shrink instead of crop.**
  New shared `FitPushButton`/`FitToolButton` painting preserves Qt/QSS button
  chrome, states, palettes, and menu arrows, while fitting the full visible
  label into the live content rectangle. Existing short labels retain their
  configured size; long and dynamic labels refit automatically. Applied across
  deck, pad/loop/FX/stem, transition, library, recording, and waveform zoom
  controls. A focused regression covers short-label non-shrink, long-label
  width/height fitting, and mnemonic ampersands. Verified: clean build, ctest
  16/16, full `--selftest`, and `git diff --check`.

- 2026-08-18 (codex): **Saved Loops, real scratch audio, Quantized IN/OUT,
  phase-only SYNC, and transition-edge loading.** A host-only SAVED LOOP pad
  layer captures, recalls/toggles, renames, replaces, and clears eight exact
  per-track loop slots; hot cues/loops merge atomically into the cache. FLX4
  touch note 0x36 and stacked-waveform drags now suspend normal transport and
  render signed forward/reverse PCM only while moving, restoring the prior
  play state on release. QUANT applies to manual IN/OUT; SYNC aligns beat phase
  once without changing tempo ratio/effective BPM. Clicking a transition edge
  loads both tracks with zero playing decks, only TO after a matching FROM with
  one, and refuses with an explanation when two play. Verified: clean build,
  ctest 15/15, full `--selftest`, `git diff --check`, and a single-process
  restart of the verified binary.

- 2026-08-18 (codex): **Host-authoritative FLX4 pad layers and tempo-scaled
  stacked waveforms.** The FLX4's private pad-bank latch can disagree with a
  virtual mode selected in Gravitino; MIDI output only changed LEDs. Incoming
  pads are now resolved through Gravitino's selected layer, while the reported
  hardware bank is retained solely to mirror logical pad lights into the bank
  the controller is displaying. Physical mode buttons still update the host
  immediately, and SHIFT deletes only when the host layer is HOT CUE. Stacked
  lanes now span equal playback time by multiplying source-window seconds by
  each deck's tempo ratio, so matched effective BPM produces matched beat
  spacing. Pointer seek/scratch uses the identical transform. Verified: build,
  ctest 15/15, full `--selftest`, and `git diff --check` pass.

- 2026-08-18 (codex): **FLX4 loops, Quantize, browser/load, scratching,
  programmable pad layers, LEDs, and persistent manual regrid integrated.**
  The physical IN/1/2X and OUT/2X controls resize an active loop contextually,
  while armed/completed loop regions render live in both waveforms. Per-deck
  persisted Quantize snaps hot-cue placement and jumps to playable whole
  beats. The center browser and LOAD buttons drive the visible library but
  refuse a playing deck with a temporary warning. Stacked-waveform drag and
  top-platter packets position-scratch; the jog rim remains a fine tempo nudge.
  Eight persisted/programmed virtual pad layers mirror FLX4 mode/pad LEDs, and
  SHIFT+HOT CUE deletes a cue. GRID menus set/nudge downbeats and correct BPM;
  edits update Deck's realtime grid and merge atomically into the cache. PAD FX,
  beat jump/loop, and hot cues execute now; sampler/keyboard/key-shift settings
  persist but await sampler/pitch DSP. Verified: clean build, ctest 15/15, full
  `--selftest`, `git diff --check`, direct visual layout inspection, and a
  single-process restart with the FLX4 connected.

- 2026-08-18 (codex): **Full FLX4 transition tutor + selectable dual-device
  audio.** Tutorial mode now opens a full, pulsing virtual DDJ-FLX4 surface,
  queues prompts in transition order, labels the physical deck/control/value,
  and accepts either physical MIDI or a click on the highlighted virtual
  control. Unsupported FLX4 gestures are shown in amber and warned before
  launch. New `.gvt` recordings persist the track beat behind each referenced
  hot-cue pad; Tutorial warns for missing, legacy-unverifiable, or position-
  mismatched pads and will not make a bad virtual cue fire. Settings > Audio
  Output lists CoreAudio devices and persists System Default, MacBook,
  Bluetooth, or FLX4 selection. Non-FLX4 master outputs now run alongside a
  second FLX4 stream: master goes to the selected speakers and a bounded SPSC
  monitor ring feeds only FLX4 phones 3/4; selecting FLX4 uses its combined
  four-channel path. PLAY now takes over an active CUE or hot-cue hold preview,
  so releasing the held cue keeps playback rolling; UI and MIDI PLAY toggling
  both distinguish that takeover from a stop request. Verified: build clean,
  ctest 10/10 (including cue/hot-cue takeover and saved hot-cue mapping), and
  full `--selftest` pass. Live visual inspection via the macOS accessibility
  tool was unavailable (the tool timed out); physical speaker/headphone
  confirmation is in progress.

- 2026-08-18 (codex): **DDJ-FLX4 headphone PFL routing implemented.** Selecting
  the FLX4 audio output opens its four-output CoreAudio device and writes master
  to 1/2 and headphones to 3/4. The official
  channel-CUE notes (`90/91 54`), master-CUE note (`96 63`), and 14-bit
  HEADPHONES MIX CC (`B6 0C/2C`) now drive a post-EQ/filter/FX, pre-fader PFL
  bus; channel CUE LEDs are host-mirrored. Master recording still receives only
  the master stereo bus. The user confirmed the separate
  "hot cue plays the other song" report was Serato playing in the background,
  not Gravitino; no hot-cue behavior was changed. Verified: clean build and
  no-op rebuild, ctest 9/9, full `--selftest`, and `git diff --check` all pass.
  The new build was restarted with the connected FLX4; physical headphone
  monitoring remains for the user to confirm by listening.

- 2026-08-18 (codex): **No duplicate audio engines / deck source layering.**
  GUI startup now holds a per-user process lock before opening CoreAudio, so a
  hidden older Gravitino process cannot keep playing underneath the visible
  window; a second launch explains that another window owns audio and exits.
  Added a concurrent offline deck-swap regression that proves `loadTrack()`
  drains an active render, stops/rewinds, and emits only the replacement PCM
  after returning. Verified: build clean, ctest 8/8 including the new stressed
  swap test (also repeated 20/20), full `--selftest`, and `git diff --check`
  all pass.

- 2026-08-17 (codex): **Safe deck loading + transition edge-list planner.**
  Library load buttons now track the selected row's readiness and each deck's
  live transport state: a playing deck's button is clearly disabled with an
  explanatory tooltip, and the load handler independently refuses replacement
  if invoked through another path. Disabled transition REC styling is more
  pronounced. A new searchable, sortable Library > Transitions tab lists every
  saved transition as a directed From track → To track edge with its name,
  BPM, beat length, and cue count, independent of the tracks currently loaded.
  Verified in the live UI at compact window size; build clean, ctest 7/7,
  full `--selftest`, and `git diff --check` all pass.

- 2026-08-17 (codex): **Hold-preview cues + deterministic transition pre-state.**
  Assigned hot cues now play while held and stop/return to their marker on
  release (unset pads still store). Crossfader checkpoint runs remain intact
  internally but the event table/waveform auto-labels show only start/end.
  Clicking the selected transition again deselects it and clears deck markers;
  a vertical splitter lets the library shrink so the event table can grow.
  New recordings persist a complete role-based snapshot for both decks and the
  mixer: positions/cues, tempo, faders/trim/EQ/filter, loops, FX, stems, and
  crossfader. Perform stops and reconstructs that state, arms, then rolls from
  the entry marker; Prime prepares/verifies the full state, rejects an entry
  already passed, and reasserts incoming transport at the actual boundary.
  Legacy partial snapshots remain supported. Verified: build clean, ctest 7/7,
  full `--selftest`, and a live compact-window UI inspection all pass.

- 2026-08-17 (codex): **Rotary audio controls no longer wrap.** Mixer trim,
  high/mid/low EQ, filter, and deck FX wet dials explicitly clamp at their
  limits instead of crossing the angular seam from minimum to maximum (or
  vice versa). Verified: build clean, ctest 7/7, and `--selftest` passes.

- 2026-08-17 (codex): **Transition workflow + cue navigation pass complete.**
  Assigned hot cues now stop and park at the marker instead of auto-playing,
  so beat-jump can be used to prepare before it. The transition panel disables
  actions that are invalid in the current state (including repeat REC and
  STOP & SAVE before any event), adds guarded rename/delete, a selected-file
  event-sequence table, persistent user cue labels, and labeled markers on both
  overview/detail waveforms. New recordings persist an `[initial]` outgoing
  setup snapshot (tempo, fader, trim, EQ, filter); the panel reports mismatches,
  provides MATCH SETUP, and Perform/Prime restores it at the anchor. Legacy
  files still get BPM matching and explicitly report that EQ was not stored.
  Verified: build clean, ctest 7/7 including hot-cue and temp-dir store CRUD
  regressions, GUI accessibility pass confirms compact panel/library layout,
  and `--selftest` passes.

- 2026-08-16 (orchestrator): **STEMS ROUND COMPLETE.** Engine half done by
  the orchestrator (codex-stems hung and was killed): Deck::attachStems with
  seamless publish / drained replace, stem-aware render sampling (untouched
  master when all levels = 1), applyEvent stem cases. Verified: selftest stem
  section (full/muted/solo RMS), live pad toggles, and a live-recorded .gvt
  containing stem_vocals/stem_drums cuts. Recorder now snaps value jumps
  > 0.45 as Step so toggles never fade on replay. Remaining parity backlog:
  sampler pads, 4-deck, smart crates/iTunes import, pitch readouts.

- 2026-08-16 (claude-stems): **Stem separation pipeline + stem pads UI.**
  New `src/analysis/StemSeparator.{h,cpp}` (gvtcore): GUI-thread QObject that
  runs the demucs CLI (`/Users/fish/.local/bin/demucs -n htdemucs -d mps`)
  via QProcess — FIFO queue, one process at a time, no timeout (minutes per
  track), tqdm "NN%" stderr lines forwarded as `progress(fingerprint,
  stage)`; duplicate requests for an in-flight fingerprint just wait. Output
  wavs (demucs `other` = our `melody`) are moved from a temp dir into the
  per-track cache `~/.gravitino/stems/<fp-hex>/{vocals,other,bass,drums}.wav`
  on success; cached tracks skip demucs entirely (`hasCached`). Decode runs
  on QtConcurrent: miniaudio `ma_decoder` at s16/2ch/48k (resamples the
  44.1k wavs), each stem padded/truncated to the track's frameCount,
  delivered as `stemsReady(fingerprint, StemSetPtr)` on the GUI thread;
  demucs missing/nonzero exit → `stemsFailed` with the stderr tail.
  DeckWidget gained a STEMS row below FX: state machine Idle ([STEMS]
  request button; pads checked+disabled) → InProgress (stage/percent label)
  → Ready (pads enabled); pads [VOCAL #38c9b8][MELODY #e8a13a][BASS #7a5ae8]
  [DRUMS #e05a8a] are checkable, dispatch `{deck, StemX, 1/0}` Origin::Ui on
  toggle, and mirror the deck stem atomics at 30 Hz under QSignalBlocker.
  MainWindow matches every StemSeparator signal against each deck's CURRENT
  track fingerprint (track swapped mid-separation → result dropped for that
  deck, cache keeps it), calls `Deck::attachStems` on match, shows failures
  in the status bar, resets the row on track load, and auto-requests the
  cheap decode-only path when `hasCached()` hits. main.cpp constructs the
  separator and passes it to MainWindow (new trailing ctor param, default
  nullptr = feature inert). docs/TRANSITION_FORMAT.md: `stem_*` rows added
  with graceful-replay-without-stems note. Verified: all 4 touched TUs
  compile -Wall -Wextra clean; standalone cache/decode proof (cache seeded
  from the orchestrator's pre-separated Demo Track 1 wavs, fp
  912f445668f4ccba): "test_stemsep OK" — 4 stems, each 8276846 frames ==
  track frameCount, rms vocals .0099 / melody .0376 / bass .1955 / drums
  .1567; duplicate-request no-op covered. Full gravitino link still blocked
  on peer `Deck::attachStems` (codex-stems-audio, in flight) — everything
  else links; retry `ninja -C build-stemsui` once it lands.
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
  OUT (loop active). LOOP IN now immediately draws a labeled dashed marker;
  before OUT, its accent shading grows to the live playhead. The completed
  loop region [loopStartSec,loopEndSec] remains shaded in BOTH the deck
  overview and the DetailWaveformView lanes while playback advances/wraps;
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
| analysis/StemSeparator.*, ui stems row (DeckWidget/MainWindow additions), main.cpp wiring | claude-stems | DONE 2026-08-16 — demucs pipeline + cache + decode + stem pads UI (see Current state above). Awaiting peer Deck::attachStems for the full link. |
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
| LOOP IN / 1/2X | A / B | `90/91 10 hh` | Nonzero press → `LoopIn` when inactive, `LoopHalve` when active; active-loop LED output `90/91 10 7F/00` |
| LOOP OUT / 2X | A / B | `90/91 11 hh` | Nonzero press → `LoopOut` when inactive, `LoopDouble` when active; active-loop LED output `90/91 11 7F/00` |
| 4 BEAT / EXIT | A / B | `90/91 4D hh` | Nonzero press → `LoopAuto=4.0` when inactive, `LoopExit` when active; LED `90/91 4D 7F/00` |
| SHIFT + 4 BEAT / EXIT | A / B | `90/91 50 hh` | Nonzero press → `LoopExit`; modifier-state LED mirrors active state |
| CUE/LOOP CALL left / right | A / B | `90/91 51 hh` / `90/91 53 hh` | Nonzero press → `LoopHalve` / `LoopDouble` |
| SHIFT + LOOP IN / OUT aliases | A / B | `90/91 4C hh` / `90/91 4E hh` | Nonzero press → `LoopHalve` / `LoopDouble`; modifier-state LEDs mirror active state |
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
  jumps there and plays while held. Releasing stops and returns to that marker.

## Known decisions & gotchas

- Engine sample rate fixed at 48 kHz (`kSampleRate`); all decode resamples to it.
- Audio track replacement uses a stop-and-drain swap; the callback uses only
  atomics, fixed 256-frame scratch buffers, and allocation-free DSP.
- Deck DSP uses selectable linear/vinyl tempo resampling or Signalsmith Stretch
  key lock, RBJ three-band EQ, a post-filter per-deck FX insert, equal-power
  master crossfade, and a shared realtime/offline `tanh` soft-clip mix path.
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
- Recorder retains six-decimal timing/BPM/tempo precision; normalized
  fader/EQ-style values remain quantized to their actual 0.001 UI resolution.
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
- Variable beatgrids, WAV/FLAC/AAC, transition
  marketplace/sharing UI, Windows/Linux builds, .app bundling + macdeployqt.
