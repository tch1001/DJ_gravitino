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

## Sharing and portability

A `.gvt` is portable transition data, not a bundle of the music itself. To use
one from a friend, copy it into Gravitino's Transitions folder and import the
same two audio tracks into the local library. Matching is strongest when the
audio fingerprint is identical, which is independent of filename and tags.
Matching title + artist is the supported fallback when an encoded copy has a
different fingerprint. Duration by itself is only a diagnostic hint and will
not authorize playback.

The file carries the role-based deck/mixer starting state, beat anchors,
hot-cue position checks, cue labels, and the complete beat-timed control event
stream. It does **not** carry either audio track, analyzed stems, the recipient's
beatgrid cache, or saved-loop pad contents. Consequently:

- Ordinary fader, EQ, filter, transport, loop, FX, and stem-level events travel
  with the file. Stem mutes work only after the recipient separates stems for
  that track; otherwise they degrade harmlessly to the original mix.
- Referenced HOT CUE pads must exist at the recorded beats on the recipient's
  machine. The stored mapping is currently used to verify and warn, not to
  overwrite the recipient's cues.
- Referenced CUSTOM/saved-loop pads must currently be recreated on the
  recipient's machine. Their loop definitions are stored in the local track
  cache, not in v1 `.gvt`.
- A materially different master/remaster, or a differently corrected beatgrid,
  can change musical alignment even when title and artist match.

So v1 is directly shareable for the same tracks and ordinary transition moves,
with explicit setup needed for HOT CUE, CUSTOM-loop, and stem-dependent moves.
A future fully self-contained exchange format should embed the referenced cue
and saved-loop definitions (while still leaving copyrighted audio out).

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

[initial]
complete    = 1         ; full two-deck + mixer snapshot
crossfader  = 0.000     ; role space: outgoing -> incoming
playing     = 1         ; unprefixed keys describe the outgoing deck
position_beat = 224.000
cue_beat    = 224.000
tempo_ratio = 1.000
fader       = 1.000
eq_low      = 0.500
eq_mid      = 0.500
eq_high     = 0.500
filter      = 0.500
quantize    = 1         ; hot-cue/manual-loop snapping state
loop_active = 0
loop_start_beat = 224.000
loop_end_beat = 224.000
fx_type     = 0
fx_on       = 0
fx_wet      = 0.500
fx_beats    = 0.500
stem_vocals = 1.000
stem_melody = 1.000
stem_bass   = 1.000
stem_drums  = 1.000
to_playing  = 0         ; to_* keys describe the incoming deck
to_position_beat = 32.000
to_cue_beat = 32.000
to_tempo_ratio = 1.000
to_fader    = 0.000
to_eq_low   = 0.500
to_eq_mid   = 0.500
to_eq_high  = 0.500
to_filter   = 0.500
to_quantize = 1
to_loop_active = 0
to_loop_start_beat = 32.000
to_loop_end_beat = 32.000
to_fx_type  = 0
to_fx_on    = 0
to_fx_wet   = 0.500
to_fx_beats = 0.500
to_stem_vocals = 1.000
to_stem_melody = 1.000
to_stem_bass = 1.000
to_stem_drums = 1.000

[hotcues]
; role+pad = track-relative beat used by the recorded transition
b1          = 32.000

[cues]
0.000       = Start beatmatch
24.000      = Exit outgoing

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
- **Sections** `[meta] [from] [to] [sync] [initial]` hold `key = value` pairs.
  Unknown keys are preserved on load and rewritten on save (forward compat).
- **`[initial]`** is optional for compatibility with older v1 files. New
  recordings set `complete = 1` and snapshot both role-based decks plus the
  mixer crossfader. Unprefixed keys are the outgoing deck; `to_*` keys are the
  incoming deck. The snapshot covers transport/cue position, tempo, channel
  level, EQ, filter, Quantize, loops, FX, and stem levels. TRIM is intentionally
  not part of new transition state: it is input gain and must remain under the
  DJ's current control. Perform
  reconstructs it before rolling; Prime prepares it immediately and reasserts
  transport state at the entry boundary. Older partial sections containing
  only the outgoing tempo/gain/EQ/filter keys remain supported. Legacy `trim`
  and `to_trim` keys still parse but are ignored by setup/replay.
- **`[cues]`** is optional and holds user labels as `beat = label`. Labels are
  annotations only: they populate the event preview and waveform markers.
  `#`, `;`, and line breaks are reserved because they delimit comments/lines.
- **`[hotcues]`** is optional and records the track-relative beat assigned to
  every hot-cue pad referenced by the transition. Keys use role + pad (`a1` is
  outgoing pad 1, `b8` incoming pad 8). Tutorial mode compares these positions
  with the loaded tracks and warns about missing, unverifiable, or mismatched
  assignments. Negative values are valid for cues in audio before the detected
  first downbeat; an omitted key, rather than a negative number, means that the
  mapping is unavailable. Legacy files without this section still load normally.
