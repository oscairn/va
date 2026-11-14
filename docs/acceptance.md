# Acceptance: the "probe A" patch against the reference

The patch: "probe A", a two-oscillator arpeggiated probe part played on the reference alone (effects and arpeggio off), as `tools/vcbench.cpp` and
`tests/core/test_core.cpp` hold it. Reference recordings (44.1 kHz) and the analysis scripts are in
measurement run va-core (2026-11-14) and measurement run va-fidelity (2026-11-14) (outside this
repository): one note at keys 84, 88, 90, 91 and 96 (on 0.3 s, off 1.3 s; 9 / 3 / 5 / 3 / 5 takes). The
part's arpeggio plays an up/down phrase over the keys held in the zone 84-96; the keys used here
(84 88 91, the chord the cost bench plays) are an assumption, the phrase is not recorded with held keys.
Keys sound three octaves down (the zone's shift). The engine rendered the same events with
`vcbench render probeA 44100 ...`, 24 seeds a key; both went through the same analysis
(`analysis/accept2.py` on `accept.py`).

The two oscillators are 5 cents apart, free running (their relative phase at a note's start is uniform in
the reference: two equal saws with OSC Reset off sum to 0.04-1.0 of their peak over 10 notes; with
reset to Phase they start together), and they drift; so the fundamental and the odd harmonics beat, and a
single take's level depends on that phase (the engine's 12-seed means alone scatter 2.4 dB). Levels are
compared as mean power over takes and seeds; the spectrum on the even harmonics (the saw alone). Release
times are measured from the scheduled note-off (the recorder's jitter is a few ms) and from the 2nd
harmonic tracked over whole periods of the fundamental (the earlier analysis's 10 ms window took in the
fundamental: the "3 dB dip at 9 ms" and the "voice end at key 84" were that and the slope-estimated
note-off, not the reference).

Rerun at commit 343b748 (the filter sections' per-sample normalisers): every row of the table below came out as
before within its printed precision (measurement run va-round2 (2026-11-14), analysis accept-probeA-343b748.txt).

**Rerun in round 4** (the saw-ladder cutoff law and the part's filter offsets; measurement run va-round4, analysis
accept-probeA-item0). The hold-to-release step was the performance's own offsets: its common block holds FEG Depth 65 and
Cutoff Frequency 63, which act as the part's offsets (va-round4 K2: a plain saw inside that performance steps from hold to
release, probe A's own blocks inside a plain performance do not; K3: +1.60 semitones of EG depth and +16 raw cutoff steps a
unit). Probe A now carries them as `PartFEGDepth=65 PartFilterCutoffFrequency=63`, the values the converter folds in.

| quantity | key | reference | engine | difference | tolerance | result |
|---|---|---|---|---|---|---|
| filter cutoff, hold | 84 / 88 / 90 / 91 / 96 | 652 / 652 / 658 / 635 / 657 Hz | 662 / 662 / 662 / 662 / 661 | +0.27 / +0.24 / +0.10 / **+0.72** / +0.10 semitone | 0.5 semitone | pass, FAIL at 91 |
| filter cutoff, after release | 84 / 88 / 90 / 91 / 96 | 587 / 582 / 586 / 569 / 586 Hz | 592 / 588 / 588 / 587 / 578 | +0.13 / +0.19 / +0.05 / **+0.54** / -0.24 | 0.5 semitone | pass, FAIL at 91 |
| even harmonics h4-h12 re h2 | 84 / 88 / 90 / 96 | | -0.6 to +0.6 / +0.3 to +0.8 / -0.1 to +0.4 / 0.0 to +0.6 dB | | 2 dB | pass |
| | 91 | | +1.4 to +1.9 dB | | 2 dB | pass |
| level, mean power | 84 / 88 / 90 / 91 / 96 | | -0.6 / +0.8 / -0.4 / **+1.9** / -1.1 dB | | 1.5 dB | pass, FAIL at 91 |

Key 91 stays the one-key anomaly (darker and quieter in the reference's three takes). The other rows are as in round 3.

**Rerun in round 3** (after the envelope coefficient fix, the true-zero filter EG depth law and the decay box as the decay
law; measurement run va-round3 (2026-11-14), analysis accept-probeA-round3.txt). Changed rows, reference against engine:

| quantity | key | reference | engine | difference | tolerance | result |
|---|---|---|---|---|---|---|
| filter cutoff, hold | 84 / 88 / 90 / 91 / 96 | 652 / 652 / 658 / 635 / 657 Hz | 613 / 612 / 612 / 612 / 612 | **-1.08 / -1.10 / -1.24 / -0.62 / -1.25** semitone | 0.5 semitone | FAIL |
| filter cutoff, after release | all five | 587 / 582 / 586 / 569 / 586 Hz | 595 / 592 / 591 / 591 / 583 | +0.23 / +0.30 / +0.16 / **+0.65** / -0.11 | 0.5 semitone | pass, FAIL at 91 |
| even harmonics h4-h12 re h2 | 84-96 | | -0.7 to -3.3 dB | | 2 dB | FAIL (follows the hold cutoff) |
| release rate (-20 to -60 dB) | all five | 84.2-85.6 dB/s | 82.9 | -1.5 to -3.1 % | 5 % | pass |
| voice end after the note-off | all five | 1.008-1.015 s | 1.056 s | +41 to +48 ms | 50 ms | pass |

The other rows (levels, pitch, attack, decay to -3 / -6 dB, release to -20 / -40 / -60 dB within 13 ms) are as before
within their tolerances. The release rows moved because the envelope coefficients were 0.3 per second fast before (the
release law had matched with that error in). The hold: the earlier depth law put 1.4 semitones at depth 256, fitted on this
very hold-to-release step; the reference's depth 256 is a true zero (the converter's three sets; va-round3 R1 G_*), yet its
hold is brighter than its sound after the release also at depth 256 (va-round3 R4 Z_*: probe A with its boxes off, depth
256 / 257 / 258 / 260 / 264, decay held: the even-harmonic metric steps 0.70 / 0.84 / 0.99 / 1.21 / 1.66 dB from hold to
after the release), so the step is not the EG depth's (round 4: it is the performance's FEG Depth offset, above). The performance's common boxes (common
knob 1 to the part's knob 1) are ruled out: probe A with them switched off sounds the same within 0.02 dB (R3).

