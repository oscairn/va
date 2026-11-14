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

A sound's voice plan is data: `vcore::PlanDesc` (which oscillator waves, each filter's type, and the
stage list in signal order). Each stage is a plain function over
structure-of-arrays state (`core/core.cpp`: `oscillators`, `filterRun`, `control`, `amp`); a plan's
kernel is those functions composed at compile time (36 instances, chosen per sound by table
lookup, no virtual call in the audio path). A stage a plan lacks is not in its loops. The same
description could drive another executor (a DSP or FPGA pipeline): the per-voice state layout is the
`Field` list in `core/voices.h`.

## Voices and lanes

Each sound has its own pool of voices, `W` per SIMD lane (4 with NEON or SSE2, 1 scalar). Control
steps run every 16 frames at most (fewer when the host splits a block at an event, so a note starts
on its exact frame), with linear ramps of every coefficient and gain between steps. The filters are the
reference's own structure, found by measurement (round 5, `docs/coverage.md`): a state-variable section with a
unit delay in its loop (lp += F bp; hp = x - lp - q bp; bp += F hp) and a one-pole, cascaded per type; each
filter ramps F, q and the second section's F and q (or the one-pole's c), and its output gain (the resonance's
pass-band gain, a high-pass's gain) rides on the amplitude ramp. The type is chosen once a control step (a
switch outside the sample loop). F stays under 1.95 and q at least 0, so every section is stable (F^2 + 2qF < 4,
qF < 2) whatever the EG, the boxes and the host's modulations add; the cutoff stops at 22.05 kHz. At rates other
than 44.1 kHz each section's F and q are recomputed at control rate so its poles in seconds are the reference's at
44.1 kHz (z' = z^(44100 / rate), in small quantities so that low cutoffs keep their precision). Filters run two
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
[0, 1.5], filter states bounded by the input and the stable filters (q down to 0.008 at resonance 255). No result relies
on denormals (flush-to-zero is set while rendering for speed only) or on NaN propagation. The audio
path uses no libm: 2^x, log2, sin, cos, acos and sqrt-based forms are polynomials or vector instructions at control rate, the pan law a polynomial.
What would need care in fixed point:

- the oscillators read band-limited tables (`phase x 2048`: an 11-bit index and its fraction; the level
  and crossfade weight come from log2 of the increment at control rate);
- the state-variable filter at high resonance (raw above 192, q 0.03-0.008): the integrator states need
  headroom of about 1/q above the input, and F near zero at low cutoffs needs fractional precision
  (F is 0.0004 at raw 0: the reference itself steps F by about 1.8e-4 there);
- the envelopes' per-step coefficients near 1 (long times: 1 - 2^-20 per frame) need 32-bit
  fractions; the release's exact-zero end is a clamp at zero, which is fixed-point friendly.

## SIMD and targets

The kernels use only the small vector type in `core/vsimd.h` (NEON, SSE2, scalar). AVX2 would be a
new branch there with `W = 8` and a second compilation of `core.cpp` behind a runtime CPU check;
pools are 64 voices, a multiple of 8, so nothing else changes. Targets: macOS arm64, Linux arm64 and
x86_64, Windows x86_64 (MSVC; GCC/Clang-only constructs are guarded: the inline-assembly FTZ and the
`always_inline` attribute). The Windows CI and release jobs exist but have never run.
