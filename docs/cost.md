# Cost

## Method

`tools/cost/cg.sh` builds `tools/vcbench.cpp` with the core at `-O3 -mcpu=cortex-a53` (g++ 13.3,
aarch64 Linux, NEON) and runs each case under callgrind with one dump per measured 128-frame block
at 48 kHz (the first measured block dropped). Cases, all on the "probe A" patch: rest (no voice), 1, 4,
8 and 16 held voices, the pattern (keys 84 88 91 88, sixteenths at 84 BPM, gate 0.9; the
engine keeps 4.1 voices on average), shedding 8 or 16 held voices in one block, and the plan
variants in `tools/cost/plans/` (8 held voices each).

The gate: at most 15k instructions a voice at 8 voices, at most 4k at rest. **Regression gate**:
`cg.sh SRC OUT check` fails (exit 1) when a case's mean is more than 10 % over
`tools/cost/baseline.txt`. It needs valgrind, so it is a documented script, run in the aarch64
container (`docker run --rm -v "$PWD":/src:ro -v /tmp/cg:/out IMAGE sh /src/tools/cost/cg.sh /src /out check`),
not a CTest test. Shown failing with a baseline lowered to 50000 for 8 voices (exit 1, "REGRESSION held8").

## Hosted cost (the plugin through CLAP)

`tools/hostbench.cpp` drives the built plugin as the host does: a sound-context snapshot of probe A loaded, 128-frame blocks at
48 kHz, `active_voices` and `notes` after every block; `cg.sh` measures it as the `hosted-*` cases (per block: `process`,
`active_voices` and `notes`). **Gate** (in `cg.sh check`): hosted at rest at most 1,000 instructions a block; hosted at 8 voices
at most 3 % over the core alone (`held8`). Shown failing with the shell of commit bf4a13f (hosted rest 2,611: "SHELL GATE").

| case | round 6, before | round 6 |
|---|---|---|
| **hosted rest cost** | 2,615 (core 217) | **780** (core 213) |
| **hosted cost at 8 voices** | 100,616 (core 96,879: +3.9 %) | **96,987** (core 94,633: +2.5 %) |
| hosted pattern | 62,916 (core 59,793) | 60,193 (core 58,683) |

What was trimmed: the shell's loops over the 64-voice pool every block (rendering, the helper's choke countdown, the per-block
limit count, the voice description, `notes`) visit only the active voices (a 64-bit mask kept at start and finish), so the
core's `voice()` is asked only for those; the core lists the sounds with voices once a render call instead of testing all 8 at
every control step, reaps only a sound with a voice releasing or fading, and advances the ages once a call.

## Code layout (a build-time choice)

`VA_KERNEL_LAYOUT` (CMake; `VC_LAYOUT` in `core/core.cpp`, `VA_FLAGS=-DVC_LAYOUT=n` for `cg.sh`): **0** (the default) every
kernel calls the shared control step and filters; **1** the hot kernels (saw-table oscillators 1-2, an optional oscillator 3, a
low-pass filter 1: the plans of 43 of the 45 analog parts surveyed, by `describe()`) inline the control step and the filters'
control, and run filter 1's LPF24 and filter 2's HPF24 inline (other types call the shared filter); **2** every kernel inlines
everything (round 5's layout before the code size change). The output is bit-identical in the three (the bench's hash on aarch64;
a render compared byte for byte on macOS arm64). Round 6, aarch64, per voice at 8 held voices, and `core.cpp` text:

