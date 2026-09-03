# Gravitino DJ

Open-source DJ software for people who'd rather perform than rehearse.

Gravitino's core idea: **record a transition once, replay it forever**. Nail the
blend between two songs one time — every fader move, EQ sweep, and tempo nudge is
captured as a *transition file* (`.gvt`, a human-readable text format). Next time
you load that pair of tracks, pick the recorded transition and Gravitino performs
it beat-perfectly, or teaches it to you hands-on in Tutorial mode. Transitions
are plain text: diff them, edit them, share them.

## Status

MVP targeting macOS (Apple Silicon), built with Qt 6 for eventual
cross-platform support. Local MP3s only for now. First-class hardware support
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
- Library browser scanning your local MP3 folder (ID3 tags via TagLib)
- **Transition recording & replay** — the headline feature (see
  [docs/TRANSITION_FORMAT.md](docs/TRANSITION_FORMAT.md))
- Full virtual DDJ-FLX4 Tutorial overlay: highlights each recorded gesture,
  accepts clicks or physical input, and checks referenced hot-cue assignments
- DDJ-FLX4 plug-and-play MIDI mapping with LED feedback (hot-plug supported —
  plug it in any time)

## Build

```sh
brew install qt rtmidi taglib cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/gravitino            # launch the app
./build/gravitino --selftest # headless: renders a scripted transition to WAV
```

## Using it

1. Launch `./build/gravitino` — it scans `~/Music` (File ▸ Open Music Folder to
   change) and analyzes BPM/beatgrids in the background.
2. Load a track on each deck (Load ▶ A / Load ▶ B or double-click), press PLAY.
   Without a controller, drag a mapped HOT CUE pad onto PLAY and release to
   latch it exactly like holding the pad while pressing PLAY on hardware. Drag
   either deck's rotating platter clockwise/counterclockwise for fine
   beat-matching: it shifts the playhead by milliseconds without interrupting
   playback, and provides the same precise positioning while paused.
3. **Record a transition**: with the outgoing deck playing, hit ● REC, do your
   blend (faders, EQ, crossfader, sync, hot cues — mouse or FLX4), then
   ■ STOP & SAVE and name it. It lands in `~/Music/Gravitino/Transitions/` as
   a readable `.gvt` text file you can edit in-app or share.
4. **Replay it**: next time that pair is loaded (matched by audio fingerprint,
   falling back to title + artist), it appears in the Transitions list —
   ▶ PERFORM executes it beat-perfectly at whatever tempo you're running.
5. **Learn it**: 🎓 TUTORIAL opens a full virtual FLX4, lights each move four
   beats ahead, and scores physical-controller or virtual-control input. It
   warns before starting when an action lacks an FLX4 mapping or a referenced
   hot cue is missing, unverifiable, or mapped to the wrong beat.

A sample transition for the Pioneer demo tracks ships in
`~/Music/Gravitino/Transitions/demo-blend.gvt` after the selftest.

Plug in a DDJ-FLX4 at any time — the status bar shows the connection and all
controls + LEDs map automatically (see docs/STATUS.md for the exact mapping).
Choose the master speakers under Settings ▸ Audio Output. The system default is
used initially; Bluetooth works with its expected latency, while a connected
FLX4 continues to provide the separate headphone-cue output.

Dev flags: `--selftest` (headless render check), `--autoload [A B]` (auto-load
matching library tracks onto the decks).

## Repository layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Agent/contributor handoff
notes live in [docs/STATUS.md](docs/STATUS.md).

## License

GPL-3.0 (DJ ecosystem heritage: Mixxx is GPL).
