# Gravitino DJ

Open-source DJ software for people who'd rather perform than rehearse.

Gravitino's core idea: **record a transition once, replay it forever**. Nail the
blend between two songs one time — every fader move, EQ sweep, and tempo nudge is
captured as a portable *transition file* (`.transition`, human-readable YAML). Next time
you load that pair of tracks, pick the recorded transition and Gravitino performs
it beat-perfectly, or teaches it to you hands-on in Tutorial mode. Transitions
are plain text: diff them, edit them, share them.

## Status

MVP targeting macOS (Apple Silicon), built with Qt 6 for eventual
cross-platform support. Local MP3, FLAC, WAV, and AIFF playback. First-class hardware support
for the Pioneer DDJ-FLX4 controller.

## Features (MVP)

- Two decks with waveform display, play/cue/hot cues, tempo slider with beat
  sync; PLAY while holding CUE or a hot cue latches the preview into continuous
  playback
- Selectable CoreAudio output for MacBook, Bluetooth, or DDJ-FLX4 speakers.
  With Mac/Bluetooth master output, a second FLX4 stream still carries
  pre-fader headphone cue; selecting FLX4 routes master 1/2 + phones 3/4
- Mixer: channel faders, 3-band EQ, trim, crossfader
- Automatic BPM detection and beatgrid
- Library browser scanning local MP3, FLAC, WAV, and AIFF files (tags via TagLib)
- **Transition recording & replay** — the headline feature (see
  [docs/TRANSITION_FORMAT.md](docs/TRANSITION_FORMAT.md))
- **Visual transition authoring** — create or edit an edge on synchronized
  outgoing/incoming waveforms, drag actions and automation at fractional beats,
  manage temporary cues/loops and initial state, audition from any cursor, and
  write mouse automation without touching the live decks
- Full virtual DDJ-FLX4 Tutorial overlay: highlights each recorded gesture,
  accepts clicks or physical input, checks referenced hot-cue assignments,
  and shows live green targets on the real deck/mixer controls
- DDJ-FLX4 plug-and-play MIDI mapping with LED feedback (hot-plug supported —
  plug it in any time)
- Live hardware/software sync status with optional cyan physical-position
  ghosts, absolute-control freezing, and jump-free per-control pickup;
  hardware actions stay disabled until a controller is connected

## Build

```sh
brew install qt rtmidi taglib libyaml cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/gravitino            # launch the app
./build/gravitino --selftest # headless: renders a scripted transition to WAV
./build/gravitino --convert-transitions # copy legacy .gvt files to YAML
```

## Using it

1. Launch `./build/gravitino` — it scans `~/Music` (File ▸ Open Music Folder to
   change) and analyzes BPM/beatgrids in the background.
2. Load a track on each deck (Load ▶ A / Load ▶ B or double-click), press PLAY.
   Without a controller, drag a mapped HOT CUE pad onto PLAY and release to
   latch it exactly like holding the pad while pressing PLAY on hardware. Drag
   either deck's rotating platter diagonally: bottom-left moves forward and
   top-right moves backward at 0.2 ms per projected pixel. It shifts the
   playhead without interrupting playback (and leaves a paused deck paused).
   The counter below the disc shows the canonical fractional beat used by
   `.transition` files.
3. **Record a transition**: with the outgoing deck playing, hit ● REC, do your
   blend (channel faders, EQ, sync, hot cues — mouse or FLX4), then
   ■ STOP & SAVE and name it. It lands in `~/Music/Gravitino/Transitions/` as
   a readable `.transition` YAML file you can edit in-app or share. Existing
   `.gvt` files remain readable and are never overwritten during conversion.
   The crossfader always remains manual live-mixer state and is deliberately
   excluded from transition recording and playback.
4. **Replay it**: next time that pair is loaded (matched by confirmed binding,
   structural fingerprint, or a checked recording ID), it appears in the Transitions list —
   ▶ PERFORM executes it beat-perfectly at whatever tempo you're running. If
   the transition uses stems that are not ready, the setup row warns you and
   offers **PREPARE STEMS** before Perform or Prime can start. Use **Graph…**
   beside New to open the separate force-directed set planner; hover any song
   to highlight its longest feasible route and estimated time to the end.
   The Library sidebar's **Undone** smart folder shows analyzed songs with no
   incoming or outgoing transition, so gaps in a planned set are easy to find.
   With **Recommended** enabled, playing a song groups the library into exact
   Camelot-key + nearby-BPM matches, nearby-BPM matches, and the remaining
   tracks, with subtle separators between groups.
5. **Learn it**: 🎓 TUTORIAL opens a full virtual FLX4, lights each move up to
   eight beats ahead, and scores physical-controller or virtual-control input. It
   warns before starting when an action lacks an FLX4 mapping or a referenced
   hot cue is missing, unverifiable, or mapped to the wrong beat. Human mode's
   **SHOW CUES** checkbox is on by default and marks every condensed action on
   both the overview and zoomed waveform views. PRIME's amber markers show the
   exact preflight target; green markers then follow the authored state and
   ramps live throughout Tutorial.
6. **Author it on the computer**: open Library > Transitions and choose New or
   Edit. The dedicated editor has beat-grid snapping (Option bypasses it),
   exact property tables, undo/redo, a private MASTER preview, stem preparation,
   automation writing, and crash-recovery drafts. Legacy or endpoint-changing
   edits are always saved as a new `.transition`.

`--selftest` writes `selftest_out.wav` in the working directory; its temporary
transition round-trip files are removed automatically.

Plug in a DDJ-FLX4 at any time — the status bar shows the connection and all
controls + LEDs map automatically (see docs/STATUS.md for the exact mapping).
Choose the master speakers under Settings ▸ Audio Output. The system default is
used initially; Bluetooth works with its expected latency, while a connected
FLX4 continues to provide the separate headphone-cue output.

Utility/dev flags: `--convert-transitions` (idempotently create portable copies
of every legacy transition), `--selftest` (headless render check), and
`--autoload [A B]` (auto-load matching library tracks onto the decks).

## Repository layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Agent/contributor handoff
notes live in [docs/STATUS.md](docs/STATUS.md).

## License

GPL-3.0 (DJ ecosystem heritage: Mixxx is GPL).