| plan (8 held) | layout 0 | layout 1 | layout 2 |
|---|---|---|---|
| probe A (saw + pulse 0.5, LPF24, HPF24) | 11.8k | **10.4k** | 10.4k |
| probe A, filter 2 left out (plan 14) | 9.1k | 8.2k | 8.2k |
| saw + LPF24 (plan 3, with filter 2's HPF24) | 10.0k | 8.6k | 8.7k |
| a third oscillator (plan 10) | 13.9k | 12.5k | 12.5k |
| saw + LPF18 (plan 12) | 6.6k | 6.2k | 6.0k |
| not hot: saw + pulse (plan 2), pulse width LFO (plan 11) | 6.3k, 13.8k | 6.3k, 13.8k | 5.9k, 12.3k |
| hosted, 8 held (probe A) | 96,875 a block | 85,203 | 85,243 |
| `core.cpp` text (g++ 13.3, -O3, cortex-a53) | **250 KB** | **381 KB** | 1.86 MB |

Where the 12.1k a voice of 0.6.0 went against the 6.9k of the core timed at commit 2549b96: about 3.2k is round 5's filter work
(filter 2 built: its HPF24 2.2k in probe A; the low-pass's two sections, each with its poles mapped to 48 kHz at every control
step, 1.0k more than the TPT pair), and 1.45k is the out-of-line structure (layout 0 against 2: calls per lane and step, the
ramps' state stored and reloaded across the calls, the filter type's switch). Layout 1 recovers all of the latter for the hot plans
at 381 KB, but not the 8.5k a voice asked for: probe A stays at 10.4k because of its filters. The default stays layout 0 until
the device timing decides, and the device's hosted timing of 0.6.0 (layout 0) supports keeping it: there the maximum block rose
about 25 % against 0.4.0 where the instructions rose 1.7x (the four cores share one 512 KB L2, so text and table bytes weigh
alongside instructions).

Bytes touched a block (callgrind with per-instruction counts, aarch64, the distinct 64-byte lines of `core.cpp` code executed in one
128-frame block; tables from the table layout, not measured):

| case | layout 0: instructions / code touched | layout 1: instructions / code touched |
|---|---|---|
| probe A, 8 held | 95,130 / 15.7 KB | 83,426 / 14.5 KB |
| the pattern (4.1 voices) | 59,188 / 14.6 KB | 54,541 / 15.6 KB |

The code one plan touches is about the same in both layouts; layout 1's cost is that each hot plan carries its own copy (381 KB
of text against 250 KB), so several different sounds playing at once touch about 12 KB more code each, where layout 0's are shared.
Tables a voice reads: two adjacent levels of each oscillator's set (16 KB an oscillator; probe A's 8 voices on keys 84-91 about
33-66 KB) and about 1.7 KB of filter laws; the same in both layouts. Layout 1 does not win on both counts, so layout 0 stays the
default. One lane at a time is now cheaper than two interleaved in layout 0 (88,569 against 94,633 a block;
layout 2: 81,913 against 82,889): the interleaved filter loop of two lanes no longer pays in instructions.

## Instructions per block (aarch64, NEON, two lanes interleaved)

**Round 7** (the wave and portamento time destinations, the saturators, the filter EG's decay law under 144): rest 211, 1 held 51,245,
4 held 51,269, 8 held 96,493 (**12.1k a voice**), 16 held 180,830, 8 held one lane at a time 90,429, the pattern 60,504 (max 98,570);
hosted rest 776, hosted 8 held 98,843 (+2.4 % over the core), hosted pattern 62,010; plans per voice at 8: saw 4,694, saw + pulse 6,489,
saw + LPF24 10,272, probe A without filter 2 9,299, saw + LPF18 6,867, three LFOs 12,576, 16 boxes 13,262, a third oscillator **14,173**, a
pulse width LFO **14,032**, a third oscillator with all three saturators (plan 15) **14,409** (gate 15k); layout 1, 8 held 84,901. Probe A's
output is bit-identical to round 6's (the bench's hash on aarch64, 8 held and the pattern: 449f7161bb45b815, af2fb0e65523d1ac). Every
case with a voice rose 1.3-1.4k a block whatever the voice count: the three wave destinations and the portamento time in the boxes' per-sound step
(a first version rounded the wave inside the destinations' loop, which stopped its vectorisation: 5.3k a block; the wave is now rounded
in a loop of its own), and the oscillators' table sets copied into the step's constants. The saturators cost 228 a voice where they act
(plan 15 against plan 10), counted by `vcore::estimateCost`. `core.cpp` text 256,788 bytes (+6,584; layout 1: 390,020, +7,848). Data
added: `LawTables::portaRate` (128 floats, 512 bytes, read once per sound a step only when a box moves the portamento time), the
saturators' laws (180 bytes, read at compile), the filter EG's decay knots under 144 (72 bytes, at compile), one voice field (the
saturators' x^2 term: 256 bytes a sound's pool, 2 KB in all) and four destinations in the per-sound arrays (192 bytes). Bytes touched a
block were not measured this round. The regression baseline takes these figures.

