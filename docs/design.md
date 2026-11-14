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

Each sound has its own pool of voices, `W` per SIMD lane (4 with NEON or SSE2, 1 scalar), its own outputs and its own
random source. Control steps run every 16 frames at most from the start of each render call (fewer when the host splits a
block at an event, so a note starts on its exact frame; the shell splits a sound only at the events of its own channels), with linear ramps of every coefficient and gain between steps. The filters are the
reference's own structure, found by measurement (round 5, `docs/coverage.md`): a state-variable section with a
unit delay in its loop (lp += F bp; hp = x - lp - q bp; bp += F hp) and a one-pole, cascaded per type; each
filter ramps F, q and the second section's F and q (or the one-pole's c), and its output gain (the resonance's
pass-band gain, a high-pass's gain) rides on the amplitude ramp. The type is chosen once a control step (a
switch outside the sample loop). F stays under 1.95 and q at least 0, so every section is stable (F^2 + 2qF < 4,
qF < 2) whatever the EG, the boxes and the host's modulations add; the cutoff stops at 22.05 kHz. At rates other
than 44.1 kHz each section's F and q are recomputed at control rate so its poles in seconds are the reference's at
44.1 kHz (z' = z^(44100 / rate), in small quantities so that low cutoffs keep their precision). Filters run two
lanes interleaved (bit-identical to one at a time; faster on in-order cores). A voice ends when its
amplitude envelope reaches exactly zero in its release, when its release reaches its quiet end (below), or when its fade
(`choke`) ends; its slot is freed in the same step.

### The quiet end of a release

A released note (after its note-off; a key the sustain pedal holds is not released: the shell sends the note-off only when the
pedal lets it go) also ends once a bound of its output is below -96 dBFS, whichever comes first with the envelope's own end
(exactly zero) and a fade's. A held note never ends by it, however quiet its sustain. It is a deliberate departure from the
envelope's full span, approved by the project's owner: a release stored long (raw 255: about 70 s to the envelope's own end)
would otherwise keep a voice's cost long after it is inaudible.

The bound is not a signal detector: it is the product of the gains on the voice's way out with its oscillators at full scale,
taken where they could be largest (`quietEnds` in `core/core.cpp`). The oscillators' sum: each oscillator in the plan at its
top level (oscillators 1-2 at raw 511, since a box or the host's modulation may take them there; oscillator 3 at its own) times
the tables' largest sample (`kTabPeak`, 1.281; a pulse of another width than 0.5 is two reads: twice that). Each filter's
resonant peak, 1 / (q sqrt(1 - q^2 / 4)) a section (1 at q of sqrt 2 and above, no bound at q under 1e-4), at the damping it
would have at the highest cutoff the voice can still reach: the cutoff as the boxes and the host set it now, the filter EG at
its level now if it raises the cutoff (it only falls in the release), the voice's key follow, velocity and drift, and the
filter LFO's whole depth; the damping is the resonance's (with the voice's velocity) under the ceiling 3 - 2F that high
cutoffs impose, and the second section of a 24 dB type at its own q of 1 under the same ceiling; a one-pole adds nothing. The
filters' output gains (the resonance's pass-band gain, a high-pass's gain at its loudest cutoff). The saturators: y = x + c x^2
less its mean is at most x (1 + 2 |c| x), c with the amplitude EG at its level now. Then the note's volume, the sound's level,
the caller's gain, the louder pan side and the louder of a unison copy's two channel gains. The amplitude LFO only lowers the
level (its top is the level) and a fade only falls: both count as 1. The engine applies no velocity or key scaling of level
(not covered), so there is none to bound. A unison note ends as a whole: its copies share one envelope, and the bound is the sum
of theirs. The bound uses the 44.1 kHz coefficients at every rate (the poles are mapped to the same places in seconds).

The envelope level at which that bound times the envelope is -96 dBFS is stored per voice (field `QEND`) when the release starts
and again whenever a term changes (the note's volume, the caller's gain or pan, a box source, a knob or panel control, a
destination set or modulated, a new sound installed): such a change marks the sound, and its released voices are recomputed
once at the start of the next render call. The end test that already runs after each control step of a sound with a voice
releasing compares the envelope with it in place of zero, so the rule costs nothing a frame or a control step. A change after a
note ended does not bring it back; the level terms a box or the host can move are taken at their top for that reason, the
filter's peak as the filter stands. A filter that rings after its input stops is not counted, as by the envelope's own end.

A unison note (`Unison` 1 or 2) is 2 or 4 whole voices in the pool, each with its own phases and drift, its detune and its two
channel gains as per-voice fields (0 and 1 for a note of one voice, so such a note renders bit for bit as before). The note's
voices are one note everywhere else: a note-off, a fade, a steal, a mono take-over and a glide move all of them, and the slot is
freed when they end, together; none is ever given up alone. The voice budget counts each (the extension's "one unison copy"
is a voice, and the plan's cost is one voice's): a unison-4 note needs 4 voices of a limit, and the shell gives the note up whole.

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
