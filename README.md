# Oscairn VA

A virtual analog instrument for [Oscairn](https://github.com/oscairn/oscairn), as a CLAP plugin
for macOS, Linux and Windows.

Licence: GNU Affero General Public License v3.0 or later (`LICENSE`); the vendored headers under `third_party/` keep their own licences (MIT; `CREDITS.md`).

Status: version 0.11.7 (plugin id `org.oscairn.va`), not complete. "The reference" in these docs is
the analog-modelling synth engine this project is measured against. The DSP core (`core/`, a
plain C++ library) plays three oscillators, both filters with low-pass and high-pass types (the two
band-pass types are not built), key follow and velocity on the filters, the filter and amplitude
envelopes with their measured laws, the LFOs, portamento and controller boxes. A patch is the model's own raw parameter values (`core/params.inc`); what the core
does not cover is reported, not approximated (`docs/coverage.md`). How it compares with the
reference, measured so far on one test patch: `docs/acceptance.md`; cost: `docs/cost.md`; design: `docs/design.md`. The shell does
CLAP, parameters and state, and the Oscairn instrument contract (`oscairn.sound-contexts`,
`oscairn.voice-budget`, `oscairn.memory`), using the Oscairn SDK's helpers.

A sound's patch text (sound contexts' `prepare`): `level=`, `pan=`, `polyphony=`, `knob1=`..`knob8=` and
the core's parameters by id, e.g. `Filter1FilterCutoff=636 Filter1FilterResonance=50`. The CLAP
parameters and their ids: `docs/parameters.md`.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

This builds `build/oscairn-va.clap`, `build/va_tests`, `build/vcore_tests` (the core's own tests, linked
directly) and `build/vcbench` (bench, render, coverage: `tools/vcbench.cpp`). The instruction-cost
gate is `tools/cost/cg.sh` (callgrind, aarch64; `docs/cost.md`). The tests load the built `.clap` and
drive it only through the CLAP ABI and the Oscairn extension structs (`tests/host.h`); they link no
plugin code. Heap allocations inside `process()` are counted by interposing the C allocator in the
test process, so they are caught inside the plugin whatever it is written in.

Device voice caps, read when an instance is created: `OSCAIRN_VA_MAX_VOICES` (1 to 128, the
instance) and `OSCAIRN_VA_MAX_VOICES_PER_SOUND` (1 to 64, each sound), in engine voices (a unison
note counts as its 2 or 4; a cap under one note's voices still lets that note sound). Anything else is no cap.

Diagnostics: with `OSCAIRN_VA_TRACE=1` in the environment, the process writes one line to stderr when it builds
the oscillator tables (once, at the first instance): `oscairn-va: tables built in T ms`. Unset, nothing is read but the variable.

The CLAP headers are vendored in `third_party/clap` and the Oscairn SDK headers in
`third_party/oscairn-clap` (MIT; the commits are in each `upstream.json`).

This project is independent and is not affiliated with or endorsed by any instrument manufacturer.
It contains and distributes no manufacturer samples, presets, firmware or code. Product names, where
they appear at all, belong to their owners.

Release: `tools/release/` (see `release.conf`); a tag `oscairn-vX.Y.Z` triggers `.github/workflows/release.yml`.