**Round 6, final** (with the round's destinations: the frequency offsets and the boxes' new outputs at each control step):
rest 213, 1 held 49,906, 4 held 49,930, 8 held 95,130 (**11.9k a voice**), 16 held 179,427, 8 held one lane at a time 89,066,
the pattern 59,188 (max 97,232); hosted rest 779, hosted 8 held 97,481 (+2.5 %), hosted pattern 60,695; plans per voice at 8: saw
4,529, saw + pulse 6,325, saw + LPF24 10,103, probe A without filter 2 9,132, saw + LPF18 6,698, three LFOs 12,405, 16 boxes 13,098,
a third oscillator **14,005**, a pulse width LFO **13,859** (gate 15k); `core.cpp` text 250,204 bytes (layout 1: 382,172; 8 held
83,426). The baseline takes these figures. Table bytes: the round adds `LawTables::lfoHz` (416 floats, 1.7 KB, read once per sound
a step only when a box moves the pitch LFO's speed).

**Round 6** (the trims above): rest 213, 1 held 49,401, 4 held 49,425, 8 held 94,633 (**11.8k a voice**), 16 held 178,946, 8
held one lane at a time 88,569, the pattern 58,683 (max 96,735); shedding 8 / 16: 100,097 / 185,561 then 30,993 / 58,575, then
rest. The regression baseline is reset to these figures.

**Round 5, code size.** The device's time per block is mostly not instructions (cache misses on the tables and the code when
other plugins run between blocks), so the kernels were made smaller: each plan's control step is one call a lane (shared by its
one- and two-lane variants) and the filters' sample loops and control step are shared by every plan, called once a control step.
`core.cpp` text at `-O3 -mcpu=cortex-a53` (g++ 13.3, aarch64): 870 KB at 0.5.0, 2.14 MB with the round's filters inlined into
every kernel, **248 KB** now. The instructions rose: rest 217, 1 held 50,647, 4 held 51,079, 8 held 96,831 (**12.1k a voice**, gate
15k), 16 held 182,232, 8 held one lane at a time 90,767 (two lanes interleaved now cost more than one: the shared control step
interleaves nothing), the pattern 59,745 (max 97,824). Plans (per voice at 8): saw 4,734; saw + pulse 6,530; saw + LPF24 (with
filter 2's HPF24) 10,316; probe A 12,104; probe A with filter 2 left out 9,343; saw + LPF18 6,909; three LFOs 12,618; 16 boxes
13,311; a third oscillator **14,210**; a pulse width LFO **14,072** (both under the 15k gate, with 0.8-0.9k to spare). The regression
baseline is reset to these figures; `vcore::estimateCost`: base 3,010, a two-section filter 2,810, any other type 2,175. Debug
builds with the address sanitizer: the control step 107 KB of stack, a plan's run 85 KB (365 KB at 0.5.0, which a
512 KB thread stack only just held).

