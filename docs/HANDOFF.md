# Gravitino Agent Handoff

Updated: 2026-08-19 (Asia/Singapore)

## Current state

All prior DJ-controller follow-ups through the compact mixer and transition
tutorial were committed and pushed at `18b70c6` (`Refine transition tutorial
and compact mixer layout`). The hold-preview CUSTOM loops, transition TRIM
exclusion, FLX4 channel meters, live full-board tutor, and selective persistent
post-transition pickup described below are intentionally uncommitted pending
the user's next checkpoint request.

The user explicitly permits stopping a running Gravitino process when a
restart is needed. Always resolve the process narrowly with
`ps -axo pid,etime,command | rg '[b]uild/gravitino$'` so Serato or unrelated
audio programs are never touched.

The Git remote is `git@github.com:tch1001/DJ_gravitino.git`; `main` tracks
`origin/main`. Do not commit or push this working tree until the user asks.

## Implemented in this working tree

- **Reliable transition identity and exact held-cue replay:** duration-only
  similarity remains available as a weak diagnostic tier but can no longer
  authorize Perform/Tutorial matching, transition auto-load, playing-FROM
  prioritization, or hot-cue dependency warnings. Those paths require an exact
  fingerprint or matching title+artist. Recorded hot-cue mappings now use NaN
  for “unavailable,” allowing real negative beats before a track's detected
  first downbeat to round-trip and validate correctly. A standalone incoming
  PLAY still restores its recorded anchor; PLAY during a held CUE/HOT
  CUE/CUSTOM preview now latches the advanced preview position without the old
  rewind. Focused regressions reproduce all three original failures and the
  full build, ctest 19/19, self-test, and diff check pass.

- **Recovering FLX4 headphone output:** MacBook/Bluetooth master output keeps a
  separate four-channel FLX4 stream for phones 3/4. A startup race could leave
  MIDI connected before CoreAudio exposed that endpoint, with no later retry.
  Gravitino now retries the non-destructive cue stream every two seconds while
  the controller is present, detects/stales out a stopped USB audio device,
  reports exact miniaudio failures/channel maps, and offers `Settings > Audio
  Output > Test FLX4 headphones` (quiet two-second phones-only tone). The live
  probe reached the controller callback at 0.12 peak and the user confirmed
  hearing it; System Default/MacBook master output was then restored.
- **Hold-preview CUSTOM loops:** populated saved-loop pads now use the same
  momentary transport contract as hot cues. Press jumps to the stored loop and
  previews it, release stops and returns to the loop start, and pressing PLAY
  while the pad remains held latches playback so release no longer stops it.
  The UI and `.gvt` event stream retain both press and release through explicit
  `saved_loop_1..8` controls, while empty pads still capture the active loop.
- **TRIM is outside transition semantics:** new recordings omit both initial
  TRIM snapshots and runtime TRIM moves. Perform also filters TRIM from legacy
  event streams and never primes/applies their stored trim values. Old files
  remain parseable, but the DJ's current input gain is deliberately left alone.
- **Physical FLX4 channel meters:** Gravitino now sends each deck's post-EQ /
  filter / FX, pre-channel-fader peak to the controller's documented five-bar
  meter (`B0/B1 02`), with dB scaling, 40 ms updates, change suppression, and
  reconnect resynchronization.
- **Live, full-board transition tutor:** the virtual FLX4 now includes the full
  beginner-facing control surface, including circular CUE and PLAY/PAUSE left
  of the pads, pad modes/pads, loop controls, browser/LOAD, mixer, channel and
  master cue, shifted pad functions, Quantize indicator, SMART controls, and
  Beat FX. The large prose block was removed
  from the board; instruction, detail, countdown, warning, score, and reset
  coaching now live in the right control panel above CLOSE ENOUGH, leaving the
  board substantially larger. Outside a just-completed automatic transition it
  mirrors live transport, modes, pad state, knobs/faders, LEDs, and meters.
- **Selective, persistent post-transition pickup:** MidiEngine snapshots the
  pre-transition virtual controls and only arms physical pickup for absolute
  controls the replay actually changed. Reconciliation uses the last observed
  physical position instead of assuming the whole FLX4 is wrong. A matched
  knob is not forgotten until every pending control is simultaneously within
  tolerance, so overshooting it makes its white target/highlight reappear.
  Pickup moves stay audio-silent while the link is frozen.

