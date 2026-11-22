# Oscairn VA: plan

## 1. Status

Version 0.11.1 (the version measured below). Implementation language today: C++ (the DSP core in `core/` and the
plugin shell). Everything in Oscairn's plugin set is to end up in Rust: the engines port after their coverage work, and new effect types are written in Rust from the start.
From 0.11.2 the voice budget counts notes and answers draft 2's importance report ([design.md](design.md#the-voice-budget)).

## 2. Supported

What the engine plays of the reference's analog engine today, in short. The parameter-by-parameter record, with the
ranges each law was checked over, is [coverage.md](coverage.md); plugin parameters: [parameters.md](parameters.md);
comparison with the reference: [acceptance.md](acceptance.md); cost: [cost.md](cost.md); design:
[design.md](design.md). A patch is the model's raw parameter values (`core/params.inc`).

| Area | Played today | Not yet (reported, not approximated) |
|---|---|---|
| Oscillators | three oscillators, all five waves; octave and pitch; pitch LFO depth; pulse width on oscillators 1 and 2 with its LFO; out level 90-511; unison with detune and spread; voltage drift | oscillator FM, ring, self sync and wave shaper; oscillator pitch and pulse width envelopes; velocity sensitivities of the oscillator levels; oscillator 3's pulse width other than 128; out select and invert; oscillator reset modes; the noise generator |
| Filters | two filters: low-pass 24/18/12/6, high-pass 24/18/12/6 and thru; cutoff 0-1023; key follow; cutoff velocity; envelope depth 96-448; LFO depth; resonance where the cutoff stays under 2.1 kHz; saturator drive at nine values | band-pass types; resonance at high cutoffs; filter out level other than 64; saturator velocity sensitivity |
| Envelopes | filter and amplitude envelopes (decay law over 100-255, filter release over 55-255) | pitch envelope depth; envelope time velocity sensitivity |
| LFOs | pitch LFO (speed 0-320); the filter LFO shares its block; amplitude LFO with the sine | audio-rate LFO speeds; the amplitude LFO's other waves, phase, delay and fade in; the part LFO with any depth |
| Level | amplitude level 56-511; amplitude LFO depth | level velocity sensitivity other than neutral; level key follow; part detune |
| Modulation | 16 controller boxes, sources 0-3 and 6-17 (foot controller 2 and the foot switch as plugin parameters), the box destinations as modulatable parameters ([parameters.md](parameters.md)); knobs 1-8, ribbon and switches 1-2 as parameters | other box destinations; sequenced and envelope-following sources; unmeasured curves; Key Controller Sets |
| Voices | mono and poly, key assign, portamento (switch, time, mode, time mode), pitch bend range, note shift, zone limits and transposes, polyphony limit | legato slope other than 0; part envelope and resonance offsets; key-on delay; pan modulation (random, alternate, scaling); micro tuning |
| Modifier | wave folder only at its neutral depth | the wave folder |
| Outputs | an output bus per sound (main or sound bus 1-8) | - |

## 3. Coverage

Measured with VA 0.11.1 on 2026-11-17. Two sets of Performances of the reference were converted: its factory (stock) set of 3631 Performances and a personal library of 87 Performances. "Fully covered": the engine's own coverage report lists nothing for that part that the engine does not play; it says nothing about effects, controls, scenes or sequences. "Best effort": the part plays, with at least one item reported. Neither set has a Performance with no sound-affecting drop yet (0 of 3631, 0 of 87): every Performance still needs work somewhere, in this plugin or elsewhere.

| Parts on this engine | Total | Fully covered | Best effort | Muted |
|---|---|---|---|---|
| Stock set | 814 | 3 | 811 | 0 |
| Personal library | 45 | 34 | 11 | 0 |

### Gaps

155 gaps of the stock set belong to this engine. "Changes the sound" is a classification of the gap, not a listening test: a stored value away from its default that is not carried counts as sound-affecting whether or not it is heard in a given Performance. Counts are Performances and parts of the stock set that carry the gap; parts switched off or muted are not counted against their other gaps. Where the engine's report gives a reason, it follows the
parameter id.

