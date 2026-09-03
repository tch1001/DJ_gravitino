# The portable `.transition` format (v1)

A transition is a directed, reusable performance edge between two canonical
song arrangements: `outgoing → incoming`. The file contains no copyrighted
audio. Instead it describes which arrangements it expects, the musical beat
coordinates used by the author, transition-owned cues and saved loops, initial
state, labels, and the exact control timeline.

New recordings are one UTF-8 YAML document with the extension `.transition`.
Legacy `.gvt` files remain readable indefinitely; editing or converting one
creates a separate `.transition` and never overwrites or removes the source.

## Portability boundary

MP3, FLAC, WAV, and AIFF assets may all satisfy an endpoint when they contain
the same arrangement. Container, bitrate, gain, sample rate, tags, and small
leading-silence/encoder offsets are not musical identity. Radio edits,
extended mixes, clean edits, and structurally changed remasters are distinct
arrangements in v1. Gravitino never guesses a nonlinear or piecewise warp.

Beats are authoritative coordinates and may be negative or fractional. A
knob movement at beat `296.452345678` remains between beats; snapping is an
editor affordance, never a storage restriction. Seconds and the reference
downbeat describe the author's source asset and help matching, but do not bind
replay to that encode's timestamps.

A confirmed local binding may provide one constant beat offset:

```
canonical beat = local analyzed beat + offset
```

This handles a fixed leading-silence/grid-origin difference. If alignment
drifts or needs multiple offsets, the arrangement is rejected in v1.

## Document shape

```yaml
format: "gravitino.transition"
version: 1
id: "d7f55bd5-618d-4f74-ad35-6d55a1a5f963"

metadata:
  name: "Whistle → Turn Down for What"
  author: ""
  created_at: "2026-08-21T00:00:00Z"
  description: ""
  license: ""
  tags: []

requires:
  - "timeline.v1"
  - "temporary-cues.v1"
  - "temporary-loops.v1"

endpoints:
  outgoing:
    identity:
      title: "Whistle"
      artists: ["Flo Rida"]
      version: "Original"
      identifiers:
        isrc: ""
        musicbrainz_recording: ""
        providers: {}
    assumptions:
      native_bpm: 104.016667
      duration_seconds: 225.59
      duration_beats: 391.2
      meter: "4/4"
      reference_downbeat_seconds: 0.42
      fingerprints:
        - algorithm: "gravitino-structure-2"
          value: "gvsf2:..."
        - algorithm: "gvfp1"
          value: "gvfp1:..."
    notes: ""
  incoming:
    identity:
      title: "Turn Down for What"
      artists: ["DJ Snake", "Lil Jon"]
      version: "Original"
      identifiers:
        isrc: ""
        musicbrainz_recording: ""
        providers:
          spotify: ""
    assumptions:
      native_bpm: 104.016667
      duration_seconds: 213.0
      duration_beats: 369.3
      meter: "4/4"
      reference_downbeat_seconds: 0.18
      fingerprints: []
    notes: ""

performance:
  master_bpm: 104.016667
  anchors:
    outgoing: {track_beat: 283.195058}
    incoming: {track_beat: 96.0}
  initial_state:
    complete: true
    mixer: {captured: true, crossfader: 0.0}
    outgoing:
      captured: true
      playing: true
      position_beat: 283.195058
      cue_beat: 283.195058
      tempo_ratio: 1.0
      fader: 1.0
      eq_low: 0.5
      eq_mid: 0.5
      eq_high: 0.5
      filter: 0.5
      quantize: true
      loop_active: false
      loop_start_beat: 283.195058
      loop_end_beat: 283.195058
      fx_type: 0
      fx_on: false
      fx_wet: 0.5
      fx_beats: 0.5
      stem_vocals: 1.0
      stem_melody: 1.0
      stem_bass: 1.0
      stem_drums: 1.0
    incoming:
      captured: true
      playing: false
      position_beat: 96.0
      cue_beat: 96.0
      tempo_ratio: 1.0
      fader: 0.0
      eq_low: 0.5
      eq_mid: 0.5
      eq_high: 0.5
      filter: 0.5
      quantize: true
      loop_active: false
      loop_start_beat: 96.0
      loop_end_beat: 96.0
      fx_type: 0
      fx_on: false
      fx_wet: 0.5
      fx_beats: 0.5
      stem_vocals: 1.0
      stem_melody: 1.0
      stem_bass: 1.0
      stem_drums: 1.0
  cues:
    - id: "incoming-launch"
      endpoint: "incoming"
      track_beat: 96.125
      label: "Launch"
      purpose: "start-track"
      color: "#55b9df"
      pairing_group: "launch"
      preferred_input: {bank: "custom", pad: 3, key: "3"}
  loops:
    - id: "incoming-intro-loop"
      endpoint: "incoming"
      start_track_beat: 96.125
      end_track_beat: 104.125
      label: "Intro loop"
      purpose: "saved-loop"
      color: "#e8a13a"
      pairing_group: "intro"
      preferred_input: {bank: "custom", pad: 5, key: "5"}
  labels:
    - at_beat: 0.0
      label: "Start beatmatch"
  timeline:
    - at_beat: 0.125
      target: "incoming"
      control: "deck.transition_cue"
      cue_id: "incoming-launch"
      value: 1.0
      curve: "step"
      input_hint:
        control: "host.performance_pad_3"
        pad_mode: "custom"
    - at_beat: 0.25
      target: "incoming"
      control: "deck.play"
      value: 1.0
      curve: "step"
    - at_beat: 0.375
      target: "incoming"
      control: "deck.transition_cue"
      cue_id: "incoming-launch"
      value: 0.0
      curve: "step"
    - at_beat: 16.625
      target: "incoming"
      control: "deck.transition_loop"
      loop_id: "incoming-intro-loop"
      value: 1.0
      curve: "step"
      input_hint:
        control: "host.performance_pad_5"
        pad_mode: "custom"
    - at_beat: 24.625
      target: "mixer"
      control: "mixer.xfader"
      value: 1.0
      curve: "scurve"

extensions: {}
```