- **Centered mixer and compact deck controls:** the `TRIM → HIGH → MID → LOW →
  FILTER` channel strips and volume faders now occupy a narrow center panel in
  the deck row, aligned below the overview waveforms. Transition/event panels
  regain the full lower width. PLAY/CUE/SYNC/QUANT/GRID use compact fixed
  widths, normal pad-mode buttons use a 2×2 grid, and the combined loop/audio
  pad bank is presented as CUSTOM (the old Sampler/Saved Loops names remain
  only as internal compatibility identifiers).
- **Persistent compact Tutorial view:** TUTOR VIEW only opens/closes the view;
  it never seeks or starts a track. The overlay spans the transition list and
  event sequence, then extends downward over the library, leaving the right
  Perform/Prime controls available. With the view open, Perform starts up to
  eight beats before the anchor for countdown runway; Prime arms Tutorial at
  the recorded entry. The view stays open after completion/abort.
- **Recorded button intent and protected hot cues:** performance-pad actions
  attach a human-readable `via=performance_pad_N@mode` hint to the audible
  `.gvt` event. Replay still uses the deterministic state event, while Tutorial
  highlights/invokes the recorded pad and the event sequence shows the source,
  avoiding a misleading PLAY instruction for CUSTOM loop starts. Missing
  CUSTOM loop mappings warn. UI shift-click, context-menu, and FLX4 shift-pad
  hot-cue deletion all consult every stored transition and require explicit
  confirmation when that cue is referenced.

- **Post-transition FLX4 soft takeover:** Perform tracks replay/setup changes
  to the physical TEMPO, channel fader, HIGH/MID/LOW, FILTER, and crossfader
  controls. At the final event, the entire FLX4→Gravitino input link freezes if
  any changed control's physical position differs. Only pickup moves are
  consumed until every target is simultaneously within tolerance; the pickup
  event itself is not applied, so audio never jumps. A blinking white status alert lists
  hardware→target values and white-static veils mark the affected virtual
  controls. Software edits retarget the pending pickup, disconnect clears it,
  and no gate appears without a connected controller. Tutorial's full FLX4
  overlay now adds an animated current-to-target marker for continuous moves
  and does not dismiss the instruction after a mere knob nudge.
- **FLX4-ordered mixer strips:** each virtual channel now uses one continuous
  top-to-bottom `TRIM → HIGH → MID → LOW → FILTER` knob stack. Deck B mirrors
  deck A so both channel faders sit beside the center crossfader; symmetric
  outer stretch keeps the entire mixer centered while preserving the compact,
  draggable transition workspace.
- **Loop- and tempo-exact transition timing:** recording and replay use a
  monotonic musical clock, so an outgoing loop wrap no longer rewinds the
  transition event sequence. Complete snapshots now persist/restore each
  deck's Quantize state, making manual LOOP IN/OUT resolve the same way during
  replay. BPMs, tempo ratios, anchors, hot-cue/loop beats, and event timestamps
  retain six-decimal engine precision; later recorded tempo events are rebased
  against the currently loaded native BPM so a manual regrid does not change
  their recorded effective BPM. Legacy files remain readable, but precision or
  incoming PLAY timestamps already discarded by an old recording cannot be
  inferred. In particular, `clarity-to-party-rock-8d60.gvt` must be re-recorded.
- **Deterministic, self-contained transition replay:** PERFORM restores both
  decks' complete recorded setup and transport state at the outgoing anchor.
  A recorded incoming PLAY always seeks to `anchor_to` at its scheduled beat,
  even if the deck was moved or started after priming. If the incoming deck
  was already rolling when recording began, replay now starts it from the
  recorded initial beat at transition beat zero. PERFORM refuses recordings
  that began stopped but contain neither an incoming PLAY nor hot-cue event,
  rather than silently performing without the new track; affected bug-era
  files must be re-recorded because their missing start timestamp is absent.
- **Space-efficient transition workspace:** each mixer channel's HI/MID/LOW EQ
  knobs is stacked vertically in the centered deck-row mixer, so the transition
  panel now owns the full lower width. Inside that panel, persistent handles
  independently resize the transition list, event sequence, and controls. The
  transition/library vertical splitter still persists its position.
- **Configurable Close Enough transition setup:** the transition panel has a
  persisted CLOSE ENOUGH checkbox plus a TOLERANCE… dialog. Strict matching
  remains the default. When enabled, BPM, volume controls (channel fader and
  crossfader), and LOW/MID/HIGH EQ can differ from the recorded
  pre-state by independently configurable margins (defaults: ±0.5 BPM,
  ±5% channel/crossfader volume, ±5% EQ). Track identity, transport/loop state, FX state and
  type, filters, stems, cue/loop timing, and other setup remain strict. The
  live readiness label says explicitly when a state was accepted only because
  it was close enough; PRIME uses the same readiness policy after its robust
  pre-state preparation.