| Gap | Performances (stock) | Parts (stock) | Changes the sound | Status |
|---|---|---|---|---|
| `BoxN ControllerSetDestination`: controller box destination: not built | 474 | 683 | yes | partly done (0.11.5): the report names each destination's class: the host's (an effect's parameter, a send, the arpeggio, the motion sequence, the part's pan), out of scope (self sync, wave shaper, FM, ring, noise, the modifier), not built (the pitch and pulse width EGs, the part LFO); played since: 54 (the part's pitch), 119 (every LFO's speed), 150 (the amplitude LFO's depth), 375 and 376 (the unison detune and spread) |
| `AmplitudeLevelVelocitySensitivity`: velocity sensitivity: measured only at 0 (no effect) | 202 | 312 | yes | open |
| `Filter1FilterSaturatorDrive`: saturator drive: measured at 0, 2, 4, 7, 8, 12, 15, 16 and 23 | 188 | 272 | yes | open |
| `PartKeyControllerSet1Switch`: key controller boxes: not built | 164 | 220 | yes | open |
| `Filter1FilterResonance`: resonance at high cutoffs: the fit misses by over 1 dB there (any value to 2.1 kHz, LPF24 0.7 kHz; to 64 up to 5.9 / 2.1 kHz; 32 anywhere) | 162 | 208 | yes | open |
| `BoxN ControllerSetCurveType`: controller box curve: not measured | 135 | 184 | yes | partly done (0.11.4): 35 preset curves measured at the mod wheel's 128 values; counted again with the engine's report on the stock set read through the converter, 60 parts (49 Performances) on curves still not measured (fully covered parts 3 to 4) |
| `AmplitudeSaturatorDrive`: saturator drive: measured at 0, 2, 4, 7, 8, 12, 15, 16 and 23 | 115 | 194 | yes | open |
| `Filter1FilterOutLevel`: filter out level: no measured law | 106 | 134 | yes | open |
| `Ageing`: aging: no effect on the drift (0-200 at drift 0, 100-200 at drift 64 and 100); with a release under 100 it moves the voices' spread of release times, not built | 105 | 157 | yes | open |
| `AmplitudeLevelKeyFollow`: amplitude key follow: no measured law | 105 | 152 | yes | open |
| `FilterCutoffLFOWave`: filter LFO wave: only the sine verified on the filter LFO | 101 | 127 | yes | open |
| `RandomPanDepth`: pan modulation: not built | 94 | 119 | yes | open |
| `NoiseGeneratorOutLevel`: noise: out of scope | 88 | 145 | yes | open |
| `AlternatePanDepth`: pan modulation: not built | 83 | 104 | yes | open |
| `Osc1OscillatorFMLevel`: FM: out of scope | 79 | 133 | yes | open |
| `Osc1OscillatorSelfSyncEGDepth`: sync: out of scope | 72 | 97 | yes | open |
| `Osc1OscillatorPitchEGDepth`: pitch EG: not built | 68 | 103 | yes | open |
| `Osc1OscillatorSelfSyncPitch`: sync: out of scope | 67 | 94 | yes | open |
| `PartKeyControllerSet2Switch`: key controller boxes: not built | 63 | 87 | yes | open |
| `BoxN ControllerSetSource`: controller box source: not built (sequenced and envelope-following sources) | 63 | 81 | yes | open |
| `Osc2OscillatorPitchEGDepth`: pitch EG: not built | 56 | 72 | yes | open |
| `Osc2OscillatorOutLevelVelocitySensitivity`: velocity sensitivity: no measured law | 55 | 80 | yes | open |
| `Osc2OscillatorSelfSyncPitch`: sync: out of scope | 52 | 75 | yes | open |
| `Osc1OscillatorOutLevelVelocitySensitivity`: velocity sensitivity: no measured law | 51 | 77 | yes | open |
| `OSCReset`: oscillator reset modes: not built (free running only) | 50 | 65 | yes | open |
| `Osc1OscillatorRingLevel`: ring: out of scope | 49 | 71 | yes | open |
| `Filter2FilterSaturatorDrive`: saturator drive: measured at 0, 2, 4, 7, 8, 12, 15, 16 and 23 | 46 | 58 | yes | open |
| `Filter2FilterResonance`: high-pass resonance: its pass-band gain not measured | 45 | 56 | yes | open |
| `AmplitudeEGDecayTime`: decay (with its boxes): law measured 100-255 | 42 | 70 | yes | open |
| `Osc1OscillatorWaveShaper`: wave shaper: out of scope | 42 | 49 | yes | open |
| `Osc3OscillatorOutLevelVelocitySensitivity`: velocity sensitivity: no measured law | 38 | 59 | yes | open |
| `Osc2OscillatorOutLevel`: a box raising a silent oscillator: not built | 38 | 44 | yes | open |
| `Filter1FilterSaturatorDriveVelocitySensitivity`: saturator velocity sensitivity: not built | 38 | 43 | yes | open |
| `Osc2OscillatorFMLevel`: FM: out of scope | 36 | 48 | yes | open |
| `AmplitudeLFOWave`: amplitude LFO wave: only the sine's phase is measured | 35 | 46 | yes | open |
| `Osc3OscillatorPulseWidth`: oscillator 3's pulse: only width 128 is built | 33 | 45 | yes | open |
| `BoxN ControllerSetCurveType`: controller box curve: measured at 17 points, a live source moves between them | 32 | 36 | yes | done (0.11.4): the 7-bit sources land on the curves' 128 measured values; the bend, which moves between them, is reported (counted again: 0 parts) |
| `Osc1OscillatorSelfSyncLFODepth`: sync: out of scope | 31 | 46 | yes | open |
| `BoxN ControllerSetDestination`: controller box on oscillator 3's level: not built | 31 | 40 | yes | open |
| `Filter2FilterOutLevel`: filter out level: no measured law | 30 | 37 | yes | open |
| `PitchLFOFadeInTime`: pitch LFO fade-in under 96: shorter than 76 ms, not resolved | 29 | 40 | yes | open |
| `Filter1FilterResonanceVelocitySensitivity`: resonance velocity sensitivity: measured at velocities 1 and 127 only | 29 | 38 | yes | open |
| `BoxN ControllerSetDestination`: controller box on oscillator 3's pulse width: not built | 28 | 30 | yes | open |
| `Osc3OscillatorPitchEGDepth`: pitch EG: not built | 27 | 43 | yes | open |
| `Osc2OscillatorRingLevel`: ring: out of scope | 27 | 40 | yes | open |
| `PartKeyControllerSet3Switch`: key controller boxes: not built | 27 | 40 | yes | open |
| `BoxN ControllerSetDestination`: controller box on filter 2's cutoff LFO depth: not measured | 27 | 38 | yes | open |
| `ScalingPanDepth`: pan modulation: not built | 27 | 30 | yes | open |
| `Osc1OscillatorPulseWidthEGDepth`: pulse width EG: not built | 26 | 39 | yes | open |
| `Filter1FilterSaturatorDrive`: filter 1's saturator with filter 2 a filter: the saturators run after both filters, so not built | 26 | 37 | yes | open |
| `FilterCutoffLFOFadeInTime`: filter LFO fade-in: not measured on the filter LFO | 26 | 32 | yes | open |
| `Osc2OscillatorSelfSyncEGDepth`: sync: out of scope | 25 | 37 | yes | open |
| `Osc2OscillatorWaveShaper`: wave shaper: out of scope | 25 | 36 | yes | open |
| `FilterCutoffLFODelayTime`: filter LFO delay: not measured on the filter LFO | 25 | 31 | yes | open |
| `Osc1OscillatorPulseWidthLFODepth`: pulse width LFO depth: measured 128-192 | 23 | 31 | yes | open |
| `Osc3OscillatorSelfSyncPitch`: sync: out of scope | 23 | 31 | yes | open |
| `Filter2FilterResonance`: resonance at high cutoffs: the fit misses by over 1 dB there (any value to 2.1 kHz, LPF24 0.7 kHz; to 64 up to 5.9 / 2.1 kHz; 32 anywhere) | 23 | 26 | yes | open |
| `PartLfoLFODepth1`: part LFO: not built | 22 | 37 | yes | open |
| `FilterCutoffEGReleaseTime`: release (with its boxes): law measured 55-255 | 22 | 30 | yes | open |
| `AmplitudeEGTimeVelocitySensitivity`: EG time velocity sensitivity: no measured law | 21 | 33 | yes | open |
| `Osc1OscillatorWaveShaperEGDepth`: wave shaper: out of scope | 21 | 31 | yes | open |
| `Osc2OscillatorSelfSyncLFODepth`: sync: out of scope | 21 | 28 | yes | open |
| `PartDetune`: part detune: not built | 20 | 31 | yes | open |
| `AmplitudeLFOFadeInTime`: amplitude LFO fade-in: not measured | 19 | 26 | yes | open |
| `FilterCutoffEGDecayTime`: decay (with its boxes): law measured 100-255 | 19 | 24 | yes | open |
| `Osc1OscillatorFMLevelVelocitySensitivity`: velocity sensitivity: no measured law | 19 | 23 | yes | open |
| `ModifierWaveFolder`: wave folder: out of scope | 18 | 30 | yes | open |
| `PartKeyControllerSet4Switch`: key controller boxes: not built | 18 | 24 | yes | open |
| `FilterCutoffEGTimeVelocitySensitivity`: EG time velocity sensitivity: no measured law | 17 | 24 | yes | open |
| `Osc2OscillatorPulseWidthLFODepth`: pulse width LFO depth: measured 128-192 | 16 | 23 | yes | open |
| `AmplitudeLFODelayTime`: amplitude LFO delay: not measured | 16 | 20 | yes | open |
| `FilterCutoffLFOSpeed`: filter LFO speed over 320: audio rate, not built | 15 | 20 | yes | open |
| `Osc2OscillatorOutLevel`: out level under 90 (with its boxes): no measured law | 15 | 19 | yes | open |
| `Osc3OscillatorSelfSyncLFODepth`: sync: out of scope | 15 | 18 | yes | open |
| `Osc2OscillatorPulseWidthEGDepth`: pulse width EG: not built | 14 | 27 | yes | open |
| `Osc1OscillatorWaveShaperLFODepth`: wave shaper: out of scope | 14 | 20 | yes | open |
| `Osc3OscillatorSelfSyncEGDepth`: sync: out of scope | 14 | 19 | yes | open |
| `Filter2FilterCutoffLFODepth`: filter 2 LFO depth: not measured | 14 | 16 | yes | open |
| `Osc3OscillatorOutLevel`: out level under 90: no measured law | 14 | 16 | yes | open |
| `Filter2FilterSaturatorDriveVelocitySensitivity`: saturator velocity sensitivity: not built | 13 | 17 | yes | open |
| `KeyOnDelayTimeLength`: key-on delay: not built | 13 | 16 | yes | open |
| `Filter1FilterType`: band-pass filters: not built | 11 | 16 | yes | open |
| `Osc1OscillatorWave`: controller box on the wave with a pulse width other than 128 or moving: not built | 11 | 16 | yes | open |
| `Osc3OscillatorWaveShaper`: wave shaper: out of scope | 11 | 16 | yes | open |
| `Osc1OscillatorPitchEGDepthVelocitySensitivity`: velocity sensitivity: no measured law | 11 | 12 | yes | open |
| `BoxN ControllerSetCurveType`: controller box curve: measured at 17 points, the knob is between them | 10 | 18 | yes | open: since 0.11.4 a knob more than 2.5 from one of the mod wheel's 128 values; counted again, 26 parts (15 Performances), more than before as knob boxes on the newly measured curves now reach this check |
| `Osc2OscillatorPulseWidthVelocitySensitivity`: velocity sensitivity: no measured law | 10 | 17 | yes | open |
| `PartLfoLFODepth2`: part LFO: not built | 10 | 16 | yes | open |
| `Osc1OscillatorRingLevelVelocitySensitivity`: velocity sensitivity: no measured law | 10 | 13 | yes | open |
| `Osc1OscillatorSelfSyncPitchVelocitySensitivity`: velocity sensitivity: no measured law | 10 | 12 | yes | open |
| `Osc3OscillatorPulseWidthEGDepth`: pulse width EG: not built | 10 | 12 | yes | open |
| `Osc2OscillatorPitchEGDepthVelocitySensitivity`: velocity sensitivity: no measured law | 10 | 11 | yes | open |
| `Filter1FilterCutoffLFODepth`: filter LFO depth (with its boxes): verified up to 128 steps from 256 | 9 | 11 | yes | open |
| `Filter1FilterCutoffLFODepth`: filter LFO depth: verified up to 128 steps from 256 | 9 | 11 | yes | open |
| `Osc1OscillatorOutLevel`: a box raising a silent oscillator: not built | 9 | 9 | yes | open |
| `Osc1OscillatorWaveShaperVelocitySensitivity`: velocity sensitivity: no measured law | 8 | 12 | yes | open |
| `Osc2OscillatorFMLevelVelocitySensitivity`: velocity sensitivity: no measured law | 8 | 10 | yes | open |
| `PartAEGAttackTime`: part offset: not built | 7 | 14 | yes | open |
| `PartAEGReleaseTime`: part offset: not built | 7 | 13 | yes | open |
| `Osc3OscillatorWaveShaperEGDepth`: wave shaper: out of scope | 7 | 10 | yes | open |
| `Osc3OscillatorFMLevelVelocitySensitivity`: velocity sensitivity: no measured law | 7 | 7 | yes | open |
| `Osc3OscillatorRingLevelVelocitySensitivity`: velocity sensitivity: no measured law | 7 | 7 | yes | open |
| `PartVelocitySensitivityDepth`: velocity curve: measured only at depth 64 | 6 | 10 | yes | open |
| `Osc2OscillatorWaveShaperLFODepth`: wave shaper: out of scope | 6 | 9 | yes | open |
| `Filter2FilterType`: band-pass filters: not built | 6 | 7 | yes | open |
| `Filter2FilterCutoffEGDepth`: EG depth (with its boxes) under 96: law measured 96-448 (it stops at 448) | 6 | 6 | yes | open |
| `Osc2OscillatorSelfSyncPitchVelocitySensitivity`: velocity sensitivity: no measured law | 6 | 6 | yes | open |
| `PartFilterResonanceWidth`: part offset: not built | 5 | 11 | yes | open |
| `PartLegatoSlope`: legato slope: measured at 0 only | 5 | 11 | yes | open |
| `PartLfoLFODepth3`: part LFO: not built | 5 | 9 | yes | open |
| `PartLfoLFODestination1`: outside the engine's range 0-69 | 5 | 9 | yes | open |
| `Osc2OscillatorWave`: controller box on the wave with a pulse width other than 128 or moving: not built | 5 | 8 | yes | open |
| `Osc1OscillatorOutSelect`: output to the amplifier: not built | 5 | 6 | yes | open |
| `Osc3OscillatorWaveShaperLFODepth`: wave shaper: out of scope | 5 | 6 | yes | open |
| `Osc2OscillatorWaveShaperEGDepth`: wave shaper: out of scope | 5 | 5 | yes | open |
| `PitchLFOSpeed`: pitch LFO speed over 320: audio rate, not built | 5 | 5 | yes | open |
| `PartAEGDecayTime`: part offset: not built | 4 | 7 | yes | open |
| `Filter2FilterCutoff`: HPF18 / HPF6: measured at cutoffs 340-960 | 4 | 6 | yes | open |
| `Osc1OscillatorOutLevel`: out level under 90 (with its boxes): no measured law | 4 | 4 | yes | open |
| `Osc3OscillatorWaveShaperVelocitySensitivity`: velocity sensitivity: no measured law | 3 | 6 | yes | open |
| `Filter1FilterCutoffEGDepth`: EG depth (with its boxes) under 96: law measured 96-448 (it stops at 448) | 3 | 5 | yes | open |
| `Filter2FilterResonanceVelocitySensitivity`: resonance velocity sensitivity: measured at velocities 1 and 127 only | 3 | 5 | yes | open |
| `Osc2OscillatorRingLevelVelocitySensitivity`: velocity sensitivity: no measured law | 3 | 5 | yes | open |
| `Osc2OscillatorWaveShaperVelocitySensitivity`: velocity sensitivity: no measured law | 3 | 5 | yes | open |
| `Osc3OscillatorPulseWidthVelocitySensitivity`: velocity sensitivity: no measured law | 3 | 5 | yes | open |
| `AmplitudeLFOPhase`: amplitude LFO phase: measured at 0 only | 3 | 4 | yes | open |
| `Filter1FilterResonance`: high-pass resonance: its pass-band gain not measured | 3 | 4 | yes | open |
| `Osc1OscillatorPulseWidthVelocitySensitivity`: velocity sensitivity: no measured law | 3 | 4 | yes | open |
| `PartPortamentoTime`: controller box on the portamento time in time modes 1 and 3: not built | 3 | 4 | yes | open |
| `Osc2OscillatorOutSelect`: output to the amplifier: not built | 3 | 3 | yes | open |
| `Osc3OscillatorOutSelect`: output to the amplifier: not built | 3 | 3 | yes | open |
| `PartVelocitySensitivityOffset`: velocity curve: measured only at offset 64 | 3 | 3 | yes | open |
| `PitchLFODelayTime`: pitch LFO delay over 96: not measured (law extrapolated past 5.3 s) | 3 | 3 | yes | open |
| `PartLfoLFODestination2`: outside the engine's range 0-69 | 2 | 6 | yes | open |
| `PartLfoLFODestination3`: outside the engine's range 0-69 | 2 | 6 | yes | open |
| `PartFilterCutoffFrequency`: the part's cutoff offset on filter 2: not measured | 2 | 5 | yes | open |
| `Osc3OscillatorPitchEGDepthVelocitySensitivity`: velocity sensitivity: no measured law | 2 | 4 | yes | open |
| `PartFEGDepth`: part FEG depth offset: law measured 56-80 | 2 | 4 | yes | open |
| `Osc3OscillatorSelfSyncPitchVelocitySensitivity`: velocity sensitivity: no measured law | 2 | 3 | yes | open |
| `Osc3OscillatorWave`: controller box on the wave with a pulse width other than 128 or moving: not built | 2 | 3 | yes | open |
| `PartAEGSustainLevel`: part offset: not built | 2 | 3 | yes | open |
| `Osc2OscillatorLFODelayTime`: oscillator LFO delay: not measured | 2 | 2 | yes | open |
| `Osc2OscillatorLFOFadeInTime`: oscillator LFO fade-in: not measured | 2 | 2 | yes | open |
| `Osc2OscillatorOutInvertEnable`: invert: not built | 2 | 2 | yes | open |
| `Osc3OscillatorOutInvertEnable`: invert: not built | 2 | 2 | yes | open |
| `Osc3OscillatorRingLevel`: ring: out of scope | 2 | 2 | yes | open |
| `PartFEGReleaseTime`: part offset: not built | 2 | 2 | yes | open |
| `PartFilterCutoffFrequency`: part cutoff offset: law measured 48-96 | 1 | 3 | yes | open |
| `Osc1OscillatorLFODelayTime`: oscillator LFO delay: not measured | 1 | 2 | yes | open |
| `Filter1FilterCutoff`: HPF18 / HPF6: measured at cutoffs 340-960 | 1 | 1 | yes | open |
| `KeyOnDelayTempoSyncSwitch`: key-on delay: not built | 1 | 1 | yes | open |
| `Osc1OscillatorLFOSpeed`: oscillator LFO speed over 320: audio rate, not built | 1 | 1 | yes | open |
| `Osc2OscillatorLFOSpeed`: oscillator LFO speed over 320: audio rate, not built | 1 | 1 | yes | open |
| `PartFEGDecayTime`: part offset: not built | 1 | 1 | yes | open |
| `PartFEGSustainLevel`: part offset: not built | 1 | 1 | yes | open |

## 4. Port to Rust

Status: not started.

A port counts as done when all of these hold: the test suite and the cost gates below are carried over and pass against the Rust build; a saved state of the C++ version opens and plays the same; the plugin id, the parameter ids and the sound format stay the same. For this plugin:

- Tests: the plugin-level suite (`va_tests`, driving the built plugin through its binary interface only) and the
  core's own tests (`vcore_tests`).
- Cost gate: `tools/cost/cg.sh` against `tools/cost/baseline.txt` ([cost.md](cost.md)).
- Saved state: `oscairn-va-state 5` and the snapshot `oscairn-va-contexts/5` open; a state 4 still loads as it does
  today.
- Same plugin id (`org.oscairn.va`), parameter ids 0-44 ([parameters.md](parameters.md)) and raw patch format
  (`core/params.inc`).

## 5. Shared building blocks

At the port to Rust the engine's parts become a library other analog-style engines are built from: oscillators,
filters, envelopes, LFOs, the controller matrix, and the aging and drift behavior. An engine is then a wiring of
those blocks plus its parameter laws, compiled together, so the audio loop makes no choice at run time. A variant
that shares this engine's wiring and parameters and differs only in its laws and shapes is a model stored in the
sound; a different wiring is a separate plugin on the same library. The library is a build-time dependency at a
pinned version: the shipped plugin stays self-contained. Its plan lives here until it has a repository of its own.
Until the port, new work keeps those parts separable. The law set's schema and how a model is
structured in code are in [models.md](models.md).

## 6. Next

This engine's package is fifteenth of seventeen in the cross-engine order (495 stock Performances touched). It comes
late only because those Performances also need other packages; 811 of 814 stock parts are best effort today. In build
order:

1. Controller box destinations not built (474 Performances).
2. Amplitude level velocity sensitivity (202).
3. Filter saturator drive between the measured values (188).
4. Key controller sets (164 for set 1).
5. Resonance at high cutoffs (162).