The serializer is deterministic, quotes strings, and keeps double precision.
Human/Raw event summaries and tutorial progress rows are derived UI data and
are never serialized.

## Identity and matching

Endpoint identity is evidence, not a remote playback dependency. Provider IDs
live under `identity.identifiers.providers`; a future Spotify or other catalog
adapter can add an ID without changing cues, timeline, or local playback.

An imported endpoint resolves in this order:

1. A previously confirmed local endpoint-to-song binding.
2. A strong algorithm-tagged structural fingerprint, checked against BPM and
   duration assumptions.
3. A matching ISRC or MusicBrainz recording ID, still checked against BPM,
   duration, and beat-count assumptions.
4. Matching title and artists, shown only for explicit manual confirmation.

Metadata-only matches never enable playback automatically. Duration alone is
only a discovery hint. Multiple local assets may belong to the same canonical
song; the DJ chooses among them when more than one compatible encode is
available.

`gvfp1` is exact decoded-audio evidence retained for legacy matching.
`gravitino-structure-2`/`gvsf2` stores normalized temporal and spectral block
features after trimming silence. It is designed to survive transcoding, gain,
sample-rate conversion, and small encoder offsets. Fingerprints are an array
so stronger algorithms can be added without invalidating older evidence.

The versioned local catalog is stored separately from transition files. It
contains canonical local song IDs, asset paths and hashes, analyzed beatgrids,
fingerprints, constant offsets, and confirmed bindings. It never modifies
audio tags. The reverse graph from songs to incoming/outgoing transitions is
rebuilt by scanning `.transition` and `.gvt`, so rename/delete cannot leave an
authoritative stale edge. Missing asset paths remain local history but are not
offered for loading.

## Transition-owned cues and saved loops

Timeline events refer to stable semantic `cue_id` values, not serialized pad
numbers. Each cue declares an endpoint and canonical track beat, with optional
label, purpose, teaching color, pairing group, and preferred key/pad. A shared
pairing group lets outgoing and incoming cues use the same teaching color.

Saved-loop timeline events likewise refer to stable semantic `loop_id` values.
Each loop owns canonical `start_track_beat` and `end_track_beat` coordinates,
so changing or losing the song's permanent saved-loop slots cannot change the
transition. Labels, purpose, color, pairing group, and preferred input use the
same optional teaching metadata as cues.