- **CUSTOM bank:** the former host-only SAVED LOOP menu was folded into the
  FLX4 sampler bank and is now presented simply as CUSTOM; old mode selections
  migrate automatically. Each pad can retain an audio-file assignment while
  its per-track captured loop takes operational/visual priority.
  An empty pad captures the active loop. A filled pad jumps to the stored start
  and previews only while held; PLAY during the hold latches transport, exactly
  like the hot-cue/CUE takeover contract. Pressing it again retriggers the loop.
  LOOP EXIT remains the explicit way to leave the saved loop itself.
  Right-click exposes loop replace/rename/clear and sampler assign/clear
  together, and the combined occupancy is reflected in the physical pad LEDs.
  Explicit saved-loop press/release events store the incoming start time and
  loop-IN beat as their TO replay anchor instead of inferring it from a later
  control move.
- **Auto-fitting button labels:** Gravitino's compact push/tool buttons now
  preserve their normal frame, state, menu arrow, palette, and configured font,
  but shrink the font at paint time when the complete live label would
  otherwise be cropped. Short labels are unchanged; dynamic recording text,
  shifted pad modes, custom loop/pad labels, and translated UI strings refit
  automatically after text or geometry changes. The shared implementation is
  `src/ui/FitButton.h`.
- **Playing-FROM transition priority:** Library > Transitions keeps every edge
  whose FROM reference matches a currently playing deck in a live top group.
  The selected column and ascending/descending direction remain the secondary
  ordering within the playing and non-playing groups. A 100 ms state poll
  refreshes the priority when playback or the loaded track changes.
- **Centered startup crossfader:** both `AudioEngine::crossfader` and the mixer
  slider initialize to `0.5`, producing an equal-power 50/50 blend until the
  user, FLX4, or a recorded transition setup moves it.
- **FLX4 loop sizing:** the physical dual-purpose IN/1/2X and OUT/2X buttons
  are resolved against the deck's live loop state. With no active loop they
  set IN/OUT; with an active loop they halve/double it. CUE/LOOP CALL and
  SHIFT+IN/OUT aliases remain supported.
- **Live loop display:** LOOP IN immediately draws a labeled marker in the
  overview and stacked waveform. Before OUT, the region grows to the live
  playhead; after OUT, the fixed loop is shaded through playback and wrapping.
- **Hot-cue Quantize:** each deck has a persisted QUANT toggle, enabled by
  default. Placement and triggering snap to a playable whole-beat grid line.
  FLX4 SHIFT+CUE toggles it and its LED mirrors host state.
- **Physical browser/load:** the center encoder navigates the visible track
  list, encoder press confirms the row, and LOAD A/B loads it. A playing deck
  cannot be replaced: both on-screen load controls and the physical command
  are refused, with a temporary status warning.
- **Scratching:** dragging either lane of the stacked waveform performs short
  audible position scrubs and freezes when the hand stops. FLX4 top-platter
  packets perform coarse position scrubbing; rim packets remain a decaying
  fine tempo nudge for beatmatching. Both paths respect track/loop bounds.
- **Performance pads:** the virtual deck exposes HOT CUE, PAD FX1, BEAT JUMP,
  and SAMPLER, plus a SHIFT MODES menu for KEYBOARD, PAD FX2, BEAT LOOP, and
  KEY SHIFT. Assignments and the selected layer persist per deck. Right-click
  programs supported parameters/labels. Mode buttons and pad LEDs are mirrored
  to the FLX4; SHIFT+HOT CUE deletes the slot and the LED follows the change.
- **Host-authoritative pad layers:** the FLX4 keeps a private hardware pad-bank
  latch and its documented MIDI output can light mode buttons but does not set
  that latch. Gravitino therefore treats its selected virtual layer as the
  action source of truth for every incoming physical pad number. It tracks the
  controller-reported bank only to mirror the logical pad lights into the bank
  the hardware is actually displaying. Virtual mode changes and physical mode
  button changes now produce the same pad behavior instead of LED-only state.
- **Tempo-scaled stacked waveform:** `windowSec_` is now playback time. Each
  lane displays `windowSec_ * tempoRatio` source seconds, and pointer seeking/
  scratching uses the same transform. Two decks at the same effective BPM now
  have the same beat spacing even when their native BPM/tempo ratios differ.
