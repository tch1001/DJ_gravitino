# STATUS — agent handoff board

> Keep this file current. When you finish or abandon work, update your section.
> An agent with zero prior context must be able to resume from this file plus
> docs/ARCHITECTURE.md and docs/TRANSITION_FORMAT.md.

## Current state (update the date/line when you change things)

- 2026-08-15 ~04:30: Repo scaffolded. Docs + pinned headers + CMake written by
  the orchestrator. Stub .cpp files compile and link; modules being implemented
  by parallel agents.

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
| audio/{AudioEngine,Deck,Eq}.cpp | codex-audio | stub |
| analysis/{TrackData,BeatAnalyzer}.cpp, library/*.cpp | claude-analysis | stub |
| transitions/{GvtFormat,TransitionRecorder,TransitionPlayer}.cpp, tests | claude-transitions | stub |
| midi/{MidiEngine,Flx4Mapping}.cpp | codex-midi | stub |
| ui/*.cpp, app/* | claude-ui | stub |
| integration/selftest | orchestrator | pending |

## Known decisions & gotchas

- Engine sample rate fixed at 48 kHz (`kSampleRate`); all decode resamples to it.
- No keylock in MVP — tempo change shifts pitch (documented TODO).
- Beatgrid is fixed-tempo (bpm + firstBeatSec) — fine for the demo tracks.
- ControlBus dispatch is synchronous, GUI thread only. MIDI thread must post
  via QMetaObject::invokeMethod(..., Qt::QueuedConnection).
- Audio-thread params are std::atomic on Deck/AudioEngine — no locks in render.
- .gvt parsers skip unknown controls with warnings, never hard-fail.
- Qt is keg-only at /opt/homebrew/opt/qt (CMAKE_PREFIX_PATH already set in
  CMakeLists.txt).

## TODO backlog (post-MVP)

- Keylock (signalsmith-stretch), variable beatgrids, WAV/FLAC/AAC, transition
  marketplace/sharing UI, Windows/Linux builds, .app bundling + macdeployqt.