On selection/replay, Gravitino allocates at most eight cues and saved loops
combined per endpoint in an isolated temporary `CUSTOM` bank. Preferred
controls are hints; collisions are assigned another free pad and the host owns
the actual controller mapping. Permanent per-track hot cues and saved loops are
never replaced or saved over. Clearing/changing the selected transition
restores the normal CUSTOM bank. More than eight combined entries, an unknown
semantic reference, an invalid loop range, or an unsupported bank is a
validation error rather than a silent truncation.

## Timeline semantics

`at_beat` is measured from transition start in master beats. `target` is the
role (`outgoing`, `incoming`, or `mixer`), never a physical deck number.
Controls use stable namespaces. Current portable timeline controls are:

- Deck transport/state: `deck.play`, `deck.stop`, `deck.cue`,
  `deck.tempo_sync`, `deck.quantize`, `deck.transition_cue`, and
  `deck.transition_loop`.
- Deck movement: `deck.tempo` (ratio `0.01..4`), `deck.fader`,
  `deck.eq_low`, `deck.eq_mid`, `deck.eq_high`, `deck.filter` (`0..1`).
- Loops/jumps: `deck.loop_in`, `deck.loop_out`, `deck.loop_exit`,
  `deck.loop_halve`, `deck.loop_double`, `deck.loop_auto`
  (`0.03125..64` beats), and `deck.beat_jump` (`-1024..1024` beats).
- FX/stems: `deck.fx_type` (integer `0..2`), `deck.fx_on`,
  `deck.fx_wet` (`0..1`), `deck.fx_beats` (`0.25..4`), and
  `deck.stem_vocals`, `deck.stem_melody`, `deck.stem_bass`,
  `deck.stem_drums` (`0..1`).
- Mixer: `mixer.xfader` (`0..1` in role space: outgoing to incoming).

Trigger/state values use `1` for press/on and `0` for release/off. Curves are
`step`, `linear`, or `scurve`; trigger controls must be `step`. A non-step
event is the destination of a glide from the preceding event/value in the same
`(role, control)` stream. Interleaved controls do not split that stream.

`input_hint` records how a state change was produced or should preferably be
taught. Replay executes `control`; input hints never make a controller model a
playback dependency.

## Compatibility and safe parsing

`requires` declares semantics necessary for correct playback. This build
supports `timeline.v1`, `temporary-cues.v1`, and `temporary-loops.v1`. Unknown
required capabilities allow inspection but block Perform/Tutorial with a clear
compatibility error.

Additive optional fields may be introduced within version 1. Unknown mapping
fields and namespaced `extensions` are preserved at their containing level on
load/save. A breaking semantic change requires a new integer `version` and an
explicit migration reader.

The reader accepts a deliberately safe YAML subset:

- exactly one document, at most 2 MiB, 32 levels deep, 100,000 YAML nodes,
  20,000 timeline events, and 1,000 labels;
- no aliases, anchors, custom tags, merge keys, duplicate mapping keys, or NUL;
- scalar mapping keys only;
- finite, bounded numeric fields and validated roles, controls, curves,
  cue/loop references, loop ranges, combined pad limits, and control values.

Malformed known fields fail the document rather than being guessed. Unknown
timeline controls also fail: a future producer must pair new behavior with an
appropriate `requires` capability instead of letting an older player perform
an incomplete transition.

## Legacy `.gvt`

The original line-based `gravitino-transition 1` reader remains part of the
compatibility boundary. It adapts legacy anchors, negative/fractional beats,
events, curves, initial state, loops, labels, gesture hints, and hot-cue
positions into the typed in-memory model. Legacy hot-cue pad references become
semantic temporary cues during adaptation.

Until conversion, a legacy document receives a stable content-derived identity
(`legacy-…`). Its first portable save receives a persistent UUID and records
the legacy source identity under `extensions.gravitino.legacy.source_id`.
Scanning keeps both source files available as distinct rows so they can be
compared with the Transitions tab's `.gvt` and `.transition` filters. The link
still makes bulk conversion idempotent. `gravitino --convert-transitions`
creates only missing portable counterparts and always leaves each `.gvt`
intact. `gravitino --upgrade-transition-loops` can promote an older portable
copy that contains one raw saved-loop slot per endpoint when its complete
initial snapshot contains the corresponding IN/OUT range. Ambiguous or absent
ranges are reported and left untouched.