- **Captured-loop persistence:** eight exact loop bounds and labels persist per
  track alongside hot cues and survive rescans without overwriting newer
  grid/analysis metadata. They are exposed through CUSTOM as described above.
- **True touch-gated scratch:** FLX4 platter-touch note `0x36` suspends ordinary
  transport. A stationary held top is silent; top-wheel movement renders
  signed PCM movement, including reverse, and release restores the transport
  state from before the touch. The stacked waveform uses the same engine and
  no longer briefly starts normal playback while dragging. The platter top is
  coarse scratch; the rim remains the fine, decaying beatmatch nudge.
- **Quantized manual loops:** QUANT now applies to LOOP IN and OUT as well as
  hot cues. Enabled manual bounds land on playable whole beats; disabled bounds
  preserve the exact pointer/playhead time (subject to the existing minimum
  valid loop length).
- **One-shot phase-only SYNC:** the on-screen SYNC and FLX4 BEAT SYNC note both
  align the selected deck to the other deck's beat phase once. They do not
  change `tempoRatio` or effective BPM and do not latch.
- **Transition-edge click loading:** clicking a row in Library > Transitions
  loads FROM to A and TO to B when neither deck is playing. With exactly one
  playing deck, it loads only TO onto the other deck when the playing track
  matches FROM. With two playing decks, or a non-matching FROM, it changes
  nothing and explains why in the status bar.
- **Manual regrid:** each deck has a GRID menu to set the current playhead as
  the downbeat, nudge the grid earlier/later by 10 ms, halve/double BPM, or
  enter a BPM from 20–400. Corrections update the waveform and Deck's realtime
  BPM/anchor state immediately, affect Quantize/loops/beat jump/sync/FX timing,
  and are atomically merged into the analysis cache without overwriting newer
  metadata from an older loaded deck.
- **Transition isolation:** browser, quantize, scratch, and raw pad-layer
  gestures do not clutter transition recordings. Resolved musical pad actions
  still travel through normal controls where appropriate.

## Honest limitations and hardware QA

- PAD FX1/PAD FX2, BEAT JUMP, BEAT LOOP, and HOT CUE execute audio/control
  actions now. SAMPLER file slots, KEYBOARD notes, and KEY SHIFT values can be
  programmed and retained, but sample playback and pitch-shift DSP do not yet
  exist; the UI says so instead of silently pretending they ran.
- Scratch audio now renders forward and reverse PCM, but platter sensitivity
  and perceived vinyl feel still need a physical/audio check.
- The FLX4 pad/mode protocol follows the controller's standard note layout.
  PAD FX note-bank bases follow its regular bank pattern and the new logical-
  layer override still needs final physical confirmation across all eight modes.
- Automated tests cannot verify speaker/headphone routing, LED colour/intensity,
  platter feel, or an audible loop wrap. Check both decks on the connected FLX4.

## Verification

