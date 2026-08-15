# The `.gvt` Transition File Format (v1)

A Gravitino Transition file captures one rehearsed transition between a pair of
tracks as plain UTF-8 text. Design goals, in order:

1. **Human-readable and hand-editable** — a DJ can open it in any editor,
   understand it, tweak a fader curve by changing a number.
2. **Tempo-independent** — timestamps are in *beats*, so the same transition
   works when the set runs faster or slower than the rehearsal.
3. **Shareable** — self-describing: carries enough metadata to match tracks in
   someone else's library (title/artist/duration/BPM + audio fingerprint).
4. **Diff-friendly** — line-oriented, one event per line, stable ordering.

File extension: `.gvt`. One transition per file.
Suggested location: `~/Music/Gravitino/Transitions/`.

## Example

```gvt
gravitino-transition 1

[meta]
name        = Slam on the drop
author      = fish
created     = 2026-08-15
description = Kill the lows, slam the crossfader on A's last drop.

[from]
title       = Demo Track 1
artist      = PioneerDJ
bpm         = 130.00
duration    = 121.36
fingerprint = gvfp1:9f83a2c11d40be77

[to]
title       = Demo Track 2
artist      = PioneerDJ
bpm         = 128.00
duration    = 118.02
fingerprint = gvfp1:31b0cc04a9e15df2

[sync]
anchor_from = 224.0     ; beat in FROM track where the transition begins
anchor_to   = 32.0      ; beat in TO track aligned to transition beat 0
master_bpm  = 130.00    ; tempo the mix runs at during the transition

[events]
; beat | target | control | value | curve
0.000    b        load
0.000    b        tempo_sync
0.000    b        eq_low      0.00
0.000    b        fader       1.00
0.000    b        play
0.031    x        xfader      0.00
8.000    x        xfader      0.50   scurve
16.000   b        eq_low      1.00   linear
16.000   a        eq_low      0.00   linear
24.000   x        xfader      1.00   scurve
32.000   a        stop
```

## Grammar

- **Line 1**: `gravitino-transition <version>` — required magic.
- `;` or `#` starts a comment (whole line or trailing). Blank lines ignored.
- **Sections** `[meta] [from] [to] [sync]` hold `key = value` pairs.
  Unknown keys are preserved on load and rewritten on save (forward compat).
- **`[events]`** holds one event per line, whitespace-separated columns:

  `beat  target  control  [value]  [curve]`

  - `beat` — decimal beats since transition start (anchor). Sorted ascending.
  - `target` — `a` (outgoing deck), `b` (incoming deck), `x` (mixer/global).
    Decks are *roles*, not physical decks: on replay, "a" is wherever the
    outgoing track sits.
  - `control` — see table below.
  - `value` — normalized number (most controls 0..1; tempo in ratio; jog in
    signed ticks). Omitted for trigger controls (`play`, `cue`, ...).
  - `curve` — optional: `step` (default), `linear`, `scurve`. Non-step means
    "glide from this control's previous value, arriving at `value` on `beat`,
    starting where the previous event for the same (target, control) ended."

## Controls (v1)

| control      | targets | value                          |
|--------------|---------|--------------------------------|
| `play`,`stop`,`cue` | a,b | (trigger)                 |
| `load`       | b       | (trigger; load the TO track)   |
| `tempo_sync` | a,b     | (trigger; match master bpm)    |
| `tempo`      | a,b     | playback ratio, 1.0 = native   |
| `fader`      | a,b     | 0..1 channel fader             |
| `trim`       | a,b     | 0..1 gain knob                 |
| `eq_low`,`eq_mid`,`eq_high` | a,b | 0..1 (0.5 = flat, 0 = kill) |
| `xfader`     | x       | 0 = outgoing ... 1 = incoming (role space) |
| `hotcue_1..8`| a,b     | (trigger; jump to stored cue)  |
| `jog`        | a,b     | signed nudge ticks (not recorded) |
| `loop_auto`  | a,b     | loop length in beats (starts beat-snapped loop) |
| `loop_in`,`loop_out`,`loop_exit`,`loop_halve`,`loop_double` | a,b | (trigger) |
| `beat_jump`  | a,b     | signed beats to jump, beat-aligned |
| `filter`     | a,b     | 0.5 = off, <0.5 LPF sweep, >0.5 HPF sweep |

Parsers must **skip unknown controls with a warning**, never fail — future
versions add controls, old players still perform the rest of the blend.

## Track matching & fingerprint

`fingerprint = gvfp1:<16 hex>` — FNV-1a 64-bit hash over the first 30 s of
mono PCM decimated to 11025 Hz, 8-bit quantized. Cheap, robust to
tag/filename edits, not robust to remasters — so matching is tiered:
exact fingerprint ▸ (title+artist, case-folded) ▸ duration within ±1.5 s.
The UI shows match confidence when offering transitions for a loaded pair.