- **`[events]`** holds one event per line, whitespace-separated columns:

  `beat  target  control  [value]  [curve]  [via=gesture@mode]`

  - `beat` — decimal beats since transition start (anchor). Sorted ascending.
  - `target` — `a` (outgoing deck), `b` (incoming deck), `x` (mixer/global).
    Decks are *roles*, not physical decks: on replay, "a" is wherever the
    outgoing track sits.
  - `control` — see table below.
  - `value` — normalized number (most controls 0..1; tempo in ratio; jog in
    signed ticks). Omitted for ordinary trigger controls. `cue`,
    `hotcue_1..8`, and `saved_loop_1..8` retain `1` press / `0` release values
    for hold-preview.
  - `curve` — optional: `step` (default), `linear`, `scurve`. Non-step means
    "glide from this control's previous value, arriving at `value` on `beat`,
    starting where the previous event for the same (target, control) ended."
  - `via` — optional physical gesture that produced the state change. Replay
    still executes `control`; Tutorial uses `via` for accurate coaching. For
    example, `saved_loop_3 1 via=performance_pad_3@custom` means CUSTOM pad 3
    started a captured loop, so the tutor highlights that pad instead of PLAY.
    Stable pad
    mode names are `hot_cue`, `pad_fx1`, `beat_jump`, `custom`, `keyboard`,
    `pad_fx2`, `beat_loop`, and `key_shift`. Files without `via` remain fully
    compatible and are taught from their state control as before.

## Controls (v1)

| control      | targets | value                          |
|--------------|---------|--------------------------------|
| `play`,`stop` | a,b | (trigger)                         |
| `cue`        | a,b     | 1 press / 0 release (hold-preview) |
| `load`       | b       | (trigger; load the TO track)   |
| `tempo_sync` | a,b     | (trigger; match master bpm)    |
| `tempo`      | a,b     | playback ratio, 1.0 = native   |
| `quantize`   | a,b     | per-deck hot-cue/manual-loop snapping, 0 or 1 |
| `fader`      | a,b     | 0..1 channel fader             |
| `trim`       | a,b     | legacy only; parsed but no longer recorded/applied |
| `eq_low`,`eq_mid`,`eq_high` | a,b | 0..1 (0.5 = flat, 0 = kill) |
| `xfader`     | x       | 0 = outgoing ... 1 = incoming (role space) |
| `hotcue_1..8`| a,b     | 1 press: play from cue; 0 release: stop and return |
| `saved_loop_1..8` | a,b | 1 press: preview stored loop; 0 release: stop and return unless PLAY latched |
| `jog`        | a,b     | signed nudge ticks (not recorded) |
| `loop_auto`  | a,b     | loop length in beats (starts beat-snapped loop) |
| `loop_in`,`loop_out`,`loop_exit`,`loop_halve`,`loop_double` | a,b | (trigger) |
| `beat_jump`  | a,b     | signed beats to jump, beat-aligned |
| `filter`     | a,b     | 0.5 = off, <0.5 LPF sweep, >0.5 HPF sweep |
| `fx_type`    | a,b     | 0 = echo, 1 = reverb, 2 = flanger |
| `fx_on`      | a,b     | state: >0.5 engaged, <=0.5 off |
| `fx_wet`     | a,b     | 0..1 dry/wet |
| `fx_beats`   | a,b     | echo/flanger time base in beats (0.25..4) |
| `stem_vocals` | a,b    | 0..1 vocal stem level (0 = muted) |
| `stem_melody` | a,b    | 0..1 melody stem level (0 = muted) |
| `stem_bass`  | a,b     | 0..1 bass stem level (0 = muted) |
| `stem_drums` | a,b     | 0..1 drums stem level (0 = muted) |

`stem_*` levels only audibly apply when the replaying machine has separated
stems attached to that deck (separation is per-machine, cached under
`~/.gravitino/stems/`). Without stems the events still set the deck's stem
atomics harmlessly — the engine keeps playing the original PCM whenever no
stems are attached (and also short-circuits back to plain PCM while all four
gains are >= 0.99) — so such transitions replay gracefully, just without the
stem mutes.

Parsers must **skip unknown controls with a warning**, never fail — future
versions add controls, old players still perform the rest of the blend.

## Track matching & fingerprint

`fingerprint = gvfp1:<16 hex>` — FNV-1a 64-bit hash over the first 30 s of
mono PCM decimated to 11025 Hz, 8-bit quantized. Cheap, robust to
tag/filename edits, not robust to remasters — so matching is tiered:
exact fingerprint ▸ (title+artist, case-folded) ▸ duration within ±1.5 s.
Fingerprint or title+artist identity is required for every operational action:
Perform, Tutorial, transition auto-loading, hot-cue dependency checks, and
playing-track prioritization. Duration-only similarity is a weak discovery hint
and must never authorize a transition for a loaded track by itself.