From `/Users/fish/gravitino`:

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/gravitino --selftest
git diff --check
```

Latest result: build succeeded; 19/19 tests passed;
the full audio, transition, loop, and stem self-test passed; and
`git diff --check` passed. Focused tests now cover touch press/release mapping,
silent stationary scratch, forward/reverse scratch audio, transport restore,
phase-only SYNC tempo preservation, Quantized/exact manual IN/OUT, exact saved
loop activation, saved-loop/hot-cue persistence with stale-metadata merge,
strict versus user-configured Close Enough transition margins, and exact
incoming transition position/timing with initially stopped or rolling decks.
Transition replay coverage also simulates an outgoing loop wrap, verifies that
the monotonic sequence still fires on time, checks Quantize restoration, keeps
six-decimal recorded tempo, and preserves effective BPM after a manual regrid.
It now also checks readable physical performance-pad gesture capture and an
eight-beat Tutorial prompt for a button due at transition beat zero.
`tests/test_soft_takeover.cpp` covers matching-at-arm, unknown positions,
input freezing, consumed pickup events, software retargeting, and a control
being highlighted again after it overshoots while another target is pending.
Transition coverage verifies continuous tutorial prompts remain active until
their recorded value is reached. The Computer Use accessibility service timed
out while reading the large Qt window, so the full-board tutor still deserves
a physical visual pass. The rebuilt UI is running as the sole Gravitino process
(PID 25864 at this handoff); resolve its current PID narrowly before any future
restart.

New focused coverage includes:

- `tests/test_flx4_mapping.cpp`
- `tests/test_jog_scratch.cpp` (including live runtime regrid state)
- `tests/test_performance_pads.cpp`
- `tests/test_soft_takeover.cpp`
- `tests/test_transition_tolerance.cpp`
- `tests/test_transition_replay.cpp`
- `tests/test_beatgrid_editor.cpp`
- `tests/test_beatgrid_persistence.cpp` (including stale-metadata merge)
- expanded quantized/legacy hot-cue cases in `tests/test_hotcue.cpp`

## Suggested physical smoke test

1. On both decks, create a 4- or 8-beat loop and press IN/1/2X, then OUT/2X.
   Verify the shaded bounds and audible wrap halve/double without resetting IN.
2. Arm LOOP IN before OUT and confirm the growing region is visible in both
   waveform views.
3. Toggle QUANT with SHIFT+CUE, place/trigger a cue off-grid, then regrid with
   GRID > Set downbeat here and confirm the corrected beat line drives it.
4. Turn the center encoder, press it, LOAD each stopped deck, then attempt LOAD
   on a playing deck and confirm it is refused with a warning.
5. Touch the platter while playing: stationary touch must be silent, forward
   and reverse turns must sound in their direction, and release must resume.
   Compare the top with the rim nudge; tune
   `kPlatterScratchSecondsPerTick` in `src/audio/Deck.cpp` only if needed.
6. In CUSTOM, capture, one-touch start, rename, and clear loop
   pads; then select every other pad layer, press pads on both decks, delete a cue with
   SHIFT+HOT CUE, and confirm virtual state and controller LEDs agree.
7. Set different tempo ratios, press BEAT SYNC, and confirm phase aligns once
   without either BPM/tempo fader moving. Toggle QUANT and compare exact versus
   whole-beat manual IN/OUT.
8. Click transition edges with zero, one, and two playing decks and verify the
   load policy/status explanations.
9. Select a transition, perturb BPM/volume/EQ just inside and outside the
   configured CLOSE ENOUGH margins, and confirm readiness changes while track,
   loop, and FX-state mismatches remain strict.
10. Confirm the centered mixer begins below the deck overviews, the compact
    transport + 2×2 mode grid fit without cropping, then drag the
    transition-list/event/control and transition/library handles; restart and
    confirm both sizes persist.
    PERFORM a newly recorded transition after deliberately moving the incoming
    deck and verify it still starts at the recorded beat/position.
11. Re-record Clarity → Party Rock with QUANT/loops and any tempo adjustment
    used in the real transition. Perform it twice and verify Party Rock starts
    on the same beat and remains phase-aligned through the exit.
12. Perform a transition after deliberately leaving several FLX4 knobs/faders
    away from their recorded final values. Confirm all hardware actions freeze,
    the affected virtual controls show blinking white fuzz, and moving each
    physical control through the displayed target removes it without an audio
    jump. In Tutorial, confirm the white target marker animates in the required
    direction and remains until the value is reached.
13. Open TUTOR VIEW and confirm it does not seek or play. With it open, test
    both Perform (eight-beat pre-anchor countdown) and Prime (live armed
    countdown), including a newly recorded CUSTOM-loop start that must
    highlight the actual CUSTOM pad instead of PLAY. Attempt to remove a hot
    cue used by the transition and confirm the dependency warning appears.
14. Confirm System/MacBook/Bluetooth master output and FLX4 headphone cue after
   restart. If duplicate audio is heard, check that Serato is stopped and that
   exactly one Gravitino process exists.

## Key files

- `src/midi/Flx4Mapping.*`, `src/midi/MidiEngine.*`,
  `src/midi/SoftTakeover.*`: physical messages, contextual routing, LED state,
  and post-replay pickup gating.
- `src/performance/PerformancePads.*`, `src/ui/DeckWidget.*`: programmable pad
  model/UI, Quantize, GRID controls, and overview loop display.
- `src/audio/Deck.cpp`, `src/audio/AudioEngine.*`: Quantize, scratch, loop,
  runtime regrid, and control execution.
- `src/ui/DetailWaveformView.*`: stacked-waveform scratch and loop visuals.
- `src/analysis/BeatGridEditor.*`, `src/library/TrackLibrary.*`: manual grid
  math and atomic persistence.
- `src/ui/LibraryWidget.*`, `src/ui/MainWindow.cpp`: browser/load guards and
  cross-subsystem wiring.

Also read `docs/ARCHITECTURE.md`, `docs/STATUS.md`, and
`docs/TRANSITION_FORMAT.md` before changing pinned interfaces or transition
serialization.
