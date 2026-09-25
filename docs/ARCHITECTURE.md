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
The `--set-builder`, `--check-set` and `--render-set` utilities also run before
the guard: none opens an audio device or takes the live editor MASTER lease.

After QApplication construction, `QtAccessibilityWorkaround.mm` installs a
process-local compatibility repair for Cocoa on Qt 6.11.0/6.11.1 only. Qt's
synthesized table rows, columns and placeholder cells borrow the table's
accessibility ID, but these releases remove that ID when disposing them.
This can recursively destroy a table/cell during a proxy layout refresh
(`QAccessibleCache::deleteInterface`), including the playing-FROM timer.
The repair checks the runtime version, Objective-C ivar layout and native-only
cache-eviction symbol, then guards the two native cleanup methods. Synthesized
elements never remove their borrowed table ID. Retiring real Cocoa cells evicts
only their native representation through `removeAccessibleElement`, not their
Qt interface: `QAccessibleTable` still owns/references those IDs during search
filter row removals. Native identity checks avoid evicting newer replacements;
Qt remains responsible for deleting interfaces on row removal/reset/destruction.
The compatibility target requires matching Qt GuiPrivate headers. Its one
non-public eviction method is resolved only on the reviewed runtime, before any
hooks are installed; no private cache layout is read or written.
It neither disables accessibility nor changes sorting/model notifications,
and does not modify the installed Qt libraries. Offscreen/other Qt versions
are untouched. Review/remove this narrow workaround when upgrading Qt; do not
blindly widen its version gate. The relevant upstream implementation is
[Qt's Cocoa accessibility element](https://code.qt.io/cgit/qt/qtbase.git/tree/src/plugins/platforms/cocoa/qcocoaaccessibilityelement.mm?h=v6.11.1).
`test_library_accessibility_native` exercises the native bridge with temporary
fixtures (requires a macOS GUI session), including explicit autorelease-pool
draining after native placeholders become real cells, repeated text searches,
hidden-tab filtering and final cell-ID cleanup. Merely calling processEvents
can leave native rows pending in an outer pool and miss the search crash. The
offscreen counterpart alone cannot catch this bug.
Its explicit `--native --unpatched` diagnostic mode reproduces
the old failure and must not be used as a passing test.

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
  the old source with the replacement. All four stem levels reset to unity on
  that deck, so an old mute/solo cannot leak into the next song. The other deck,
  EQ/faders and permanent cue metadata are unchanged. Attaching prepared stems
  does not reset levels; authored transition setup may restore its own levels.
- **Transition-editor preview**: the GUI thread renders a private two-deck
  `AudioEngine` into a bounded lock-free stereo ring. While its exclusive
  preview lease is held, the live callback reads that ring into MASTER and
  neither renders nor advances the live decks. All live ControlBus origins and
  deck-loading UI are blocked for the lease. Lease release drains any
  in-flight callback before the private source can be destroyed. The callback
  remains allocation- and lock-free; preview bypasses the live master tap.
- **MIDI thread** (RtMidi callback): converts raw MIDI → ControlEvent, posts to
  GUI thread via queued signal. LEDs written directly from GUI thread.
- **Library analysis**: a GUI-owned pending/urgent queue feeds QtConcurrent
  workers. Three background jobs leave the fourth worker slot available for
  interactive loads; requests promote/deduplicate a pending row, or retry an
  error. Already-running work is not restarted. Additional urgent requests run
  before ordinary queued songs as capacity becomes available. Generation-scoped
  cancellation stops superseded scans; the library destructor cancels and joins
  its workers before QObject teardown. Results and throttled progress (at most
  10 Hz per worker plus stage changes) are delivered on the GUI thread.
- Transition Player runs on a GUI-thread QTimer (~5 ms) reading the master
  deck's beat position from the audio engine (atomic double).
  Its event-step implementation is also used by editor audition through the
  internal external-clock adapter in `TransitionPlayerExt.h`; preview no
  longer owns a second scheduler/dispatch implementation. The pinned player
  header adds private adapter friends only; public signals/arm remain unchanged.
- `Deck::loopWrappedSeconds()` is a read-only cumulative counter of source
  time skipped by actual forward loop wraps. Audio publishes it only after a
  successful position commit; seeks, scratches, and paused renders do not
  increment it. The player samples the counter before position, so PRIME can
  recognize an entry crossed between polls even if one or more loops have
  already wrapped back below the anchor. The counter is runtime-only.

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
- Touch-gated platter scratching follows an atomic physical-position target
  using two sample-clock 6 ms smoothers (roughly 12 ms group delay). Sparse MIDI
  packets no longer become isolated callback-sized bursts separated by silence.
  Scratch-only 64-tap, speed-adaptive windowed-sinc resampling suppresses aliasing
  in both directions; lookup tables are constructed before audio starts, with
  no callback allocations or locks. Near-zero motion fades to silence and
  64-sample de-clicks bridge touch/release, including key-lock re-entry. Release
  commits the final hand position, even for packets arriving between callbacks.
  Seeks/reloads reset the smoothing state; active loops retain their clamp bounds.
  Untouched mouse-platter input remains an exact fine positional nudge without
  pausing playback; jog-rim tempo bending and transition schemas are unchanged.
- EQ: RBJ biquad low-shelf 250 Hz / peak 1 kHz / high-shelf 4 kHz, ±26 dB with
  full-kill at slider bottom.
- Limiter: soft-clip tanh on master to avoid inter-deck clipping.
- Crossfader: the compact mixer has an **OFF** checkbox, checked on every
  launch. Bypass uses the existing equal-power center gains (no gain boost)
  and ignores mouse/MIDI crossfader moves; channel faders remain authoritative.
  `CrossfaderEnabled` is an appended, local-only ControlBus state control,
  excluded from capture, executable events and portable timeline validation.
  Enabling starts centered and independently arms MIDI pickup; disabling
  releases only that pickup gate and removes crossfader from hardware sync
  scoring. Editor preview copies this runtime context. No transition schema
  or user recipe is rewritten.
- Settings > Audio Output selects the persisted CoreAudio master device; the
  initial default follows macOS, so MacBook and Bluetooth speakers work.
  Selecting DDJ-FLX4 uses one four-channel stream (master 1/2, phones 3/4).
  Selecting another master device opens a second four-channel FLX4 stream and
  feeds only its phones 3/4 from a bounded lock-free cue ring. This prevents a
  second audio callback from advancing/rendering either deck again.
  `AudioEngine::refreshOutputDevices()` runs on a one-second GUI-thread timer
  after realtime output is requested, independent of MIDI connection. It
  resolves System Default to a concrete endpoint ID, follows default changes
  (including stereo/four-channel changes), and reopens stopped/interrupted or
  callback-stalled streams without reloading or rewinding decks. Notification
  callbacks only set atomics; no device lifecycle work occurs on CoreAudio's
  callback thread. A three-second heartbeat watchdog covers backends that
  continue reporting started after callbacks stop. An explicitly selected
  missing endpoint remains selected and silent until it returns; it does not
  fall back to speakers, including at launch. Failed manual switches restore
  the previous preference. Offline engines and intentional stopDevice() never
  initiate recovery. The same timer retries the secondary FLX4 stream even if
  CoreMIDI and CoreAudio enumerate at different times. The Audio Output menu includes
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

Cache reuse/preservation verifies audio identity independently of modification
time. `TrackData::decodedAudioSha256` and the optional same-named cache-v2 field
contain `gvpcm1:` plus a SHA-256 over a format-domain prefix and decoded
48 kHz stereo float32 samples in little-endian order. Only terminal stereo
frames of digital silence/subnormal decoder residue are excluded: MP3 tag edits
can change this inaudible end-padding. Leading/interior silence and every
normal-valued sample (even extremely quiet ones) remain exact and position-sensitive.
This does not alter playback PCM. It is local safety evidence,
not an encode-tolerant song fingerprint, and never enters `.transition` or `.gvt`.
An exact decoded match permits tag/timestamp changes while retaining the whole
effective grid, cues and saved loops and refreshing tags/file hash. Legacy caches
without PCM identity can use an exact asset-byte SHA; caches predating both
hashes are migrated only with unchanged timestamp, matching historical fingerprint
and exact decoded duration. A changed legacy file without sufficient evidence is
refused, not guessed safe from approximate structural similarity. Analysis-version
migrations preserve every existing effective grid, including automatic grids
that saved transitions may already reference; refreshed detector results remain
separate until the user explicitly changes the effective grid.

Different or unverifiable audio at an existing cached path is left unloaded with
a persistent error tooltip and a message on LOAD; the old cache and catalog entry
stay untouched. Restore the original audio or give a replacement its own filename
and review/bind its grid. Corrupt caches and failed verification/write/backups
also fail closed. Before any changed cache write (including manual setup edits),
the exact previous JSON bytes are atomically archived under
`~/.gravitino/cache/preserved/<path-hash>-<content-hash>.json`; duplicate snapshots
are reused, never overwritten with different contents. These are local backups,
not transition files. Writers are serialized and automatic refresh checks its
starting cache snapshot so it cannot overwrite a newer saved edit. A loaded deck
cannot save setup against a file whose timestamp has since changed: reload first.
Scans reject files whose timestamp/size changes during processing. No automatic
recovery is inferred from transition endpoint assumptions, so a newer intentional
downbeat correction is never replaced by an older recipe's reference downbeat.

## Transition record/replay

`TransitionStore` watches both supported extensions and the library directory,
debounces file events (including atomic replacement saves), and uses a two-second
metadata fallback. Content hashes avoid reparsing unchanged recipes and needless
model resets. `aboutToChange` precedes vector replacement, then `changed` refreshes
the library/graph. Invalid updated files are excluded rather than launching an old
cached recipe. TransitionPanel owns copies, not pointers into the store, and defers
its refresh while armed/running until finish/abort. Editor preview already owns its
take; live reload never changes that audio graph. A clean idle editor reloads saved
changes, preserving the cursor; dirty/staged edits, focused fields and active or
paused audition defer to the explicit **Reload Saved** action with confirmation.
Save-time conflict detection remains in place. None of this writes source recipes.

Canonical coordinates and the local grid's downbeat phase are distinct: a local
asset may carry a fractional `canonicalBeatOffset`. Timeline/sample-editor grids
draw the actual local beat lines at that offset (including four-beat emphasis),
not fabricated integer canonical lines. An explicitly approved local anchor change
can preserve every canonical source timestamp by changing the catalog offset by
the same beat delta. Never perform that user-data repair implicitly.

See `docs/TRANSITION_FORMAT.md` for the file format. Runtime flow:

1. User loads track A (playing) and track B, arms **Record Transition**.
2. Recorder notes the *anchor*: beat position in A when recording starts, and
   the first beat position at which B is playing. It also captures a complete
   role-based pre-transition snapshot: both decks' transport/cue, tempo,
   channel/EQ/filter, loop, FX and stem state. TRIM and crossfader are
   deliberately excluded because input gain and master blending remain live
   DJ state.
3. Every executable transition ControlEvent is logged with a timestamp in
   **beats relative to anchor** (master-deck beats). Beats, not seconds — so a
   transition recorded at 120 BPM replays correctly at 128. Crossfader, trim,
   jog, browsing, and monitor-only input are filtered at capture. When a
   performance pad caused the audible event,
   the recorder also stores an optional physical input hint. Replay
   remains state-based; Tutorial can therefore say “CUSTOM pad 3” instead of
   misleadingly reducing the gesture to its resulting `play` event.
4. Stop recording → capture the exact authored completion beat,
   normalize/thin continuous streams → save a typed v1 YAML
   `.transition`. Semantic hot cues and saved loops (canonical IN/OUT beats)
   are allocated together into an isolated temporary CUSTOM bank without
   changing permanent per-track cues or loop slots. The deck's CUSTOM selector
   explicitly distinguishes NORMAL (editable track loops/custom audio) from
   TRANSITION (protected temporary slots). Bank selection changes pad display,
   input routing, and LEDs, not replay's isolated slots. Held pads are released
   through their original bank before switching, and routine transition-bank
   refreshes preserve the user's choice.
5. Replay: user loads the same pair (confirmed catalog binding, encode-tolerant
   structural fingerprint, checked recording ID, or explicit manual confirmation), picks
   a transition, then uses **Perform** to reconstruct the recorded pre-state
   and roll from the anchor, or **Prime** to prepare it while retaining A's live
   position and wait for A to cross the anchor. The player reasserts incoming
   transport at that boundary, then fires events on schedule with linear/
   s-curve interpolation between sparse values.
   Waiting is seeded afresh on every arm, including retries after setup
   failures. Actual audio loop-wrap distance bridges otherwise-missed entry
   crossings; a backward seek alone never counts as a wrap or starts replay.
6. `MidiEngine` keeps a shadow for every absolute musical control: last
   physical value/known state, current software truth, and independent pickup
   state. Perform records which executable controls setup/replay changed and
   arms pickup for those controls afterward. Manual FREEZE HW is separate: it
   keeps observing physical tempo, channel fader, trim, EQ, filter, FX wet, and
   crossfader values while preventing only those inputs from reaching audio;
   buttons, pads, transport, loading, and browsing stay live. Unfreezing arms
   every mismatched or unknown absolute control. Each control regains authority
   only after reaching software, and its crossing packet is consumed, so one
   knob never waits for another and no pickup creates an audible jump.

The top workspace is `Deck A | compact FLX4 mixer | Deck B`; the centered mixer
starts below the overview-waveform baseline, followed by the full-width detail
waveform. Each deck includes a position-driven rotating platter and a
two-decimal canonical beat counter. A mouse drag is projected onto a diagonal
axis: bottom-left moves forward, top-right moves backward, and perpendicular
motion is ignored. Each projected pixel sends 0.02 `PlatterScratch` tick
(0.2 ms) without `PlatterTouch`, selecting the engine's direct
position-adjustment path
without changing PLAY state. Pointer deltas are incremental, so direction can
reverse within a gesture while the disc rotation continues to follow actual
track position. The FLX4's touch-gated mapping is unchanged. Transient pad-bank
feedback is fixed-height, horizontally ignored, and elided with its full text
in a tooltip, so long CUSTOM help cannot alter the deck's width. A mapped
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
launch gestures become one instruction. Portable launch recognition follows
the semantic cue ID and its allocated temporary pad, so mixer and other-deck
events may interleave without splitting the gesture. Human mode also defaults
to showing every condensed action start as a labeled cue on the relevant
deck's overview and zoomed waveforms; a header checkbox can hide them. Raw
mode retains the prior recorded event table; both views derive from the same
typed timeline and never alter serialized checkpoints.
The mixer's former top spacer is a hardware-state strip. It distinguishes
SYNC, MISMATCH, PARTIAL/UNKNOWN, and FROZEN; the SHOW HW checkbox adds cyan
last-reported physical markers/tooltips, GET HW STATE refreshes the MIDI
connection and reports snapshot completeness, while FREEZE HW explicitly decouples absolute
hardware from authoritative software. Freeze is session-only, survives
controller hot-plugging, blocks Tutor View, and cannot be enabled while Tutor
View is open.
All three hardware-state controls are disabled while no controller is
connected. The virtual FLX4 transport follows the physical vertical stack on
both decks: a small SHIFT button above CUE above the larger PLAY/PAUSE button.
There is no invented VINYL-mode button because the FLX4 surface does not have
one.
The Library's Transitions tab keeps legacy and portable files as distinct rows
and exposes independent `.gvt` and `.transition` checkboxes. Portable files are
shown by default; legacy `.gvt` rows are opt-in so converted counterparts do
not clutter normal use. This makes migration comparisons available without
hiding the legacy source. `--convert-transitions` creates only missing portable
counterparts and is safe to run repeatedly. The crate sidebar also includes an
automatic **Undone** smart folder: once a track is analyzed, it appears there
only when the catalog's rebuildable reverse graph has neither an incoming nor
outgoing transition for its canonical song identity. Transition reloads and
track-analysis completion refresh this coverage without touching audio tags.
Each song's Status cell paints an analysis progress bar, including empty
**Queued · 0%** tracks. `TrackLibrary::prioritizeAnalysis(row)` is the shared
path for mouse/double-click/hardware LOAD requests. Internal progress callbacks
report decoding, waveform, fingerprint, BPM and key stages; the model exposes
fraction/active/error roles without changing the six table columns. Fractions
are monotonically weighted work estimates, not a remaining-time prediction;
100% is published only after successful library registration. Cache hits report
only decoding/waveform work. Decoding measures compressed-file read position
through the same miniaudio decoder rather than querying MP3 frame length (which
can decode the entire file again). PCM parity with the original file reader is
tested across supported formats, VBR MP3 and missing-length-header MP3.

LOAD stays available for queued tracks on stopped decks. `LibraryWidget` keeps
one pending path per deck and automatically loads that exact song once ready,
even if the table has since been searched or sorted. It does not start playback.
A newer load replaces that request; playing/changed decks, disabled live UI,
an exclusive editor preview lease, or a library reset cancel it. Worker errors
clear the pending load and expose a retryable diagnostic. Background failures
remain visible in the row tooltip. Permanent grids, hot cues/saved loops,
transition files, catalog matching rules and cache schema are unchanged.
This is queue responsiveness, not lazy loading: directory discovery is still
synchronous, and each analyzed track is still fully decoded into memory.

Transition selection uses `TrackLibrary::profileAt` / `SongCatalog::assetProfile`
to resolve already-catalogued endpoints before their PCM is ready. These cached
profiles are identity hints, never playback-ready tracks. Clicking an edge
prioritizes both songs and stages an owned recipe snapshot; neither stopped
deck is replaced until both decoded tracks pass fresh endpoint matching.
Changed/playing decks, a newer selection, a library/store reset, disabled live
UI, or an editor lease cancel the pending pair. A matching playing outgoing
deck can be retained. The transition table's Status column reports OUT/IN
analysis progress, errors or missing matches, then Ready (2/2 songs); this is
audio-analysis readiness, not a promise that optional stems are prepared.

Library sorting uses typed model roles: BPM and duration compare numerically,
while Camelot keys compare by their numeric wheel position and A/B suffix.
The Library page's default-on **Recommended** mode is a view-only priority
layer over that chosen sort. While either deck is playing, candidates matching
the exact Camelot code and a native BPM within an inclusive +/-10 range form
the first tier; BPM-only matches form the second; all other songs retain the
normal third tier. A candidate already loaded on a playing deck is not treated
as its own recommendation. With two playing decks, its best result against
either is used. The user's selected column and direction remain the secondary
order inside every tier, and the delegate draws boundaries only where adjacent
visible tiers change. Disabling Recommended, or having no playing deck,
restores the ungrouped sort. The transition edge list uses the same boundary
treatment between its pinned playing-FROM group and other edges.
A Graph button immediately left of New opens a separate force-directed set
planner. Canonical endpoint identities become labeled song nodes and logical
transitions become directed edges labeled with their transition names; a
legacy source and its converted portable counterpart share one logical edge.
Edge names are small, faint captions without boxes, leaving song titles and
route highlights visually dominant. Light pairwise repulsion, edge
springs, center gravity, node dragging, panning, and zooming keep the graph
organizable. Hover computes and highlights the longest-duration feasible simple
route, excluding repeated songs so cycles remain finite and excluding a next
edge when its outgoing anchor has already passed at the carried arrival beat.
The estimate starts at the source asset's beginning, uses native BPM for the
music between transitions, uses each edge's authored end beat/master BPM for
the overlap, carries the incoming launch position into the next song, and
includes the final song's remaining duration.
Songs currently loaded on Deck A and Deck B retain cyan and magenta outer
highlights, respectively, with explicit A/B badges. The graph polls the two
live deck track identities while its window is open, so direct loads, replay
loads, and controller-driven loads all update without coupling the audio engine
to graph UI signals. These deck markers remain visible underneath the separate
hover-route treatment.
During Perform and Tutorial, every positive-duration row receives a faint cyan
track whose maximum width is normalized against the longest gap in the current
Human or Raw sequence. Only the active row fills, reaching its track end at the
next distinct action beat. A derived, non-actionable beat-zero “Transition
starts” row provides the first countdown interval; simultaneous actions do not
create fake intervals. Neither the marker nor its progress is written to
`.transition` or legacy `.gvt`.
The full-size Transition Editor is the creation/editing surface for the same
typed model consumed by replay. A single undoable working copy drives two
waveforms, an action lane, independent `(role, control)` automation lanes,
semantic cue/loop and initial-state inspectors, and an advanced safe-YAML
view. Timeline points, cue markers, loop edges, labels, and the explicit END
marker are directly draggable; grid snapping never changes stored precision.
The inspector uses a persistent section selector, not an overflowing tab strip,
with Tempo / Setup, Cues / Loops and All fields shortcuts. Initial State exposes
incoming playback BPM/ratio directly; Cues / Loops exposes the selected start,
end and calculated repeat length without horizontally scrolling the table.
Numbers in these controls, editable tables and YAML use shortest round-trippable
decimals, retaining full double precision. `TransitionFieldsEditor` builds a searchable typed tree from
`transitionDocumentFields`, the same structured root used by YAML serialization.
It exposes every saved field, including unknown mappings, arrays, metadata and
extensions; format/version are visible but read-only. Compatibility crossfader
fields are visible only as inert data here, not restored to playback controls.
Field changes are staged, validated with the existing safe parser, then applied
as one document/Undo operation. Preview and Save first apply a valid field draft;
concurrent document edits block stale draft application. Leaving offers an
explicit apply/discard/cancel choice. Only Save writes the transition, and
identity/endpoint changes still require Save As. Endpoint edits and Undo/Redo
re-resolve preview assets to avoid auditioning the previous endpoint's audio.
Its stacked waveforms are projected through a derived per-deck transport trace
rather than a linear anchor offset. Audible loop passes therefore unroll on the
monotonic transition clock with repeated canonical beat grids and pass badges;
stopped spans stay dark and unused saved loops remain definition overlays. The
trace and repeat counts are editor data only and never change the serialized
transition. Plain wheel input scrolls vertically, Shift-wheel pans horizontally,
and Command-wheel zooms around the beat beneath the pointer. Native two-axis
trackpad movement retains both axes. Selecting a timeline event opens its exact Events
inspector and retains identity across chronological re-sorting. Placing the
playhead between events selects and centers the next executable action; the
same selection follows preview playback, while compatibility-only crossfader
rows stay hidden. Deleting an automation point selects the next point in its
own `(role, control)` lane, or the previous point in that lane at the end; an
empty lane clears selection instead of targeting another knob/deck. Discrete
action-card deletion retains chronological following. Both the button and
Delete/Backspace keys support rapid cleanup.
The Events inspector defaults to **Auto apply**, beside Apply: finished number
or reference edits and selector changes update the undoable working copy, not
the saved file. Turning it off stages changes for explicit Apply. Refresh,
selection and Undo/Redo cannot trigger auto-apply, and unchanged Apply does not
create duplicate undo steps. Changing a point to a non-cue control clears its
obsolete cue/loop reference instead of silently coercing it back to a cue.
Continuous-to-continuous type edits retain the ramp curve; incomplete cue IDs
report a non-modal error without mutating the model.
Starting preview reconstructs cursor state by rendering the private graph from
beat zero, then routes only that graph to MASTER. It can resolve the currently
loaded deck assets while the library scan catches up and does not mutate the
application-wide cursor during preparation. A Write Automation take
punch-replaces touched streams as one undo command. Autosave drafts, source
hash conflict detection, forced Save As for legacy/endpoint edits, and
schema-level validation keep library files non-destructive and reopenable.
Live setup and editor audition now share `TransitionPlayback` preparation,
including Tutor's transport-only restrictions. Perform and preview both use
the outgoing anchor and roll that deck; historical outgoing initial position
and playing fields remain preserved, not alternative playback-start controls.
The transport trace follows that same entry rule. `TransitionPlayer` supplies
both paths' cue/loop allocation, grid-adjusted tempo, setup events, held-cue
PLAY latch, interpolation, and compatibility exclusions. Complete starting
value lookups include FX and stems, including legacy partial-state ramps.
Preview inherits role-mapped live trim, key lock and crossfader (which remain
outside the file), without changing the live engine or permanent song cues.
Its ring carries musical timestamps, so the cursor follows consumed audio,
not the producer's buffered future. Cursor reconstruction clips the last
render to a sample instead of overshooting by a whole preview block.

Parity means the same recipe and starting controls: synthetic offline renders
and the actual editor MASTER-ring path are compared at identical clock steps.
It is not a promise of sample-identical live-device output. Live Perform still
dispatches on the GUI timer, preview dispatches at render-block boundaries,
PRIME intentionally retains accepted live outgoing tempo, and an already
playing deck can have FX/stretch history or manual changes absent from a fresh
audition. Audio-clock event scheduling and a defined DSP-history policy remain
necessary for a stronger sample-accurate guarantee.

The editor explicitly labels WHEN (elapsed transition beats) versus WHERE
(canonical song beats), explains value units and ramp destinations, and links
launch gestures to their source cue. A default-on gesture checkbox moves cue
press/PLAY/release together on an inspector timing edit; raw editing remains
available, sort preserves same-beat order/selection, and the group edit is one
undo step. Outgoing setup position/playing/tempo are read-only derived values
in Initial State; Transition details edits their authoritative controls.
Drag snapping defaults Off; precise numeric fields step by 0.01 beat. Optional
controller teaching hints collapse to keep the common inspector compact.
During cue, loop-boundary, label, action or automation drags, a thin cyan
center guide crosses both waveform lanes and the automation area. Its live
readout distinguishes source-song beats from transition time and stays inside
the visible scroll viewport. The guide follows actual snapping/clamping and
the selected loop repetition, not the pointer's off-center grab position.
Discrete action cards now follow the pending drag too. This is paint-only
feedback until release, which remains one undoable edit; no replay/data format
or permanent song-cue changes are involved.
The editor also offers whole-mix keyboard transport: held C auditions from the
cursor and returns on release, Space while held latches that audition, and an
ordinary Space starts or pauses preview. The play-from-cursor button establishes
the same audition cue; STOP and natural preview completion return the playhead
to it, while pause/resume preserves it. These shortcuts work after clicking
buttons or tables, while text and value editors retain normal keyboard input.
Selecting a transition with stem controls or non-unity initial stem levels
shows a setup warning and PREPARE STEMS action for the affected physical decks;
Perform and Prime remain gated until those stems are attached rather than
silently falling back to full-track audio.
PRIME mismatches use a separate amber layer on the main controls: sliders get
an exact target line/handle, dials a radial marker, and discrete states a target
tooltip. Targets are recomputed continuously and disappear individually when
resolved. Tempo-range changes from either the UI or MIDI immediately reproject
the amber marker, including a target previously clipped at the old range limit;
the overlay cache includes displayed fractions as well as authored values.
A failed PRIME keeps useful targets visible until correction,
selection change, or Abort. Outside Tutor, PRIME may restore deck setup (never
crossfader). Tutor PRIME and Tutor Perform prepare only transport positions,
cue positions, loop bounds, and temporary transition cues/loops; hardware-
facing musical controls must be matched physically. Tutor PRIME is strict and
requires a second explicit click after correction, while Tutor Perform treats
setup differences as advisory. During an armed/running Tutorial, a distinct
green layer interpolates authored step/linear/S-curve targets on the main
controls without changing audio or scoring.
Discrete loop failures are action-oriented: an unwanted active loop names the
FLX4 4 BEAT/EXIT button, while a required inactive loop names its allocated
CUSTOM pad when available or gives the exact LOOP IN/OUT beats. Tutor setup
also preserves a currently active loop and its bounds across position
preparation, preventing the preflight itself from silently resolving the
mismatch.
The virtual FLX4 mirrors live controls, pad state, LEDs, and channel meters;
prose/countdown/reset guidance lives in the transition control panel above
CLOSE ENOUGH. Perform gives the guided run up to eight beats of pre-anchor
countdown; Prime arms the same guidance against the live outgoing deck.

### CUSTOM stem-echo actions

NORMAL CUSTOM pads offer Saved loop / custom audio, Vocal Echo and Instrumental
Echo through their right-click Pad action menu. The optional local assignment
action is validated only for CUSTOM; older settings retain saved-loop/audio
behavior. Switching actions preserves track cues, saved loops and assigned audio
paths. TRANSITION CUSTOM remains protected. Wet level and beat length are
configurable, defaulting to 50% and half a beat.

Stem echo is momentary: Vocal Echo enables vocals and mutes melody/bass/drums;
Instrumental Echo does the inverse. Press sends those four stem levels plus
echo type, wet, beat length and enable through ControlBus. Release restores
the previous FX settings and all four fractional stem levels. Switching modes,
banks or overlapping FX pads also releases the held action. Reloading a track
discards the old snapshot so a late release cannot overwrite the new deck's
state. Without attached stems, the pad explains preparation and changes nothing.

Every press/restoration action carries its pad input hint. The recorder retains
these as discrete steps, including quick taps, and thins subsequent manual knob
movements separately. Replay uses ordinary FX/stem controls, not a local pad
assignment. No transition schema, control ID or capability change is needed.

### Tone-play authoring and shared sampler

The editor's Tone play workspace separates a canonical outgoing-song source
slice ruler from a transition-beat piano roll. Drag IN/OUT or type exact source
beats; draw, move, resize, delete or duplicate notes and edit pitch/start/gate/
velocity. The same document undo stack and guarded Save/Save As path own edits.
Source navigation has independent −/+ zoom buttons, Fit slice, Whole song,
pointer-anchored Command-wheel zoom, wheel panning and a horizontal scrollbar.
Opening a stored slice or enabling a new instrument fits the source selection;
navigation never changes its stored bounds. START/END handles remain separate
even at overview scale; fractional grid labels and PCM detail support close
trimming. The source header shows seconds and beats; Preview slice auditions
the source at its original pitch, distinct from Preview mix.

The piano roll supports ruler-drag bar ranges (using the outgoing meter),
rectangle selection and Shift-click membership. Copy/Paste (Command-C/V) keeps
relative timing, pitch, gates, velocity, unknown note fields and a selected
range's leading/trailing silence. Paste begins at the cyan cursor and advances
it by the copied range for repeated phrases. Duplicate, move, resize and Delete
operate on the selected group; each edit is one undo command. Clipboard payloads
are size-bounded and validated before mutation, and never replace source slice
or instrument settings. Snap exposes 1, 1/2 … 1/64 beat and Off; drawing uses that
length instead of imposing a quarter-beat gate. Piano zoom now reaches 1024 px
per beat so a 1/64-beat note has a usable resize edge. Minimum note duration
remains the existing 1/64 beat; no format or playback semantic change is needed.
Piano keys audition one isolated pitched note through the exclusive MASTER
lease; returning restores the prior editing cue and cannot write an automation
take. Preview mix/C/Space reuse normal editor audition. No automatic migration,
source-audio writing, beatgrid edit or permanent cue change is involved.

`TonePlayPattern`/`TonePlayNote` are optional typed transition data, with safe
range/polyphony validation and unknown-field retention. `tone-play.v1` gates
compatibility. Source slices use canonical track coordinates even while editing
a legacy recipe; the recipe's transition anchor keeps its normal format-aware
conversion. Enabled tone patterns refuse Tutorial in both UI and player, with
an explanation on the disabled Tutor button. Legacy `.gvt` writes reject tone
data before opening the target; modern editor saves remain separate YAML files.

`AudioEngine::prepareTonePlay/clearTonePlay/tonePlayBeat` manage a GUI-prepared
immutable PCM slice and schedule. `TransitionPlayer` prepares it at arm and
uses the appended, runtime-only `TonePlayEnable` ControlBus action to enable or
stop it. That action is excluded from recording, timeline parsing and editable
lanes. `TonePlayProcessor` owns a fixed 16-voice array and the scratch path's
band-limited resampler, initialized off the callback. A seq_cst reader gate
drains callbacks before replacing/freeing source data; the audio thread never
allocates or waits on a lock. Notes are sample-clocked from the outgoing entry
crossing; subsequent loops, seeks and stops do not retrigger the melody. The
clock integrates outgoing tempo, and live automation reads that same published
beat. Ordinary non-tone transition scheduling remains unchanged.

Sample voices join the outgoing deck's post-fader master and PFL streams, with
their own gain, before crossfade and limiting. They layer by default; optional
replacement ducks only original outgoing audio across the authored note phrase.
They intentionally do not inherit the song's EQ/FX/stem/fader processing. A
classic one-shot pitch shift also changes slice playback length; gates only
shorten it. Preview, Perform and set export share this engine path, and the WAV
manifest embeds the same YAML pattern. Tests protect fractional onset, octave
pitch, block-size invariance, loop/stop clock continuity, safe replacement,
both physical deck mappings, real editor-ring parity and exported audio/data.

The optional `TonePlaySustain` adds one independent background vowel beneath
the foreground notes, gated by `tone-play-sustain.v1`. Its canonical loop IN/OUT
stay inside the original slice; transition start/length, pitch, gain and fade
times are independent of the piano-roll events. Preparation crossfades one
periodic cycle in immutable memory. The audio thread uses a dedicated single
voice with periodic band-limited reads and sample-clock attack/release, never
retriggering notes or changing deck loops. Both layers share the outgoing
sampler clock and route. No allocation, locking or unbounded growth occurs in
the callback. End/replace-outgoing calculations include enabled holds.

The editor's BACKGROUND HOLD panel exposes these parameters, a purple vowel
selection mode inside the amber source slice, and isolated Preview hold through
the same MASTER lease. Preview slice/piano keys explicitly exclude the hold.
A purple dashed band behind the green foreground notes shows the held interval.
All hold edits use document validation/Undo; trimming the source so it excludes
the hold is refused, not silently remapped. All-fields/YAML sees the same model.
The background region and crossfade are authored, not automatic pitch detection;
auditioning a steady vowel remains necessary for a natural result.

Optional `TonePlayEffects` (`tone-play-effects.v1`) sends only foreground notes
to independent echo/reverb returns using DeckFx, leaving the direct sample
unchanged. Return storage/type selection and wet-only initialization happen in
prepare, off the audio callback. Returns use the sampler's effective tempo and
continue after gates until the bounded, faded tail end. The background hold
never feeds either effect; optional smoothed foreground ducking lowers only
that hold to reduce masking of short pitched notes. All instances remain local
to the sampler and reset on prepare/clear. No shared deck FX state is modified.
Editor slice/key audition includes the full tail; hold-only audition has no
foreground sends. Perform, preview and offline export use the same path.

## Offline set recording

`SetRenderWindow` owns an ordered, read-only snapshot of transition recipes and
metadata-only asset profiles. Its QtConcurrent worker constructs its own
ControlBus, AudioEngine and TransitionPlayer in the worker thread. Progress is
queued back to the GUI; cancellation/destruction joins the worker safely.
Only two assets and their needed stems are decoded at once. Song-catalog grids
and identities are read, never re-analyzed or persisted. Asset SHA-256 and
decoded fingerprints must still match; changed/deleted queued recipes require
an explicit reopen/re-add. No source transition, permanent cue or audio tag is
written. Explicit stem preparation uses the existing cache workflow.

`SetRenderer` resolves a chain of N transitions to N+1 compatible assets. It
rejects missing/ambiguous choices, unavailable stems, unsupported recipes,
stopped/looping incoming handoffs and next anchors already passed. The shared
transport trace predicts these checks. Rendering uses the same setup, Perform
positioning, semantic cue/loop resolution and external-clock TransitionPlayer
as editor preview, not a second control scheduler. Rendering clips blocks at
event boundaries, scheduling discrete launches to within one sample. Existing
live replay remains GUI-timer driven; this does not eliminate that jitter.

The first song starts at file time zero at the first transition's BPM. Each
subsequent solo section integrates a linear-in-time BPM ramp from the actual
incoming exit tempo to the next recipe's master BPM. Its duration for B source
beats and endpoint tempi P,Q is `120*B/(P+Q)` seconds. Automation inside a
transition is unchanged. The same solo interval linearly interpolates the
actual LOW/MID/HIGH knob positions to the next outgoing setup in normalized
knob space (not linear amplitude). All three reach their exact target before
the next setup is applied, preventing the old instantaneous EQ reset. First
song EQ starts at the fresh deck's neutral state; the final tail retains its
ending EQ. A missing captured target retains the live value. An audible
handoff with no renderable solo interval and differing EQ is refused rather
than snapped. Faders, filter, FX and stems are not part of this EQ bridge.
The outgoing deck is retired at its authored end
(legacy end fallback: last executable event + one beat); the incoming state
continues until the next entry. The last song plays to file end. There is no
invented fade, structural remapping or automatic repair of authored moves.
Key lock is a set option (default on); trim defaults to unity and manual
crossfader to center. Crossfader data in recipes remains inert.

`RecordingWav` is a synchronous private disk sink using QSaveFile, distinct from
the live MasterRecorder ring. It never drops ring-buffer blocks or commits a
partial/cancelled recording, refuses an existing output, and enforces RIFF's
4 GiB limit. PCM encoding matches REC MASTER: 48 kHz, stereo, signed 16-bit.
The appended `gvtm` chunk contains UTF-8 JSON `gravitino.recording` version 1:
title/date, output format and frame count, track identities/grids, every
transition's full portable YAML and exact start/end frames, key-lock setting,
solo tempo ramps, and handoff policy. Renderer `offline-set.v2` additionally
records `eq_ramps` with song_index, start/end_frame, linear-time curve,
normalized-knob domain and LOW/MID/HIGH from_values/to_values. These derived
bridges do not alter any embedded recipe or the transition schema. INFO
title/software/date/comment tags and
`cue `/`LIST adtl` chapter markers are independently readable by standard WAV
players. `readRecordingManifest` is a bounded, read-only chunk reader. Recipes
are data, never commands. Live REC MASTER manifest capture and importing these
embedded recipes into the transition library remain separate future work.

Local `.set.json` queue files are `gravitino.set` version 1 with title, key_lock,
ordered transition_paths and optional explicit asset_paths. They contain local
paths and are not portable replacement transition documents. Both .gvt and
.transition load through the format-neutral reader. Song-list autoselection
uses portable files only, avoiding duplicate legacy migrations.

## Protected live queue and preparation

`LiveSetSession` adapts the same `SetRenderer` orchestration to a stereo stream
instead of a WAV sink. No second transition scheduler or BPM/EQ bridge exists.
Its dedicated one-thread producer pool never waits behind library analysis or
stem jobs. A bounded 30-second SPSC float ring provides backpressure on the
producer; the device callback only copies samples using atomic cursors. Startup
and underrun recovery prebuffer two seconds (or the remaining completed tail).
Underruns are visible, not concealed as guaranteed gapless playback. Only two
private decks are decoded; neither shares ControlBus with the preparation decks.
All new tracks are freshly identity-checked, without changing grids or cues.

`AudioEngine::acquireLiveProgram` requires the independent FLX4 phones route and
refuses an active editor lease. The queue exclusively replaces MASTER 1/2;
ordinary decks now render into PHONES 3/4, including their faders/FX. Channel CUE
can still select pre-fader monitoring. Editor exclusive preview freezes only
those preparation decks and goes to PHONES; the program source keeps advancing
and its MASTER tap remains recordable. No controller bus event reaches the
private set graph. A seq_cst callback-reader gate protects external source
lifetimes when leases are released. No new allocation or lock is in the callback.
Both a primary four-channel FLX4 and another master device plus secondary FLX4
phones use the existing routing. Arbitrary non-FLX4 preparation devices are not
implemented. Losing phones never redirects preparation to MASTER. Stop, pause,
EOF, worker failure and unplugging retain isolation; an explicit return refuses
while preparation decks or editor preview are playing. Application shutdown
uses `shutdownLiveProgram` to stop device/recovery and detach/drain the source
before destroying the main-window-owned session/ring. This shutdown-only method
is never used for the user-facing return to MASTER.

The **Live queue / Record set** window reuses song-list/transition selection,
asset choice, stem preparation and `.set.json` plans. Its live controls start,
pause/resume, stop and explicitly leave protection; closing the window only
hides it. Main-window close confirms stopping a running set. An on-screen
PREP badge and alternating FLX4 PLAY/CUE lights persist until isolation ends,
including reconnect and immediate input LED echoes. The warning changes only
illumination, not button behavior. Physical output-volume knobs remain outside
this software isolation guarantee.

Accepted transitions/assets/grids/key-lock are immutable snapshots. **Append**
submits a candidate extension to the producer; it validates the chain and new
recipe bytes at the next song boundary, or during the final song's solo tail.
Removed/reordered/edited accepted entries, ambiguous assets, missing stems and
already-rendered entry points are refused. During the final tail, a new anchor
also needs one source second beyond the producer cursor. The displayed elapsed
time/section follows consumed audio, not the producer's up-to-30-second lead.
This first version is append-only; no skip, replace-next, or automatic fallback
mix is invented. New requests must have a connecting recipe before appending.
`TrackLibrary::addAudioFiles` / **File → Add request audio** insert and prioritize
selected MP3/FLAC/WAV/AIFF files without rescanning existing rows, cancelling
workers or replacing deck references. Added files stay at their original paths;
the session list is not a new persisted crate. Cache/catalog registration uses
the existing protected analysis path.

Headless tests compare stream samples with exported PCM (within WAV quantization),
exercise mid-tail appends, prefix/late-entry refusal, bounded buffering and
cancellation, and verify MASTER remains unchanged during MIDI preparation,
four-channel editor preview and loss of the headphone route. Real FLX4 routing,
physical LEDs and sustained gig-load behavior still require hardware audition.

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
does not arm post-replay takeover. Manual FREEZE HW remains selected across a
disconnect/reconnect, with all physical values returning to unknown until the
controller reports them again. Tutorial's virtual FLX4 diagram still mirrors
and scores gestures, while the main controls carry the independent green live
target layer. Recorded performance-pad `via=` hints illuminate the actual
pad layer and pad number, while transition-critical hot-cue mappings are
checked before a cue can be deleted.
