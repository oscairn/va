# Design of the DSP core

## Layers

- `core/` is a plain C++17 library: no plugin API, OS, threads, files, heap, exceptions or RTTI.
  The caller owns the `vcore::Core` object (`sizeof(Core)`, everything preallocated: 8 sounds, 64
  voice slots). `core/vcore.h` is its whole interface.
- `src/core_engine.cpp` implements the shell's `engine.h` on it; `src/plugin.cpp` is the only part
  that knows CLAP. A sound is compiled on the main thread (`Core::compile`: the laws, libm allowed)
  into a triple buffer per sound context and installed by the audio thread (`Core::install`, a copy)
  before it renders, starts a note or applies a parameter: no lock, no data race.

## The patch and the laws

A patch is the reference model's own parameters in raw units, all 671 of them (`core/params.inc`: the
names, raw ranges and defaults of the reference model's published parameter interface, reproduced so that
a patch carries its raw values; text form `vcore-patch/1 Id=value ...`). The format does
not change when a feature is added: what the core does not reproduce is reported by
`vcore::notCovered()` (`docs/coverage.md`). The laws (`core/laws.cpp`, knot tables in
`core/laws.inc`) were fitted to our own recordings of the reference's audio output (probe patches, recorded
and analysed by us in the measurement runs va-core, va-fidelity, va-round2, va-round3 and later, from 2026-11-14; the
recordings and the analysis scripts are not published, and the docs name a run and its batch only to say where a figure came from); they turn raw values into physical ones once per sound: cutoff in Hz, section Qs and
pass-band gain, time constants in seconds, levels in dB, cents.

## Plans

A sound's voice plan is data: `vcore::PlanDesc` (which oscillator waves, whether the low-pass and
high-pass stages exist, and the stage list in signal order). Each stage is a plain function over
structure-of-arrays state (`core/core.cpp`: `oscillators`, `svf4`, `control`, `amp`); a plan's
kernel is those functions composed at compile time (36 instances, chosen per sound by table
lookup, no virtual call in the audio path). A stage a plan lacks is not in its loops. The same
description could drive another executor (a DSP or FPGA pipeline): the per-voice state layout is the
`Field` list in `core/voices.h`.

## Voices and lanes

Each sound has its own pool of voices, `W` per SIMD lane (4 with NEON or SSE2, 1 scalar). Control
steps run every 16 frames at most (fewer when the host splits a block at an event, so a note starts
on its exact frame), with linear ramps of every coefficient and gain between steps; a filter section ramps its
`g` only and computes its normaliser 1 / (1 + g (g + k)) from it every sample (ramping the two apart made
the sections unstable under fast sweeps: a knob box at ratio +127 on the filter EG depth reached 2.7e8). The
cutoff stops at the filter's top (the law at raw 1023), whatever the EG and the boxes add. Filters run two
lanes interleaved (bit-identical to one at a time; faster on in-order cores). A voice ends when its
amplitude envelope reaches exactly zero in its release, or when its fade (`choke`) ends; its slot is
freed in the same step.

## Control rate against the reference

The reference model's code processes in pages of 64 samples (its per-module page-size accessor
returns 64) and computes the filter coefficients in a vector loop per page; whether that loop runs
over the page's samples or over voices was not established from the code. By measurement: the
fastest filter sweep available (filter EG release over 17 semitones, `G_d288`) and the amplitude
attack and decay of probe A show no periodic artefact at lags 16, 32 or 64 samples above the high
band's own content (-55 dB re the signal); the reference either computes the control values per
sample or interpolates them. This core's 16-frame ramps leave a periodic residue at -68 dB on the
same probe and analysis, under the reference's own high-band content, so 16 frames with linear
ramps were kept. The LFOs are computed at the control steps (each voice its own phase; the pitch reaches the
oscillators through the increment ramps): up to speed 320 (108 Hz) that is 25 steps a cycle or more at 44.1 kHz, and
the engine's LFO frequency and depth match the reference's within 0.1 %; the reference's speeds above 320 reach
1357 Hz (audio rate) and are reported as not covered.

## Arithmetic and fixed point

Every per-sample value is bounded: phases in [0, 1), increments at most 0.45, envelope levels in
[0, 1.5], filter states bounded by the input and the stable filters (Q at most 59). No result relies
on denormals (flush-to-zero is set while rendering for speed only) or on NaN propagation. The audio
path uses no libm: 2^x and tan are polynomials at control rate, the pan law a polynomial.
What would need care in fixed point:

- the oscillators read band-limited tables (`phase x 2048`: an 11-bit index and its fraction; the level
  and crossfade weight come from log2 of the increment at control rate);
- the state-variable filter at high Q (raw resonance above 192, Q 26-59): the integrator states need
  headroom of about Q above the input, and `g` near zero at low cutoffs needs fractional precision
  (about 24 bits for 30 Hz at 48 kHz);
- the envelopes' per-step coefficients near 1 (long times: 1 - 2^-20 per frame) need 32-bit
  fractions; the release's exact-zero end is a clamp at zero, which is fixed-point friendly.

## SIMD and targets

The kernels use only the small vector type in `core/vsimd.h` (NEON, SSE2, scalar). AVX2 would be a
new branch there with `W = 8` and a second compilation of `core.cpp` behind a runtime CPU check;
pools are 64 voices, a multiple of 8, so nothing else changes. Targets: macOS arm64, Linux arm64 and
x86_64, Windows x86_64 (MSVC; GCC/Clang-only constructs are guarded: the inline-assembly FTZ and the
`always_inline` attribute). The Windows CI and release jobs exist but have never run.
