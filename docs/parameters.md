# CLAP parameters

Per sound (a parameter event with a channel reaches the sounds that channel selects; without one, every
selected sound). The ids are stable: a new parameter gets a new id, an id is never reused or renumbered.

| id | name | patch-text key | range | default | what it does |
|---|---|---|---|---|---|
| 0 | Level | `level` | 0..1 | 1 | the sound's level, linear (1 = the patch's own level) |
| 1 | Pan | `pan` | -1..1 | 0 | equal-power pan |
| 2 | Polyphony Limit | `polyphony` | 1..64, stepped | 64 | the most voices the sound keeps |
| 3-10 | Knob 1-8 | `knob1`..`knob8` | 0..1 | the patch's own value (raw `PartAssignableKnobNValue` / 1023; model default 512) | the part's knob N, live |
| 11 | Ribbon | `ribbon` | 0..1 | 0 | the ribbon as a controller box source (source 6), live; the box sees v = round(value x 127) through the 7-bit map (below) |
| 12, 13 | Switch 1, Switch 2 | `switch1`, `switch2` | 0 or 1, stepped | 0 | switches 1-2 as box sources (16, 17): 0 or 1024 |
| 14-37 | the controller box destinations | none (the raw patch's own parameters) | the raw parameter's range | the model's default; shows the sound's own value | below; modulatable |
| 38 | Foot Controller 2 | `foot2` | 0..1 | 1 | foot controller 2 as a box source (4), as the ribbon; not saved (a performance controller, as the MIDI ones) |
| 39 | Foot Switch | `footswitch` | 0 or 1, stepped | 0 | the foot switch as a box source (5): 0 or 1024; not saved |

The knobs, the ribbon and the switches are what the reference's controller boxes read (box sources 8-15 = knobs 1-8). A change takes
effect at the next control step of the core (at most 16 frames). Whatever the boxes add, the cutoff stops at the
filter's top (22.05 kHz) and the filter EG's depth at its range (raw 64-448: a box driving it past 448 sounds as the static
448 in the reference). Nothing is recompiled and nothing is allocated on the audio thread: the boxes are compiled
with the sound, the knob value is applied in the control step. A sound starts at its patch's own knob
values (`prepare` text or the raw patch); the CLAP state and the sound-contexts snapshot save the knob
parameters, the ribbon and the switches with the other values (`oscairn-va-state 4`, `oscairn-va-contexts/4`: the shell's
values are ids 0-13; the formats before them are not read). A host never passes the ribbon's or the switches' MIDI controllers
(CC 16, 86, 87) to a plugin, so they are parameters, as the knobs.

## Controller box destinations (ids 14-37)

Every destination the core's boxes move is also a parameter, flagged `CLAP_PARAM_IS_MODULATABLE`, so that a converter can map a
plain source-to-destination box onto the host's control tree with a host-side curve instead of an in-engine box. Its value is
the raw patch parameter itself (saved with the patch, as the patch text holds it; a value event sets it live, nothing is
recompiled). A modulation (`CLAP_EVENT_PARAM_MOD`, the amount in the parameter's own unit: raw steps) adds to the destination
together with its boxes: an amount a is a box sum of a x 8192 / span, added to the boxes' outputs before their +-8192 clamp,
so a modulation of a = m x span (m normalised, 1 a full box) moves the destination by the same raw steps a box output of
m x 8192 would, toward zero, within the destination's range. A modulation is not saved, not shown as the value, and stays with
the sound slot until the host changes it; a modulation for one note (a note id or key) is not applied.

| id | name | raw parameter | range | span (raw steps of a full box) |
|---|---|---|---|---|
| 14, 15 | Osc 1 / Osc 2 Pitch LFO Depth | `Osc1OscillatorPitchLFODepth`, `Osc2...` | 0-494 | 247 |
| 16, 17 | Osc 1 / Osc 2 Level | `Osc1OscillatorOutLevel`, `Osc2...` (an oscillator at level 0 in the patch stays off) | 0-511 | 511 |
| 18 | Cutoff | `Filter1FilterCutoff` (the part's cutoff offset is added, as to the patch's value) | 0-1023 | 1023 |
| 19 | Filter EG Depth | `Filter1FilterCutoffEGDepth` | 64-448 | 192 |
| 20 | Resonance | `Filter1FilterResonance` | 0-255 | 255 |
| 21-23 | Filter EG Attack / Decay / Release | `FilterCutoffEGAttackTime`, `...DecayTime`, `...ReleaseTime` | 0-255 | 255 |
| 24-26 | Amp EG Attack / Decay / Release | `AmplitudeEGAttackTime`, `...DecayTime`, `...ReleaseTime` | 0-255 | 255 |
| 27 | Osc 3 Pitch LFO Depth | `Osc3OscillatorPitchLFODepth` | 0-494 | 247 |
| 28 | Filter 2 Cutoff | `Filter2FilterCutoff` (the part's cutoff offset is not added: on filter 2 it is not measured) | 0-1023 | 1023 |
| 29 | Filter 2 EG Depth | `Filter2FilterCutoffEGDepth` | 64-448 | 192 |
| 30 | Filter 2 Resonance | `Filter2FilterResonance` | 0-255 | 255 |
| 31, 32 | Osc 1 / Osc 2 Pulse Width | `Osc1OscillatorPulseWidth`, `Osc2...` (a moved width plays the pulse as two table reads) | 0-255 | 255 |
| 33 | Filter LFO Depth | `Filter1FilterCutoffLFODepth` | 64-448 | 192 |
| 34 | Pitch LFO Speed | `PitchLFOSpeed` | 0-415 | 415 |
| 35-37 | Osc 1-3 Frequency (cents) | none: an offset from 0 in cents, not saved with the patch (a value event holds until the next sound load) | -3600..3600 | 3600 |

For a linear box (curve Standard 5) both paths give the same result: the ABI test plays the mod wheel at 127 through a box of
ratio +8 on the cutoff and a modulation of 127.875 steps, bit for bit the same. In-engine boxes stay for what a host curve does
not express: the reference's curve shapes, several boxes summed and clamped, the bipolar offset. Coverage
(`vcore::notCovered`) judges the patch with its boxes; a value or modulation set from outside can take a destination beyond
what was measured, as a knob can.

## Editor hints (`oscairn.ui`)

Every parameter has a hint: Level (role level, priority 1), Pan (pan), Cutoff (cutoff, 2), Resonance (resonance, 3), Filter EG
Depth (filter envelope, 4), the filter EG's and the amplitude EG's attack, decay and release (their roles, and an envelope view
each: views 2 and 1, fields attack / decay / release), Osc 1 / 2 Level (levels 1 and 2); the knobs, and the ribbon with the
switches, and filter 2's cutoff, EG depth and resonance (ids 28-30), are a group each (the switches as switches).

## MIDI controllers (controller box sources)

The other box sources are the channel's MIDI controllers, at the reference's default numbers (a sound's boxes read the
channel of its latest note; the core keeps them per channel):

| reference source | MIDI | value the box sees (0..1023) |
|---|---|---|
| 0 pitch bend | pitch bend | the 14-bit value / 16 |
| 1 mod wheel | CC 1 | 8 v up to v = 64, then 512 + (v - 64) x 512 / 63 (127: 1024) |
| 2 aftertouch | channel pressure, or the pressure note expression (the channel's latest) | as a CC (its map is not measured: reported as not covered) |
| 3 foot controller 1 | CC 11 | as CC 1 |
| 7 breath | CC 2 | as CC 1 |

The ribbon (6) and switches 1-2 (16, 17) are parameters 11-13 (a host does not pass their CCs). CC 4 has no default meaning
in the reference (its foot controllers are CC 11 and, for the second, a control number a host does not pass) and does nothing.

CC 121 (reset all controllers) puts the channel's sources at their rest values: the foot controllers and breath at the
top, the others at 0, the bend centred (the reference's reset; the rest values themselves are taken from its documentation,
not measured). Foot controller 2 and the foot switch are parameters 38 and 39 (a host passes neither's MIDI controller; va-round6 A1: the reference
rests at the top and at 0 for them, as a box on the cutoff showed, and took neither CC 95 / 96 nor CC 88 in that probe, so their value
map is not measured and a box reading them is reported). The sequenced sources and the envelope-following sources are not built. In the reference CC 11 is also expression (the part's volume); the shell does not
apply expression.

The 7-bit map (va-round5 C1): the mod wheel through a +32 box on the cutoff (LPF12, the section's F read exactly) lands on the
static cutoff this map predicts at 11 values from 16 to 96 within 1e-5 of F, where round(v x 1023 / 127) predicts 1-2 raw steps
more and misses by 0.4-1.9 % (the earlier "within 0.3 cent" check through the pitch LFO's depth could not tell the two apart). The
14-bit bend / 16 is not re-measured.

The box law (va-round3 R1, `docs/coverage.md`): a box's output is (c + o - 1024) x ratio / 8 within +-8192, c its curve at
the source's value, o 1024 unipolar or 512 bipolar, ratio the raw value - 128 (raw 255: +128); a destination's outputs are
summed and the sum clamped to +-8192; the destination's raw value moves by the sum x span / 8192 steps, toward zero, and
stays within its range. The truncation comes once, after the sum (va-round5 C1: three +1 boxes from the wheel at 127 on cutoff 600,
15.98 steps each, sound as the static 647 within 1e-5 of F; truncating each box first would give 645). This differs from what the
sample engine's round measured on the reference's sample-based parts (each box truncated before the sum): on the analog model it is
the sum. A box reaches an oscillator's or filter 1's destination only where that unit's switch for the box
is on (destinations 142, 143 and 145 move filter 1 and filter 2, each where its switch for the box is on; filter 2's are ids
28-30).

Coverage (`vcore::notCovered`, `vcbench coverage`) is judged at the patch's own knob values. A knob moved
live can take a box outside what was measured; the other sources (performance controllers) are judged over their whole
range.