Cross-build tolerance: the same render on every target within 2e-4 of the stored statistics (every
100 ms window's RMS; `tests/core`). Measured: Linux arm64 3.6e-8, macOS x86_64 SSE2 (Rosetta) 1.7e-5,
scalar 2.3e-9, against the macOS arm64 values.

| quantity | key | reference (median or mean, range) | engine | difference | tolerance | result |
|---|---|---|---|---|---|---|
| level, mean power 0.4-0.9 s | 84 / 88 / 90 / 91 / 96 | -39.7 / -40.9 / -39.4 / -41.6 / -38.4 dB | -40.3 / -40.1 / -39.9 / -39.8 / -39.5 | -0.6 / +0.8 / -0.4 / **+1.8** / -1.1 dB | 1.5 dB | pass, FAIL at 91 (3 takes) |
| each oscillator alone (the other's level 0), mean power | 84 / 90 / 96 | osc 1: -41.3 / -41.1 / -40.7; osc 2: -44.4 / -44.1 / -44.1 dB | +0.54 / +0.44 / +0.15; +0.77 / +0.62 / +0.48 dB | | 1.5 dB | pass |
| level against velocity 40/80/127 | 84 / 90 / 96 | within 0.4 dB (no effect) | no effect | 0 | 0.5 dB | pass |
| even harmonics h4-h12 re h2 (median) | 84 / 88 / 90 / 96 | | worst +0.9 / +1.2 / +0.7 / +0.9 dB | | 2 dB | pass |
| | 91 | | +1.7 to +2.2 dB | | 2 dB | FAIL |
| filter cutoff, hold (fit on even harmonics) | 84 / 88 / 90 / 91 / 96 | 652 / 652 / 658 / 635 / 657 Hz | 668 / 667 / 667 / 667 / 667 | +0.41 / +0.39 / +0.25 / **+0.87** / +0.25 semitone | 0.5 semitone | pass, FAIL at 91 |
| filter cutoff, after release | 84 / 88 / 90 / 91 / 96 | 587 / 582 / 586 / 569 / 586 Hz | 596 / 593 / 592 / 592 / 584 | +0.27 / +0.33 / +0.19 / **+0.68** / -0.06 | 0.5 semitone | pass, FAIL at 91 |
| attack, onset to 90 % (broadband) | 84 / 88 / 90 / 91 / 96 | 4.8 / 5.7 / 4.4 / 2.2 / 2.9 ms | 8.5 / 6.9 / 6.2 / 5.9 / 4.6 | **+3.6** / +1.1 / +1.8 / **+3.6** / +1.7 ms | 3 ms | FAIL at 84 and 91 |
| decay to -3 / -6 dB (2nd harmonic) | 88 / 90 / 91 / 96 | 0.074/0.156, 0.070/0.150, 0.063/0.143, 0.073/0.156 s | 0.066/0.141, 0.065/0.140, 0.065/0.138, 0.067/0.143 | within 15 ms | 20 / 30 ms | pass |
| | 84 | 0.011 / 0.011 s | 0.011 / 0.011 | 0 | 20 / 30 ms | pass |
| release rate (-20 to -60 dB) | all five | 84.2-85.6 dB/s | 84.7-85.8 | within 1 % | 5 % | pass |
| release to -20 / -40 / -60 dB | all five | 0.234-0.238 / 0.468-0.475 / 0.701-0.713 s | within 8 ms | | 25 ms | pass |
| voice end (exact zero) after the note-off | all five | 1.008-1.015 s | 1.032 s | +17 to +24 ms | 50 ms | pass |
| pitch (2nd harmonic / 2) | all five | +0.3 to +2.0 cents (-1.5 to +5.2) | +0.4 to +0.5 cents | within 1.6 cents | 6 cents | pass |
| alias, saw alone at 1047 Hz (energy off the harmonics; drift 0) | | -95.9 dB | -94.0 dB | +1.9 dB | at most -80 dB | pass |
| alias, pulse alone at 1047 Hz (drift 0) | | -101.0 dB | -95.1 dB | +5.9 dB | at most -80 dB | pass |
| alias against key, saw / pulse alone, keys 36-108 | | -47 to -102 dB | within 4 dB of the reference from key 60 (below, the metric is window leakage on both) | | informative | |
| the pattern: last note's end after its note-off | 84 88 91 88 | 0.568-0.576 s | 0.575 s | within 7 ms | 25 ms | pass |
| the pattern: per-note level at the note-off re the first note's peak (median over 24 notes) | 3 takes + key 84 repeated | | +0.3 / +0.4 / +0.4 dB (one take: +5.7, normalised on a low first note) | | 1.5 dB | pass (3 of 4) |

What fails, with the evidence:

- **Key 91.** Its three takes are darker than the other keys (cutoff 635 Hz at hold against 652-658 at
  84, 88, 90 and 96) and 1.8 dB quieter; the engine is the same at every key. Not explained; three takes
  only, all from one batch.
- **Attack at keys 84 and 91** (broadband onset to 90 % of the first peak): the engine is 1.1-3.6 ms
  slower at every key. The filter EG's attack law (raw 30) is the amplitude EG's, not measured on the
  filter EG; a faster filter attack would bring the broadband peak earlier. Unverified.
- **Cutoff +0.25 to +0.41 semitone** at the other keys (within the tolerance): drift's mean cutoff offset is
  ruled out (noise through the filter at 636: 592.8/597.3 Hz with drift 64 against 597.3/597.5 with drift
  0). The law is not tuned to it.

Fixed in this round, with the evidence in `docs/coverage.md`: the alias (band-limited tables, measured),
the oscillator out level law (330 is 7.6 dB under 511 in the reference; the earlier table gave 2.6 dB, which
made the pulse 5.5-5.8 dB too loud: the old "+2.2 dB at key 84"), the release threshold (0.443), the
drift's movement. The earlier pulse alias "pass" (-89 dB) was a coincidence of its probe: the pulse sat 5.9
cents sharp, where 44100 / f0 is a whole number and the aliases land on harmonics; the earlier reference
alias figure (-85.9 dB) came from a probe with drift 64, whose movement smears the harmonics.

The pattern (keys 84 88 91 88, sixteenths at 84 BPM, gate 0.9, arpeggio off in the reference; 3 takes
and one of key 84 repeated), the engine driven by the reference's detected onsets and the nominal gate
(`analysis/arpcmp.py`, the key's 2nd harmonic over whole periods of the fundamental, 6 seeds): the notes
release by the fast law in both (the level at the note-off is about 0.45, just over the threshold at 0.443);
the last note ends 0.575 s after its note-off against 0.568-0.576 s; per-note peaks within 0.6 dB and the
level at the note-off within 0.4 dB (medians; one take is normalised on a low first note and reads +5.4 /
+5.7 dB).

# Acceptance: the LFOs against the reference

Probes (measurement run va-round2 (2026-11-14), batches L1 and L2, 44.1 kHz): a sine on oscillator 1 at key 60,
filters thru, drift 0, the pitch tracked (analytic signal of the fundamental band, 1 ms); the filter LFO on a saw through
LPF24 at 636, the amplitude LFO on the sine's envelope. The engine rendered the same patches and events
(`vcbench render`; `analysis/acceptL.py`), on a pulse of width 0.5 (it has no sine wave), its fundamental tracked the
same way.

| quantity | reference | engine | tolerance | result |
|---|---|---|---|---|
| pitch LFO speed at 16 / 100 / 139 / 150 / 208 / 225 / 288 / 320 | 0.06834 / 0.5152 / 1.0304 / 1.28275 / 5.00478 / 8.24319 / 43.066 / 107.67 Hz | within 0.3 % (100: +0.29 %, the others within 0.01 %) | 2 % | pass |
| pitch LFO depth at 250 / 259 / 272 / 283 / 300 / 343 / 375 / 407 / 151 | 1.17 / 5.86 / 14.65 / 27.54 / 69.73 / 205.08 / 365.04 / 525.0 / 205.08 cents | within 0.1 cent | 0.6 cent or 2 % | pass |
| wave shapes (saw, square, triangle, sine; 20 bins a cycle) | as measured | within 2 cents of the reference's profile | 3 cents | pass |
| key-on reset, phase 0 / 45 / 90 / 180 / 270 (start phase, 6 notes each) | 358-360 / 42-43 / 87-89 / 176-179 / 267-269 degrees | 356 / 41 / 86 / 176 / 266 | 5 degrees | pass |
| delay 16 / 32 / 96 | 0.3586 / 1.0180 / 5.3194 s | 0.3595 / 1.0182 / 5.2917 s | 2 % | pass |
| fade-in 128 / 160 / 214 | 0.312 / 1.274 / 13.93 s | 0.309 / 1.275 / 13.84 s | 3 % | pass |
| amplitude LFO depth 144 / 192 / 224 / 64 (dB amplitude of the sine in dB) | 1.13 / 6.02 / 12.04 / 6.02 | 1.13 / 6.02 / 12.05 / 6.02 | 0.2 dB | pass |
| amplitude LFO phase on reset (192 / 64) | 80 / 267 degrees | 86 / 268 | 10 degrees | pass |
| filter LFO swing 280 / 232 (frames matched against the engine's static cutoff bank: reads the engine back) | 51.0 / 50.7 raw steps (626 cents), centre raw 658 | 50.8 / 50.0 (640 cents), centre raw 639 | 3 % | pass (the centre: the static cutoff law, below) |

The filter LFO's law (26.6 cents a step) did not come back from the engine through the analysis it was taken with (a
fitted 4-pole model: 488 against 639 cents); matched frame by frame against the engine's own static cutoffs (va-round3
`analysis/lfoF.py`) the swings agree within 1.4 %. The centre does not: the reference at raw 636 sounds as the engine's
raw 658 (also without the LFO, L2 W_static_c636: 657; at 500 it reads 469, at 800 832), an S-shaped disagreement with the
cutoff law of noise through the filter, open with the converter's cutoff investigation.
With key-on reset off, each voice of the reference runs its own LFO and its phase at a note's start is not tied to the
note-on or to a running clock (18 note starts, two voices at once 25 degrees apart); the engine starts each note at a
uniform random phase. Speeds above 320 (161-1357 Hz in the reference) are not built.

# Acceptance: the controller boxes against the reference

Probes measurement run va-round3 (2026-11-14) batch R1 (a sine at key 60 whose pitch LFO depth the boxes move; the
engine on a pulse of width 0.5, both pitch-tracked; `analysis/boxR1.py`, `analysis/engR1.py`), cutoff and filter EG boxes
(saw through LPF24, reference against reference).

| quantity | reference | engine | tolerance | result |
|---|---|---|---|---|
| mod wheel at 20 values, breath, ribbon, foot controller 1, switches 1-2, pitch bend (as a source) | the box law within 0.3 cent | within 0.4 cent of the reference | 1 cent | pass |
| two boxes summed; +28 and -14; the sum's clamp (two +64 boxes from 0: no vibrato from the wheel's middle up) | as the law | within 0.4 cent | 1 cent | pass |
| bipolar box | as the law | within 0.6 cent | 1 cent | pass |
| curves: Standard 6, Threshold (10, 4) at 17 values | knots | within 3.7 / 1.5 cents (one knot each; elsewhere 0.8) | 1 % of the box | pass |
| a cutoff box landing on 755.75 | sounds as the static 755 (0.00 dB; 756: 0.11 dB) | 755 | exact | pass |
| a filter EG depth box past 448 | sounds as the static 448 (0.00 dB at three wheel values) | stops at 448 | exact | pass |
| a box on the filter EG decay | sounds as the static decay it lands on (within 0.26 dB, eight values) | the decay law at that value | | pass |
| switch 1 at CC 65 | as CC 64 (2.3 cents under the law's 65) | the law (65) | | unexplained, one point |