**Round 5** (the reference's filter structure, filter 2 built, key follow and velocity; the figures before the code size change): rest 216, 1 held 47,841, 4 held 48,273,
8 held 85,745 (**10.7k a voice**, gate 15k), 16 held 159,538, 8 held one lane at a time 84,537, the pattern 55,387 (max 86,738);
shedding 8 / 16: 90,161 / 164,209 then 29,367 / 54,143, then rest. Plans (8 held voices, per voice): saw 4,506; saw + pulse 6,186;
saw + LPF24 8,918 (its box on the filter EG depth reaches filter 2 too, so filter 2's HPF24 runs); probe A 10,718; probe A with
filter 2 left out (`14-*`: its HPF24 at cutoff 0 then only a gain) 8,517; saw + LPF18 6,211; saw + LPF24 + a moving HPF24 8,918;
three LFOs 11,186; 16 boxes 11,879; a third oscillator 12,827; a pulse width LFO 12,568. Every plan class within the 15k gate.
The filters cost more than the TPT pair they replace: a two-section filter 2.3k a voice (was 1.3k), filter 2's HPF24 2.2k, LPF18
1.7k, at 48 kHz, where each section's F and q are recomputed at control rate so that its poles are the reference's at 44.1 kHz
(the mapping is skipped at 44.1 kHz; not measured there). Probe A rose by 3.2k a voice: 1.0k its low-pass, 2.2k its filter 2,
which round 4 left out as transparent (the reference's HPF24 at 0 is -0.65 dB and a box on the filter EG depth moves it). The
regression baseline (`tools/cost/baseline.txt`) is reset to these figures. `vcore::estimateCost` counts 2,283 for a two-section
filter and 1,705 for any other type (LPF12, LPF6 and the one-section high-passes not measured).

**Round 4** (commit bb74f6a: the part's filter offsets, the box destinations as parameters, portamento, mono, the new waves,
the third oscillator, the pulse width LFO): rest 215, 1 held 35,916, 4 held 36,348, 8 held 60,228 (7.5k a voice, gate 15k),
16 held 108,005, 8 held one lane at a time 60,228, the pattern 40,888 (max 61,064); shedding 8 / 16: 64,645 / 112,677 then
22,580 / 40,292, then rest. `cg.sh check` against the baseline: every case within it (held8 +1.6 %, the pattern +2.8 %).
Plans (8 held voices, per voice): saw 4,576; saw + pulse 6,256; saw + LPF24 5,843; probe A 7,529; + HPF24 8,559; + the pitch LFO
7,709; + three LFOs 7,937; + a random LFO 7,718; three LFOs and 16 boxes 8,606; **a third oscillator (a sine) 9,752**; **a pulse
width moved by its oscillator's LFO 9,615**. Gates for the new plan classes: three oscillators with two filters and LFOs at most
15k a voice (9.8k); a pulse width LFO the same (9.6k). `vcore::estimateCost` adds 2,225 for a third oscillator and 416 for an
oscillator LFO (with its pulse's second read). The probe A plan rose by about 117 a voice since round 3 (not broken down by cause).

Round 3, at the controller boxes (the earlier figures in brackets):

| case | mean | max | per voice |
|---|---|---|---|
| rest | 216 | 216 | |
| 1 held | 34,765 | 34,765 | (one lane of 4 costs as full) |
| 4 held | 35,221 | 35,221 | 8.8k |
| 8 held | 59,293 | 59,311 | **7.4k** (gate 15k) [6.9k] |
| 16 held | 107,214 | 107,231 | 6.7k |
| 8 held, one lane at a time | 59,069 | 59,087 | 7.4k |
| pattern | 39,776 | 59,544 | |

Plans (8 held voices; per voice): saw 4,452; saw + pulse 6,130; saw + LPF24 5,740; saw + pulse + LPF24 (probe A, two
knob boxes) 7,412; the same + HPF24 8,558; probe A + the pitch LFO 7,546; + the pitch, filter and amplitude LFOs 7,796; + a
random pitch LFO at speed 320 7,551; the three LFOs and 16 boxes (every live source, eleven destinations, a measured curve
on five) 8,449. Gates per plan class: two oscillators, two filters, three LFOs and 16 boxes at most 15k a voice (8.4k).
Every plan rose by about 3.7k a block (470 a voice at 8): the envelope coefficients now come from a series in double (eight
a control step, once per sound: the polynomial they replaced was 2e-4 off) and the boxes' destinations are looked up once
per sound a step. The boxes cost about 47 a voice each at 8 voices (they run per sound, not per voice: fewer voices, a larger
share). The filter sections compute their normalisers every sample (two divisions a sample a section pair). The oscillators
read band-limited tables (one read a saw, one a pulse of width 0.5 from its own tables, two any other width); the drift's
knots are drawn per voice at control rate. `vcore::estimateCost` is built from these differences (base 2,780, a table
read 1,670, LPF24 1,295, HPF24 1,155, an LFO 130, a box 47; a pulse is counted as two reads, which over-estimates a pulse of
width 0.5). The shell reports a sound's estimate through `oscairn.voice-budget` before any note sounds, at 1.15 ns an
instruction (device-calibrated for this code at commit 2549b96; not re-timed for this round's code) and scaled to the host's block.
The oscillator tables (4 x 41 levels x 2049 floats and one sine level, 1.35 MB, built once per process) are a memory-access cost callgrind does
not count.

## Shedding voices (the project's overload rule)

A voice given up (`choke`) fades over the shell's 3 ms (144 frames) and its lane is freed when the
fade ends. Measured (round 3), the blocks from the shed block on: 8 voices 63,706 / 18,339 / 216 / 216; 16
voices 111,882 / 32,535 / 216. The shed block costs the held block plus 4.4k (8 voices) and the next
block about 2/7 of it (the last 16 frames of the fade): about 2.6k extra a shed voice, less than
the 6.9k one block of that voice costs. The fade starts in the block the limit drops.

## Time on this Mac (Apple silicon, thread CPU per block, `vcbench time`)

| case | two lanes interleaved | one lane |
|---|---|---|
| rest | 0.19 us | 0.16 us |
| 1 held | 2.51 us | 2.59 us |
| 8 held | 3.88 us | 4.45 us |
| 16 held | 6.92 us | 8.37 us |
| pattern | 2.68 us | 2.77 us |

Two lanes interleaved are the default: bit-identical output (`tests/core`), equal instruction count,
3-17 % faster here, about 20 % on the device per the spike.

## Device

Static binaries for the device's time mode: `vcbench-time-neon` and `vcbench-time-scalar`, built
with `g++ -std=c++17 -O3 -DNDEBUG -mcpu=cortex-a53 -static` from `core/*.cpp tools/vcbench.cpp`
(`vcbench time [CPU]`). Timed on the device at commit 2549b96 (thread CPU per 128-frame block, a pinned idle
core, mean / median / p99): NEON 8 held 62.0 / 60.8 / 80.3 us, 16 held 125 / 121 / 173 us, the pattern
44.3 / 36.6 us (unpinned 39.4 / 36.3 / 65.4); scalar 8 held 149 us, 16 held 299 us. About 7.8 us a voice and 1.13 ns
an instruction: slower per instruction than the placeholder spike (42-53 us for 8 voices at 5.5k instructions a
voice), which had no tables: the band-limited tables cost time that callgrind does not count. 8 voices are 4.7 % of
a 1,333 us block.

## Table memory

Read by the voices (float, 41 levels of 2049 samples, a level 8.2 KB; two adjacent levels a voice at a time):

| plan | tables | bytes |
|---|---|---|
| saw | the saw set | 336 KB |
| pulse of width 0.5 | the square set | 336 KB |
| pulse of any other width | the saw set (two reads) | 336 KB |
| saw + pulse 0.5 (probe A) | both sets | 672 KB |
| triangle | the triangle set | 336 KB |
| saw 2 | the saw 2 set | 336 KB |
| sine (any oscillator) | one level | 8.2 KB |
| a third oscillator | its wave's set (probe A + a sine: 680 KB) | as its wave |
| the LFOs | none (computed) | 0 |
| controller boxes | the laws at every raw value (`LawTables`, built once per process) and the curves' knots, read once per sound a step | 17.9 KB + 0.5 KB |
| filters (round 5) | per voice a control step: the resonance's gain (1 KB) and a high-pass's gain by semitone (0.7 KB a type); the cutoff law (4 KB, part of `LawTables`) once per sound a step with boxes; per sound 1 KB of velocity tables | 1.7 KB read a voice |

A voice touches two adjacent levels of a set (16 KB) per oscillator; which ones moves with its pitch.
