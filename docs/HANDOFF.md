# Gravitino Agent Handoff

Updated: 2026-08-18 (Asia/Singapore)

## Current state

All queued DJ-controller follow-ups through Saved Loops, true platter scratch,
Quantize-aware manual loops, phase-only SYNC, and transition-edge loading are
integrated in the working tree. The stable pre-work baseline is commit
`1b9958e feat: expand transition workflow and FLX4 integration`; the changes
described below are intentionally uncommitted pending the user's next
checkpoint request.

The user explicitly permits stopping a running Gravitino process when a
restart is needed. Always resolve the process narrowly with
`ps -axo pid,etime,command | rg '[b]uild/gravitino$'` so Serato or unrelated
audio programs are never touched.

## Implemented in this working tree

- **Post-transition FLX4 soft takeover:** Perform tracks every replay/setup
  change to the physical TEMPO, channel fader, TRIM, HIGH/MID/LOW, FILTER, and
  crossfader controls. At the final event, the entire FLX4→Gravitino input link
  freezes if any physical position differs. Only pickup moves are consumed
  until each control reaches or crosses its target; the pickup event itself is
  not applied, so audio never jumps. A blinking white status alert lists
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
- **Space-efficient, draggable transition workspace:** each mixer channel's
  HI/MID/LOW EQ knobs are stacked vertically, reducing the mixer's horizontal
  footprint. A persistent horizontal splitter resizes mixer versus transition
  workspace; inside the transition panel, persistent handles independently
  resize the transition list, event sequence, and controls. The existing
  transition/library vertical splitter now persists its position as well.
- **Configurable Close Enough transition setup:** the transition panel has a
  persisted CLOSE ENOUGH checkbox plus a TOLERANCE… dialog. Strict matching
  remains the default. When enabled, BPM, volume controls (channel fader,
  trim, and crossfader), and LOW/MID/HIGH EQ can differ from the recorded
  pre-state by independently configurable margins (defaults: ±0.5 BPM,
  ±5% volume, ±5% EQ). Track identity, transport/loop state, FX state and
  type, filters, stems, cue/loop timing, and other setup remain strict. The
  live readiness label says explicitly when a state was accepted only because
  it was close enough; PRIME uses the same readiness policy after its robust
  pre-state preparation.
- **Combined SAMPLER / SAVED LOOPS bank:** the separate host-only SAVED LOOP
  menu was folded into the FLX4's real SAMPLER bank and old saved-loop mode
  selections migrate automatically. Each pad can retain a sampler-file
  assignment while its per-track saved loop takes operational/visual priority.
  An empty pad captures the active loop. Every press of a filled saved-loop pad
  jumps to the stored start and plays, including retriggering an already-active
  loop like a one-shot hot cue; LOOP EXIT is the explicit way to leave it.
  Right-click exposes loop replace/rename/clear and sampler assign/clear
  together, and the combined occupancy is reflected in the physical pad LEDs.
  The saved-loop retrigger also publishes an idempotent PLAY gesture after
  seeking, so transition recordings store the incoming start event and capture
  the loop IN beat as their TO replay anchor instead of inferring it from a
  later control move.
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
- **Saved loop persistence:** eight exact loop bounds and labels persist per
  track alongside hot cues and survive rescans without overwriting newer
  grid/analysis metadata. They are exposed through the combined
  SAMPLER / SAVED LOOPS bank described above.
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
`tests/test_soft_takeover.cpp` covers matching-at-arm, unknown positions,
input freezing, target crossing, consumed pickup events, and software
retargeting; transition coverage verifies continuous tutorial prompts remain
active until their recorded value is reached.
The verified UI
binary is running as the sole `./build/gravitino` process (PID 12512 at this
handoff); resolve its current PID narrowly before any future restart.

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
6. In SAMPLER / SAVED LOOPS, save, one-touch start, rename, and clear loop
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
10. Drag mixer/transition, transition-list/event/control, and
    transition/library handles; restart and confirm all three sizes persist.
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
13. Confirm System/MacBook/Bluetooth master output and FLX4 headphone cue after
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
