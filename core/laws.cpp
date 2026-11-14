// SPDX-License-Identifier: AGPL-3.0-or-later
// The reference's measured laws, applied once per sound at load time (main thread: libm allowed
// here, never in the audio path). Sources and fit errors: docs/coverage.md.
#include <algorithm>
#include <cmath>

#include "vcore.h"

namespace vcore {
namespace {

struct Law { const float* x; const float* y; int n; int lo, hi; };
#include "laws.inc"

// Linear between knots, held at the ends.
float at(const Law& l, float x) {
    if (x <= l.x[0]) return l.y[0];
    for (int i = 1; i < l.n; ++i)
        if (x <= l.x[i]) return l.y[i - 1] + (l.y[i] - l.y[i - 1]) * (x - l.x[i - 1]) / (l.x[i] - l.x[i - 1]);
    return l.y[l.n - 1];
}

// Calibration: the output level of this core against the reference's at part volume 127 (the host applies the part's volume),
// one constant (dB), fitted on one held saw (key 60, out level 511, filters thru, amplitude level 511; measurement run va-round5
// V1, three takes within 0.01 dB). The earlier constant (-15.96) was fitted on recordings at part volume 100, 4.12 dB down.
constexpr double kCalibrationDb = -11.40;

// Release laws (measured on sine probes at release 160 with sustain 140-511, on probe A, and at
// release 145 on 78 notes of 0.005-1 s at sustains 140-511, decays 120-180 and attack 80): the law is
// chosen by the level at the note-off alone, also during the attack or the decay. From a level at or
// above the threshold the release follows its table and ends where a straight line in dB is 53 dB down;
// under it the release runs 0.912 times as fast and ends 87 dB down.
constexpr double kReleaseThreshold = 0.443;  // between 0.437 (low law) and 0.449 (high law) at the note-off
constexpr double kLowReleaseRate = 0.912, kEndHighDb = -53.0, kEndLowDb = -87.0;

// Filter EG depth: 0.5 semitone of fc a step from 256 (va-round5 F2: depth 300 at cutoff 540 sounds 22.05 semitones over the static
// cutoff; on the earlier cutoff scale 0.5025, depths 96-446 within 0.08-0.11 semitone RMS, conv-cutoff), 256 a true zero (sustain
// 0 and 511 sound the same there). The depth stops at +-192 steps
// (va-round3 R1 G_*: a box driving it past 448 sounds as the static 448 within 0.01 dB).
// The part's filter offsets (0-127, 64 none; va-round4 K3: a saw at key 60 through LPF24, the EG at full sustain and released at
// once). FEG Depth adds 3 depth steps a unit to the EG's depth (1.60 semitones a unit on the earlier cutoff scale, 56-72 within 0.04; it scales with the EG
// level: none at sustain 0, K0), as depth steps before the depth's stop; Filter Cutoff Frequency adds 16 raw cutoff steps a unit
// (60-72 at 636 and 72 at 450 land on the static ladder's Hz within 0.3 semitone; 96 opens the filter). The performance's own
// offsets add to the part's unit for unit (K3: 72 + 72 sounds as 80): the converter folds them into these.
constexpr double kPartFegStepsPerUnit = 3;  // 1.5 semitones a unit (round 5, the semitone scale of fc: K3's PartFEGDepth=72 within 0.05 dB RMS)
constexpr int kPartCutoffPerUnit = 16;
// Voltage drift (sines held 8 and 30 s, 4 voices a take; 16 short notes; noise through LPF24 at 636):
// pitch = mean + static (uniform, sd as given) + amplitude x r(t), r linear between uniform knots in [-1, 1]
// every 2.85 s (knot spacing fitted on drift 127: 2.8-2.9 s). Cents, at drift 0 / 32 / 64 / 80 / 100 / 127; linear between.
// 64: mean +0.85 (12 voices over 30 s +0.75, 16 note starts +1.03), static sd 1.0, amplitude 3.6 (within-voice
// sd 1.8 less the tracker's 0.6). Over 64 (va-round8 D: four sines held 30 s, three takes, 12 voices; 16 note starts): the voices'
// mean offset (the held and the note starts' means averaged), the movement's amplitude from the within-voice sd (less the tracker's 0.6,
// over the 0.43 sd of r about a 29 s mean), the static sd from the voices' means less the movement's share of them (0.178 amplitude).
// The cutoff: a static offset per voice, uniform within +-the half-width (semitones; va-round8 DC: a saw at key 36 through LPF24 at 600,
// each of 16 notes' cutoff fitted on harmonics 1-14 with the filter's own response, within 0.01 dB: sd 0.30 / 0.77 / 1.67 / 3.99 at
// 64 / 80 / 100 / 127, means within their standard errors of 0; 32: half of 64's, not measured).
constexpr float kDriftX[] = {0, 32, 64, 80, 100, 127}, kDriftMean[] = {0, 0.96f, 0.85f, 1.65f, 2.70f, 17.0f},
                kDriftStaticSd[] = {0, 0.3f, 1.0f, 2.23f, 4.99f, 7.12f}, kDriftAmp[] = {0, 1.8f, 3.6f, 6.26f, 10.7f, 26.8f},
                kDriftCut[] = {0, 0.256f, 0.513f, 1.341f, 2.884f, 6.902f};
constexpr double kDriftPeriod = 2.85;

// Oscillator band-limiting (saw and pulse alone, filters thru, drift 0, every key 36-108): quarter-octave
// levels of floor(2^(n/4)) harmonics; with u = 4 log2(F / f0), level floor(u) + 1 weighs u - floor(u)
// (F = 18530 Hz, 75 keys within 0.1 %). The harmonics also roll off with frequency, the same for saw and
// pulse (dB at the centre of each kHz, 0.5 .. 17.5 kHz; extended at the last slope).
constexpr double kTabF = 18530, kPi = 3.14159265358979323846;
constexpr float kRollDb[] = {0.01f, -0.03f, -0.07f, -0.14f, -0.23f, -0.33f, -0.44f, -0.56f, -0.69f, -0.83f, -0.97f, -1.09f, -1.21f, -1.32f, -1.44f, -1.54f, -1.63f, -1.69f};
double rolloffDb(double f) {
    const double x = f / 1000 - 0.5;
    if (x <= 0) return 0;
    const int i = int(x);
    if (i >= 17) return kRollDb[17] + (kRollDb[17] - kRollDb[16]) * (x - 17);
    return kRollDb[i] + (kRollDb[i + 1] - kRollDb[i]) * (x - i);
}

// Oscillator out level (probe A, one oscillator at a time, key 84; both oscillators, saw and pulse alike within
// 0.05 dB): dB re 511 at 511/420/330/250/160. Under 160 (not covered) the converter's table cubed, which
// fits the measured knots within 0.6 dB, joined at 160; under 80 linear to 0.
// va-round4 W1 (a sine on oscillators 1 and 3 alike, every wave alike): 300 / 164 / 90 -> -9.25 / -19.75 / -30.17 dB. Under 90
// (not covered) the converter's table cubed, joined at 90; under 80 linear to 0.
constexpr float kOscLevelDbX[] = {90, 160, 164, 250, 300, 330, 420, 511}, kOscLevelDbY[] = {-30.17f, -20.15f, -19.75f, -12.42f, -9.25f, -7.59f, -3.39f, 0};
float oscLevel(int level) {
    const Law db{kOscLevelDbX, kOscLevelDbY, 8, 90, 511};
    if (level >= 90) return float(std::pow(10.0, at(db, float(level)) / 20));
    const double join = std::pow(10.0, kOscLevelDbY[0] / 20) / std::pow(double(at(kOscLevel, 90)), 3);
    const int l = std::max(level, 80);
    return float(join * std::pow(double(at(kOscLevel, float(l))), 3) * (level < 80 ? level / 80.0 : 1));
}
// Pulse width (W1 PW_*: harmonic n of a square at width w is |sin(pi n w)| / n, fitted within 0.03 dB): w = 0.01 + 0.98 raw / 256
// (0-240 within 0.0005; 255 reads 0.99, here 0.986), 1 % to 99 %; the oscillator's LFO moves it by its depth - 128 raw steps at
// LFO value 1 (W1 PM_*: 8.02 / 32.05 / 64.06 at depths 136 / 160 / 192).
float pulseWidth(float raw) { const float w = raw >= 255.f ? 0.99f : 0.01f + 0.98f * raw / 256.f; return w < 0.01f ? 0.01f : w > 0.99f ? 0.99f : w; }

// LFOs (pitch, filter cutoff and amplitude share them; sine on oscillator 1 at key 60, pitch tracked; va-round2 L1/L2).
// Speed, raw -> Hz: linear up to 64 (0.0042709 Hz a step, 0 at 0), log-frequency between knots above (every 8 from 80
// to 320; at 16-step spacing the knots between were predicted within 4 %). Knots up to 320 from the pitch track (sine
// fits, 2-20 s), above from the FM sidebands (336-415).
constexpr float kLfoSpeedX[] = {0, 16, 32, 48, 64, 80, 88, 96, 104, 112, 120, 128, 136, 139, 144, 150, 152, 160, 168, 176, 184, 192, 200, 208, 216, 224, 225, 232, 240, 248, 256, 264, 272, 280, 288, 296, 304, 312, 320, 336, 352, 368, 384, 400, 415};
constexpr float kLfoSpeedY[] = {0, 0.068341f, 0.13669f, 0.20503f, 0.27337f, 0.34697f, 0.41532f, 0.48366f, 0.55199f, 0.62034f, 0.70446f, 0.84114f, 0.97782f, 1.0304f, 1.1461f, 1.2828f, 1.3247f, 1.6613f, 2.0398f, 2.3973f, 2.8178f, 3.5328f, 4.2898f, 5.0048f, 6.3086f, 7.9908f, 8.2432f, 9.6731f, 11.944f, 15.309f, 18.673f, 22.543f, 29.272f, 36.337f, 43.066f, 56.525f, 69.983f, 83.441f, 107.67f, 161.4f, 258.47f, 387.5f, 602.8f, 947.5f, 1356.6f};
constexpr int kLfoSpeedN = sizeof kLfoSpeedX / sizeof kLfoSpeedX[0];
double lfoHz(int raw) {
    if (raw <= 64) return at(Law{kLfoSpeedX, kLfoSpeedY, kLfoSpeedN, 0, 415}, float(raw));
    int i = 5; while (i < kLfoSpeedN - 1 && raw > kLfoSpeedX[i]) ++i;
    const double u = (raw - kLfoSpeedX[i - 1]) / double(kLfoSpeedX[i] - kLfoSpeedX[i - 1]);
    return std::exp((1 - u) * std::log(double(kLfoSpeedY[i - 1])) + u * std::log(double(kLfoSpeedY[i])));
}
// Oscillator pitch LFO depth: steps from 247 -> cents at the LFO's peak (symmetric, the sign inverts); the reference
// moves in 1/2048 octave (0.586 cent); linear between the measured steps (28-123: ten knots from va-round3 R1's box steps,
// C_mw and S_clamp; the line between 80 and 96 was 12.6 and 3.5 cents high at 86 and 92) (the 19 knots added in L2 were predicted within
// 0.1 % by the others, except at 168-184: 9-24 %, now measured).
constexpr float kLfoDepthX[] = {0, 1, 2, 3, 4, 6, 8, 12, 15, 16, 20, 24, 25, 28, 30, 31, 32, 35, 36, 38, 40, 48, 50, 52, 53, 58, 61, 64, 80, 86, 92, 96, 100, 104, 108, 116, 120, 123, 124, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200, 208, 216, 224, 232, 240, 247};
constexpr float kLfoDepthY[] = {0, 0.59f, 0.58f, 1.17f, 1.76f, 2.93f, 4.10f, 5.86f, 7.62f, 7.62f, 9.96f, 14.07f, 14.65f, 17.58f, 21.09f, 21.09f, 21.68f, 24.61f, 27.54f, 32.23f, 37.49f, 57.42f, 62.11f, 67.38f, 69.73f, 82.62f, 92.58f, 97.26f, 137.11f, 150.00f, 184.57f, 205.08f, 225.00f, 244.92f, 264.85f, 304.69f, 324.61f, 345.12f, 345.12f, 365.04f, 404.88f, 444.73f, 484.57f, 525.00f, 564.84f, 624.61f, 825.00f, 1024.80f, 1224.61f, 1424.99f, 1624.80f, 1824.60f, 2024.99f, 2224.82f, 2400.00f};
constexpr int kLfoDepthN = sizeof kLfoDepthX / sizeof kLfoDepthX[0];
// Filter cutoff LFO depth: 25.17 cents of cutoff a step from 256 at the LFO's peak (va-round8 F and round 7 A3: a saw at key 36 through
// LPF24 at 500, a square LFO; each static's swing of harmonics 3-6 matched to the engine's at a fractional depth through the same
// analysis: 24 depths from -128 to +128 steps (8-128 either way) within 0.83 %; the earlier 26.6, fitted through a filter model at
// 8-40 steps, read the swings 4-5 % large). Amplitude LFO depth: the gain in dB is -A (1 - value),
// the top at the level (within 0.01 dB), A by steps from 128 (dB, sine in dB within 0.01 dB; linear between knots); the
// amplitude LFO runs a quarter cycle ahead of the pitch LFO (sine, reset on: at the top at the note-on, 80-87 degrees).
constexpr double kLfoCutCentsPerStep = 25.17;
constexpr float kLfoAmpX[] = {0, 4, 8, 12, 16, 24, 32, 40, 48, 64, 80, 96, 104, 112, 120, 124, 127};
constexpr float kLfoAmpY[] = {0, 0.188f, 0.378f, 0.753f, 1.13f, 1.693f, 2.45f, 3.198f, 3.951f, 6.02f, 8.466f, 12.04f, 14.487f, 18.062f, 24.271f, 30.479f, 48.164f};
// Delay: 0.00563 d^1.5 s (8-96, within 0.5 %); the LFO runs from the note-on and the delay gates it. Fade-in: the level
// rises as 1 - (1 - u)^2 over 2^((x - 154.5) / 15.7) s (96-214; 128-214 within 1 %).
constexpr float kLfoPhaseDeg[] = {0, 30, 45, 60, 90, 120, 135, 150, 180, 210, 225, 240, 270, 300, 315, 330};
void lfoLaws(Lfo& o, int wave, int speed, int reset, int phase, int delay, int fade, double sr) {
    o.rate = float(std::min(lfoHz(speed) / sr, double(kLfoMaxRate)));
    o.wave = uint8_t(wave);
    o.reset = reset != 0;
    o.phase0 = kLfoPhaseDeg[phase & 15] / 360.f;
    o.delay = float(0.00563 * std::pow(double(delay), 1.5) * sr);
    o.fadeRate = fade ? float(1 / (std::exp2((fade - 154.5) / 15.7) * sr)) : 1e30f;
}

// Portamento (va-round4 P1: a sine glides 60 -> 72, the pitch by zero crossings): linear in semitones, rate by the time
// (semitones a second: a line through 20-80 % of the way, residual 0.02-0.04 semitone from 49, 0.1-0.2 at 8-34); 0 is no glide;
// log-rate between the knots. Time mode 1 (and 3) takes the octave's time whatever the interval (7 / 12 / 24 semitones in
// 0.172 / 0.186 / 0.175 s at 49); modes 2 and 3 start at most 12 semitones away (60 -> 84 from 72). Times 1-7 glide as 8 (va-round7
// A3: an octave at 72 -> 84 5-95 % of the way in 27-44 ms, time 8 in 35-42 ms by the same tracking).
constexpr float kPortaX[] = {8, 12, 18, 24, 34, 49, 64, 80, 96, 112, 127};
constexpr float kPortaRate[] = {332.9f, 267.6f, 219.8f, 172.5f, 123.0f, 68.16f, 36.52f, 17.76f, 7.516f, 2.415f, 0.1219f};
double portaRate(int t) {  // semitones a second
    if (t <= 0) return 1e9;
    int i = 1; while (i < 10 && t > kPortaX[i]) ++i;
    const double u = t <= kPortaX[0] ? 0 : (t - kPortaX[i - 1]) / double(kPortaX[i] - kPortaX[i - 1]);
    return std::exp((1 - u) * std::log(double(kPortaRate[i - 1])) + u * std::log(double(kPortaRate[i])));
}


// The saturators (va-round7 X1: a 65.4 Hz sine through LPF24 at 1023; h1 against drive 0, h2 against h1 with its sign): at the drives
// measured, y = g (x + a2 x^2), x the filters' output over a single sine's at oscillator level 511 (1.1663 in the engine's units); g and
// a2 hold from oscillator level 90 to 511 (within 0.1 dB and 5 %), h3 stays under -64 dB to drive 23. Filter 1's and filter 2's are one
// law (S2_* within 0.03 dB and 0.0004); a filter's acts only where the filter does (thru: none). Between the measured drives g is not
// monotonic (16: +1.30 dB, 23: +0.73), so only those are taken (coverage).
constexpr int kSatDrive[] = {0, 2, 4, 7, 8, 12, 15, 16, 23};
constexpr float kSatFiltDb[] = {0, 0.166f, 0.537f, 1.070f, 1.165f, 1.131f, 1.072f, 1.298f, 0.727f};
constexpr float kSatFiltA2[] = {0, 0.00188f, 0.00224f, 0.00291f, 0.00318f, 0.00448f, 0.00581f, 0.00633f, 0.01159f};
constexpr float kSatAmpDb[] = {0, 0.148f, 0.363f, 0.579f, 0.609f, 0.768f, 0.859f, 0.878f, 1.716f};
constexpr float kSatAmpA2[] = {0, 0.00124f, 0.00148f, 0.00191f, 0.00209f, 0.00295f, 0.00383f, 0.00417f, 0.00763f};
constexpr double kSatUnit = 1.1663;  // the engine's single sine at oscillator level 511, filter open

// Unison (va-round8 U: a sine at keys 60, 84 and 96, each copy's partial read in the left and right channels; va-round7 X1 for the values
// the parts use): mode 1 is two copies at -/+ the detune in cents (0-15: within 0.07 cent), each 3.01 dB down (1 / sqrt 2); mode 2 four at
// -2, -2/3, +2/3 and +2 times it (within 0.06 cent), each 6.02 dB down. The flat copies sit left, the sharp right: the near channel at the
// copy's level, the far one attenuated by the spread (dB at spread 0-15, read within 0.01 dB; mode 2's inner copies as mode 1's at 2/3 of the
// spread, its outer ones at twice it; past -60 dB, under the method's floor, silent). The same at every key; a glide moves the copies together.
constexpr float kUniFarDb[3][16] = {
    {0, 0, -0.38f, -0.38f, -0.75f, -1.50f, -1.88f, -2.63f, -3.39f, -4.51f, -5.64f, -6.77f, -8.66f, -10.16f, -12.42f, -14.68f},  // mode 1
    {0, 0, 0, -0.38f, -0.38f, -0.75f, -0.75f, -1.13f, -1.51f, -1.88f, -2.26f, -3.01f, -3.39f, -4.14f, -4.89f, -5.65f},           // mode 2 inner
    {0, -0.38f, -0.75f, -1.88f, -3.39f, -5.65f, -8.65f, -12.42f, -17.66f, -26.28f, -44.97f, -200, -200, -200, -200, -200}};    // mode 2 outer
void unisonLaws(Sound& s, const Patch& q) {
    const int u = std::min(2, int(q[Unison]));
    s.unison = uint8_t(1 << u);
    if (!u) return;
    const float g = u == 1 ? float(std::sqrt(0.5)) : 0.5f;
    static const float kMul[2][4] = {{-1, 1}, {-2, -2.f / 3, 2.f / 3, 2}};
    for (int k = 0; k < s.unison; ++k) {
        const float m = kMul[u - 1][k], far = g * float(std::pow(10.0, kUniFarDb[u == 1 ? 0 : std::fabs(m) > 1 ? 2 : 1][q[UnisonSpread]] / 20));
        s.uniDet[k] = m * float(q[UnisonDetune]) / 100;
        s.uniL[k] = m < 0 ? g : far; s.uniR[k] = m < 0 ? far : g;
    }
}

// ---- the filters ------------------------------------------------------------------------------------------------------------
// va-round5 F1: every type is a cascade of the reference's sections (core.cpp filterRun, filterControl), read within 0.001 dB on a
// 32.7 Hz saw's harmonics against thru. The cutoff law gives the reference's fc (Hz) at each raw value (F3: LPF12 every 8 raw
// steps, its F and q fitted within 0.001 dB); a section's undamped coefficient is Ft = 2 sin(pi fc / 44100), its damping
// q = 2^(1 - r / 32) at resonance r (r 64 / 128 / 192 / 255: 0.5 / 0.125 / 0.031 / 0.0079, within 1 %), F = Ft (1 - min(q, 1) Ft / 4)
// (within 0.4 % at 600 / 780 / 900 over every resonance), q at most 3 - 2F (864-1023); a one-pole's c = 1 - e^(-2 pi fc / 44100)
// (LPF6 and LPF18 at 540-960 within 0.3 %). The filter EG adds 0.5 semitone of fc a depth step (F2: depth 300 at 540 sounds at
// 22.0 semitones over the static cutoff).
#include "filters.inc"
double filterFc(int c) {  // log-frequency between the knots
    int i = 1; while (i < kFcN - 1 && c > kFcX[i]) ++i;
    const double u = std::max(0.0, std::min(1.0, (c - kFcX[i - 1]) / double(kFcX[i] - kFcX[i - 1])));
    return std::exp((1 - u) * std::log(double(kFcY[i - 1])) + u * std::log(double(kFcY[i])));
}
}  // namespace
int satIndex(int drive) { for (int i = 0; i < 9; ++i) if (kSatDrive[i] == drive) return i; return -1; }
double cutoffHz(int raw) { return filterFc(std::max(0, std::min(1023, raw))); }
namespace {
void velocityLaws(Sound& s, int f, int cut, int feg, int res) {
    for (int v = 0; v < 128; ++v) {
        const float vc = float(cut < 0 ? 128 - v : v), vf = float(feg < 0 ? 128 - v : v);
        const double c = std::abs(cut) * at(Law{kVelX, kVelCut, 5, 1, 127}, vc);
        s.velCut[f][v] = int16_t(std::max(-1000.0, std::min(1000.0, std::round(c * 8))));
        const double m = std::max(0.0, 1 + std::abs(feg) * double(at(Law{kVelFegX, kVelFeg, 3, 1, 127}, vf)));
        s.velFeg[f][v] = uint8_t(std::min(255.0, std::round(m * 128)));
        // resonance (F2: two velocities a sign): a positive sensitivity lowers it by up to half of it toward velocity 1, a negative one
        // toward velocity 127, linearly between (not measured between)
        const double rr = res > 0 ? -res / 2.0 * (127 - v) / 126.0 : res < 0 ? res / 2.0 * (v - 1) / 126.0 : 0;
        s.velRes[f][v] = int8_t(std::max(-127.0, std::min(0.0, std::round(rr))));
    }
}

double log2Coef(double tau, double sr) { return -1.0 / (tau * sr * std::log(2.0)); }  // per frame
double attackTau(int raw) { return raw >= 100 ? at(kAmpAttack, float(raw)) : at(kShortAttack, float(raw < 96 ? raw : 96)); }

void envelope(Env& e, double a, double d, double s, double r, double sr) {
    e.la = float(log2Coef(a, sr));
    e.ld = float(log2Coef(d, sr));
    e.lr = float(log2Coef(r, sr));
    e.lrLow = float(log2Coef(r / kLowReleaseRate, sr));
    e.sustain = float(s);
    e.endHigh = float(std::pow(10.0, kEndHighDb / 20));
    e.endLow = float(std::pow(10.0, kEndLowDb / 20));
    e.threshold = float(kReleaseThreshold);
}
}  // namespace

// Level n holds the saw (rising, -1..1) with its roll-off taken at the pitch where the level plays alone. The other waves against
// the saw (va-round4 W1, harmonics re the saw's at the same key and out level, keys 36-72): saw 2 is the saw's harmonics x 1.148
// (+1.2 dB) with the fundamental x 1.493 (+3.48 dB) (within 0.3 dB to the 9th harmonic, 0.8 dB at the 21st at key 72); the
// triangle (odd harmonics 1 / n^2, alternating) has its fundamental x 1.496 (+3.50; h3-h11 within 0.06 dB of 1 / n^2); the
// sine x 1.832 (+5.25 dB, 5.15-5.32 over the keys), nothing else (under -100 dB). All band-limited as the saw (the same
// harmonics at keys 84-108, saw 2 two more at 84).
static float gTab[4 * kLevels * (kTabN + 1) + kTabN + 1];  // saw, square, triangle, saw 2 levels, then the one sine level
static bool buildTables() {
    static double sn[kTabN];
    for (int k = 0; k < kTabN; ++k) sn[k] = std::sin(2 * kPi * k / kTabN);
    for (int n = 0; n < kLevels; ++n) {
        const int H = std::min(int(std::floor(std::exp2(n / 4.0) + 1e-9)), kTabN / 2 - 1);
        const double f0 = kTabF / std::exp2(n / 4.0);
        double c[kTabN / 2];
        for (int h = 1; h <= H; ++h) c[h] = -2 / (kPi * h) * std::pow(10.0, rolloffDb(h * f0) / 20);
        float* t = gTab + n * (kTabN + 1);
        for (int k = 0; k < kTabN; ++k) {
            double y = 0;
            for (int h = 1; h <= H; ++h) y += c[h] * sn[(h * k) & (kTabN - 1)];
            t[k] = float(y);
        }
        t[kTabN] = t[0];
        float* q = t + kLevels * (kTabN + 1);
        for (int k = 0; k <= kTabN; ++k) q[k] = t[k] - t[(k + kTabN / 2) & (kTabN - 1)];
        float* tri = t + 2 * kLevels * (kTabN + 1);
        float* s2 = t + 3 * kLevels * (kTabN + 1);
        for (int k = 0; k < kTabN; ++k) {
            double y = 0;
            for (int h = 1; h <= H; h += 2) y += c[1] * 1.496 * ((h >> 1) & 1 ? -1 : 1) / (double(h) * h) * std::pow(10.0, (rolloffDb(h * f0) - rolloffDb(f0)) / 20) * sn[(h * k) & (kTabN - 1)];
            tri[k] = float(y);
            s2[k] = float(1.148 * t[k] + (1.493 - 1.148) * c[1] * sn[k]);
        }
        tri[kTabN] = tri[0]; s2[kTabN] = s2[0];
    }
    float* sine = gTab + 4 * kLevels * (kTabN + 1);
    for (int k = 0; k <= kTabN; ++k) sine[k] = float(-2 / kPi * 1.832 * sn[k & (kTabN - 1)]);
    return true;
}
const float* bandTables() { static const bool built = buildTables(); (void)built; return gTab; }

// ---- laws at every raw value (control rate: the controller boxes move a destination's raw value) ------------------
namespace {
double pitchDepthSemi(int d) { return std::copysign(double(at(Law{kLfoDepthX, kLfoDepthY, kLfoDepthN, 0, 247}, float(std::abs(d)))), double(d)) / 100; }
// Filter EG decay and release share one law (va-round3 R2 FD_/FR_: equal within 1 % at 176, 208, 232 and 255): the
// generated tables to 208, then 220-255 fitted on the first 15-70 % of the segment (a saw through LPF24 read against a static
// depth bank, reference against reference; the method reads the engine's own law back within 1 % at 176-208). Under 144 the
// amplitude decay's table stands in (not covered). Past 70 % of the way the reference runs slower than one exponential
// (at 144: 0.13 of the way left at 0.3 s against 0.02): not modelled (docs/acceptance.md).
constexpr float kFegTimeX[] = {144, 148, 152, 156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204, 208, 220, 232, 244, 255};
constexpr float kFegTimeY[] = {0.09438556f, 0.1122494f, 0.1335085f, 0.1620045f, 0.1965986f, 0.2289998f, 0.2667505f, 0.3229769f, 0.3910709f,
                               0.4566108f, 0.5331451f, 0.6455899f, 0.7817613f, 0.9128375f, 1.065898f, 1.292156f, 1.566451f, 2.614f, 4.278f, 7.237f, 12.153f};
constexpr Law kFegTime{kFegTimeX, kFegTimeY, 21, 144, 255};
// Under 144 (va-round7 E1: a saw at key 84 through LPF24 at 650 read against a static depth bank at the same key, 2.5 ms frames, the
// first 15-70 % of the segment from its peak, two takes each within 3 %): 100 / 104 / 110 / 117 / 120 / 126 / 133 / 138 -> 14.6 / 16.7 /
// 23.6 / 29.4 / 34.0 / 46.1 / 59.4 / 73.6 ms; the knots are the amplitude decay's table scaled by the reference's reading over the engine's
// through the same analysis (which reads the engine's law within 1 %).
constexpr float kFegDecayLowX[] = {100, 104, 110, 117, 120, 126, 133, 138, 144};
constexpr float kFegDecayLowY[] = {0.01440f, 0.01669f, 0.02399f, 0.02991f, 0.03414f, 0.04691f, 0.05998f, 0.07421f, 0.09438556f};
double fegDecayTau(int raw) { return raw >= 144 ? at(kFegTime, float(raw)) : at(Law{kFegDecayLowX, kFegDecayLowY, 9, 100, 144}, float(raw)); }
// Amplitude release under 112 (R2 AR_*: a sine's level slope from -3 to -40 dB, scaled by 1.042 so that 112 reads the
// generated table's 24.3 ms): 80 / 96 / 104 -> 6.29 / 12.12 / 17.36 ms. Under 80 (va-round8 R2: a 2093 Hz sine's level by its analytic
// signal at sample resolution, a line from -3 to -40 dB under the hold, 12 notes at each of 0-64): straight lines in dB, the time
// constant varying from voice to voice (sd 0.4-0.65 ms at ageing 100); the means fitted by tau = 0.652 ms x e^(0.0262 raw) over 0-80,
// its values at 0 / 16 / 32 / 48 the knots, 64 the mean there (3.77 ms): the engine through the same analysis within 2.6 standard errors
// of each of the 12 means (the largest at 40 and 56: +20 % and +12 %).
constexpr float kAmpRelLowX[] = {0, 16, 32, 48, 64, 80, 96, 104, 112};
constexpr float kAmpRelLowY[] = {0.000652f, 0.000992f, 0.001509f, 0.002294f, 0.00377f, 0.00629f, 0.01212f, 0.01736f, 0.02432072f};
double ampReleaseTau(int raw) { return raw < 112 ? at(Law{kAmpRelLowX, kAmpRelLowY, 9, 0, 112}, float(raw)) : at(kAmpRelease, float(raw)); }
// The filter EG's release under 128 is the amplitude's (va-round8 R2 and R: a 2093 Hz sine through LPF24 at 500 with depth 352, its level
// read as the EG's level through a static depth bank; 55 / 60 / 64 / 70 / 80 (12 notes) / 100 / 128 (4) -> 2.77 / 3.27 / 3.90 / 4.85 /
// 6.72 / 13.98 / 47.4 ms against the amplitude's law 2.94 / 3.40 / 3.77 / 4.72 / 6.29 / 14.74 / 46.1; under 55 the filter's own response
// hides the EG, not resolved).
double fegReleaseTau(int raw) { return raw >= 144 ? at(kFegTime, float(raw)) : raw >= 128 ? at(kFegRelease, float(raw)) : ampReleaseTau(raw); }
}  // namespace

static LawTables gLaw;
static bool buildLaws() {
    for (int i = 0; i < 1024; ++i) gLaw.cutSemi[i] = float(12 * std::log2(filterFc(i)));
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < kHpN; ++i) gLaw.hpGain[k][i] = float(std::pow(10.0, at(Law{kHpTabX[k], kHpTabY[k], kHpTabN[k], 0, 0}, float(i)) / 20));
    for (int i = 0; i <= 494; ++i) gLaw.pitchSemi[i] = float(pitchDepthSemi(i - 247));
    for (int i = 0; i < 512; ++i) gLaw.oscLevel[i] = oscLevel(i);
    for (int i = 0; i <= 415; ++i) gLaw.lfoHz[i] = float(lfoHz(i));
    for (int i = 0; i < 128; ++i) gLaw.portaRate[i] = float(portaRate(i));
    for (int i = 0; i < 256; ++i) {
        gLaw.resGain[i] = float(std::pow(10.0, at(Law{kResGainX, kResGainY, kResGainN, 0, 255}, float(i)) / 20));
        gLaw.invTau[TAttack][i] = float(1 / attackTau(i));
        gLaw.invTau[TAmpDecay][i] = float(1 / at(kAmpDecay, float(i)));
        gLaw.invTau[TAmpRelease][i] = float(1 / ampReleaseTau(i));
        gLaw.invTau[TFegDecay][i] = float(1 / fegDecayTau(i));
        gLaw.invTau[TFegRelease][i] = float(1 / fegReleaseTau(i));
    }
    return true;
}
const LawTables& lawTables() { static const bool built = buildLaws(); (void)built; return gLaw; }

// Curves (the box's curve bank 0): 0 the linear one (Standard, parameter 5: c = s, 1024 at 1023); the others measured at
// 17 source values (va-round3 R1 V_*: the mod wheel at 0, 8, ..., 120, 127 through a box on the pitch LFO's depth, c read
// back to +-5 of 1024), linear between: kCurveKnots.
// (the mod wheel at 0, 8, ..., 120, 127 through the 7-bit map, va-round5 C1: 8 v up to 64, then 512 + (v - 64) 512 / 63)
const float kCurveS[17] = {0, 64, 128, 192, 256, 320, 384, 448, 512, 577.0159f, 642.0317f, 707.0476f, 772.0635f, 837.0794f, 902.0952f, 967.1111f, 1024};
const CurveDef kCurves[kCurveCount] = {
    {0, 5, 0, {}},
    {0, 6, 0, {0, 33, 71, 118, 156, 204, 261, 308, 365, 431, 497, 573, 649, 734, 829, 924, 1024}},
    {0, 9, 0, {0, 0, 14, 24, 33, 43, 52, 71, 81, 109, 137, 175, 223, 298, 412, 630, 1024}},
    {0, 10, 0, {0, 0, 0, 0, 0, 14, 14, 14, 24, 33, 43, 52, 71, 99, 156, 298, 1024}},
    {2, 10, 4, {0, 0, 0, 0, 0, 0, 0, 0, 33, 156, 280, 412, 526, 659, 791, 914, 1024}},
    {2, 0, 4, {0, 128, 251, 374, 507, 621, 753, 877, 1009, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024}},
    {2, 6, 1, {0, 0, 0, 0, 0, 0, 0, 0, 128, 384, 630, 895, 1024, 1024, 1024, 1024, 1024}},
};
int findCurve(int bank, int type, int p1, int p2) {
    if (bank) return -1;
    for (int i = 0; i < kCurveCount; ++i) if (kCurves[i].type == type && kCurves[i].p1 == p1 && (type == 0 || kCurves[i].p2 == p2)) return i;
    return -1;
}
float curveAt(int id, float s) {  // no libm: control rate
    if (id <= 0) return s >= 1023.f ? 1024.f : s;
    const float* y = kCurves[id].y;
    int i = 1; while (i < 16 && s > kCurveS[i]) ++i;
    const float u = (s - kCurveS[i - 1]) / (kCurveS[i] - kCurveS[i - 1]);
    return y[i - 1] + (y[i] - y[i - 1]) * (u < 0 ? 0 : u > 1 ? 1 : u);
}

// Each destination's span (raw steps at a full box) and raw range (va-round3 R1: the box law; the spans are the
// reference's, read back from the boxes' effect on the pitch LFO's depth (247), the cutoff (1023), the filter EG's depth (192)
// and decay (255)).
const DestDef kDestDefs[kDests] = {
    {247, 0, 494}, {247, 0, 494}, {511, 0, 511}, {511, 0, 511}, {1023, 0, 1023}, {192, -192, 192}, {255, 0, 255},
    {255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {255, 0, 255}, {247, 0, 494},
    {1023, 0, 1023}, {192, -192, 192}, {255, 0, 255},  // filter 2's cutoff, EG depth and resonance as filter 1's
    // va-round6 A1: the pulse width (255), the filter LFO's depth (192), the pitch LFO's speed (415), the oscillators' frequency
    // (3600 cents: 36 semitones at a full box)
    {255, 0, 255}, {255, 0, 255}, {192, 64, 448}, {415, 0, 415}, {3600, -3600, 3600}, {3600, -3600, 3600}, {3600, -3600, 3600},
    {127, 0, 127},  // va-round6 A1 / round 7 A2: the portamento time (127 at a full box; a glide follows it: Core::render)
    // va-round7 A2: the oscillators' wave (4 steps at a full box, rounded half away from zero: boxRaw)
    {4, 0, 4}, {4, 0, 4}, {4, 0, 4}};

// The destinations' raw values with the boxes at these source values (src: s 0..1023 per source; knobs 0..1).
void boxRaw(const Sound& s, const float* src, const float* knob, int16_t* raw, const int16_t* stored, const float* mod) {
    if (!stored) stored = s.stored;
    float out[16], total[kDests] = {};
    for (int b = 0; b < s.boxes; ++b) {
        const Sound::Box& x = s.box[b];
        const float v = x.src >= 8 && x.src <= 15 ? float(int(knob[x.src - 8] * 1023.f + 0.5f)) : src[x.src];
        const float o = (curveAt(x.curve, v) + x.off - 1024.f) * x.ratio * 0.125f;
        out[b] = o < -8192.f ? -8192.f : o > 8192.f ? 8192.f : o;
    }
    for (int t = 0; t < s.targets; ++t) total[s.target[t].dest] += out[s.target[t].box];
    if (mod) for (int d = 0; d < kDests; ++d) total[d] += mod[d];
    for (int d = 0; d < DWave1; ++d) {
        const float tt = total[d] < -8192.f ? -8192.f : total[d] > 8192.f ? 8192.f : total[d];
        int v = stored[d] + int(tt * float(kDestDefs[d].span) * (1.f / 8192));  // toward zero, as the reference
        raw[d] = int16_t(v < kDestDefs[d].lo ? kDestDefs[d].lo : v > kDestDefs[d].hi ? kDestDefs[d].hi : v);
    }
    for (int d = DWave1; d <= DWave3; ++d) {  // the wave rounded half away from zero (va-round7 A2: +-1024 of a box moves it a step)
        const float tt = total[d] < -8192.f ? -8192.f : total[d] > 8192.f ? 8192.f : total[d], x = tt * (4.f / 8192);
        const int v = stored[d] + int(x + (x < 0 ? -0.5f : 0.5f));
        raw[d] = int16_t(v < 0 ? 0 : v > 4 ? 4 : v);
    }
}

const float* waveTable(int wave, bool square) {
    const TabSet set = wave == 1 ? TSaw2 : wave == 2 ? (square ? TSquare : TSaw) : wave == 3 ? TTriangle : wave == 4 ? TSine : TSaw;
    return bandTables() + set * kLevels * (kTabN + 1);
}

void applyLaws(Sound& s, const Patch& q, double sr) {
    s.plan = describe(q);
    const int stride = Osc2OscillatorWave - Osc1OscillatorWave;
    for (int n = 0; n < 3; ++n) {
        auto P = [&](Param p) { return q[Param(p + n * stride)]; };
        const int level = P(Osc1OscillatorOutLevel), wave = P(Osc1OscillatorWave);
        s.osc[n] = oscLevel(level);
        s.pw[n] = pulseWidth(float(P(Osc1OscillatorPulseWidth)));
        s.tab[n] = waveTable(wave, s.plan.osc[n] == Wave::Saw);
        s.stride[n] = wave == 4 ? 0.f : float(kTabN + 1);
        if (n < 2) {
            s.pwRaw[n] = float(P(Osc1OscillatorPulseWidth)); s.pwDepth[n] = float(P(Osc1OscillatorPulseWidthLFODepth) - 128);
            lfoLaws(s.oscLfo[n], P(Osc1OscillatorLFOWave), P(Osc1OscillatorLFOSpeed), P(Osc1OscillatorLFOKeyOnReset), P(Osc1OscillatorLFOPhase),
                    P(Osc1OscillatorLFODelayTime), P(Osc1OscillatorLFOFadeInTime), sr);
        }
        const double d = P(Osc1OscillatorPitch) - 504;
        const double cents = std::copysign(double(at(kPitchCents, float(std::fabs(d)))), d);
        s.pitch[n] = float(std::log2(440.0 / sr) - 69.0 / 12 + (P(Osc1OscillatorOctave) - 3) + cents / 1200);
    }
    s.tabU = float(4 * std::log2(kTabF / sr));
    lfoLaws(s.lfo[0], q[PitchLFOWave], q[PitchLFOSpeed], q[PitchLFOKeyOnReset], q[PitchLFOPhase], q[PitchLFODelayTime], q[PitchLFOFadeInTime], sr);
    lfoLaws(s.lfo[1], q[FilterCutoffLFOWave], q[FilterCutoffLFOSpeed], q[FilterCutoffLFOKeyOnReset], q[FilterCutoffLFOPhase], q[FilterCutoffLFODelayTime], q[FilterCutoffLFOFadeInTime], sr);
    lfoLaws(s.lfo[2], q[AmplitudeLFOWave], q[AmplitudeLFOSpeed], q[AmplitudeLFOKeyOnReset], q[AmplitudeLFOPhase], q[AmplitudeLFODelayTime], q[AmplitudeLFOFadeInTime], sr);
    for (int n = 0; n < 3; ++n) {
        const int d = q[Param(Osc1OscillatorPitchLFODepth + n * stride)] - 247;
        s.lfoPitch[n] = float(std::copysign(double(at(Law{kLfoDepthX, kLfoDepthY, kLfoDepthN, 0, 247}, float(std::abs(d)))), double(d)) / 100);
    }
    s.lfoCut = float((q[Filter1FilterCutoffLFODepth] - 256) * kLfoCutCentsPerStep / 100);
    s.lfoCutPerStep = float(kLfoCutCentsPerStep / 100);
    {
        const int a = q[AmplitudeLevelLFODepth] - 128;
        s.lfoAmp = float(std::copysign(double(at(Law{kLfoAmpX, kLfoAmpY, 17, 0, 127}, float(std::abs(a)))) / 6.0206, double(a)));
        s.lfo[2].phase0 = s.lfo[2].phase0 + 0.25f - (s.lfo[2].phase0 >= 0.75f ? 1.f : 0.f);
    }
    unisonLaws(s, q);
    s.porta = q[PortamentoMaster] && q[PartPortamentoSwitch];
    s.portaFull = q[PartPortamentoMode] != 0;
    s.portaTimeMode = uint8_t(q[PartPortamentoTimeMode]);
    s.portaRate = float(portaRate(q[PartPortamentoTime]) / sr);
    s.mono = q[PartMonoPolyMode] == 0;
    s.single = q[PartKeyAssignMode] == 0;
    s.keyOffset = float(12 * (q[ZoneZoneOctaveShift] - 64) + (q[ZoneZoneTranspose] - 64) + (q[PartNoteShift] - 64));
    s.bendUp = q[PartReceivePitchBend] ? float(q[PartPitchBendRangeUpper] - 64) : 0.f;
    s.bendDown = q[PartReceivePitchBend] ? float(q[PartPitchBendRangeLower] - 64) : 0.f;
    {
        const float d = float(q[VoltageDrift]);
        const Law mean{kDriftX, kDriftMean, 6, 0, 127}, sd{kDriftX, kDriftStaticSd, 6, 0, 127}, amp{kDriftX, kDriftAmp, 6, 0, 127}, cut{kDriftX, kDriftCut, 6, 0, 127};
        s.driftMean = at(mean, d) / 100; s.driftStatic = at(sd, d) * std::sqrt(3.f) / 100; s.driftAmp = at(amp, d) / 100;
        s.driftRate = float(1 / (kDriftPeriod * sr));
        s.driftCutMean = 0; s.driftCutSpread = at(cut, d);
    }

    // Filters (va-round5 F1-F3; filterFt and the laws above): the type (the band-passes are not built: thru, reported), the cutoff in
    // raw steps (filter 1 with the part's offset), the EG's move in raw steps, the resonance, key follow.
    s.rateRatio = float(44100.0 / sr);
    s.cutOffset = int16_t(kPartCutoffPerUnit * (q[PartFilterCutoffFrequency] - 64));
    const int cutoff = std::max(0, std::min(1023, q[Filter1FilterCutoff] + s.cutOffset));
    s.fegOffSteps = float(kPartFegStepsPerUnit * (q[PartFEGDepth] - 64));
    const int fstride = Filter2FilterType - Filter1FilterType;
    for (int f = 0; f < 2; ++f) {
        auto P = [&](Param p) { return q[Param(p + f * fstride)]; };
        Sound::Filter& x = s.flt[f];
        const int type = P(Filter1FilterType);
        x.type = uint8_t(type < kFilterTypes ? type : 0);
        x.cut = float(12 * std::log2(filterFc(f ? P(Filter1FilterCutoff) : cutoff)));
        x.feg = float(kFegSemisPerStep * std::max(-192.0, std::min(192.0, P(Filter1FilterCutoffEGDepth) - 256 + (f ? 0.0 : double(s.fegOffSteps)))));
        x.res = float(P(Filter1FilterResonance));
        x.keyFollow = kKeyFollowSemis[std::min(5, P(Filter1FilterCutoffKeyFollow))];
        velocityLaws(s, f, P(Filter1FilterCutoffVelocitySensitivity) - 256, P(Filter1FilterCutoffEGDepthVelocitySensitivity) - 256,
                     P(Filter1FilterResonanceVelocitySensitivity) - 256);
    }

    // Envelopes: one-pole segments; sustain = level / 511, linear.
    envelope(s.amp, attackTau(q[AmplitudeEGAttackTime]), at(kAmpDecay, float(q[AmplitudeEGDecayTime])), q[AmplitudeEGSustainLevel] / 511.0,
             ampReleaseTau(q[AmplitudeEGReleaseTime]), sr);
    envelope(s.feg, attackTau(q[FilterCutoffEGAttackTime]), fegDecayTau(q[FilterCutoffEGDecayTime]), q[FilterCutoffEGSustainLevel] / 511.0,
             fegReleaseTau(q[FilterCutoffEGReleaseTime]), sr);
    s.invSrLn2 = float(1 / (sr * std::log(2.0)));

    // Controller boxes (boxRaw): every box switched on whose source, curve and destination the core has; an oscillator's
    // or filter 1's destination only where that unit's switch for the box is on. The knobs are live (Core::setKnob), the
    // other sources are the channel's controllers (Core::setController); what was not measured is reported (notCovered).
    (void)lawTables();
    {
        const int o[2] = {Osc1OscillatorPitchLFODepth, Osc2OscillatorPitchLFODepth}, l[2] = {Osc1OscillatorOutLevel, Osc2OscillatorOutLevel};
        s.stored[DPitchDepth1] = int16_t(q[Param(o[0])]); s.stored[DPitchDepth2] = int16_t(q[Param(o[1])]);
        s.stored[DOscLevel1] = int16_t(q[Param(l[0])]); s.stored[DOscLevel2] = int16_t(q[Param(l[1])]);
        s.stored[DCutoff] = int16_t(cutoff); s.stored[DFegDepth] = int16_t(q[Filter1FilterCutoffEGDepth] - 256);
        s.stored[DReso] = int16_t(q[Filter1FilterResonance]);
        s.stored[DCutoff2] = int16_t(q[Filter2FilterCutoff]); s.stored[DFegDepth2] = int16_t(q[Filter2FilterCutoffEGDepth] - 256);
        s.stored[DReso2] = int16_t(q[Filter2FilterResonance]);
        s.stored[DFegAttack] = int16_t(q[FilterCutoffEGAttackTime]); s.stored[DFegDecay] = int16_t(q[FilterCutoffEGDecayTime]);
        s.stored[DFegRelease] = int16_t(q[FilterCutoffEGReleaseTime]); s.stored[DAmpAttack] = int16_t(q[AmplitudeEGAttackTime]);
        s.stored[DAmpDecay] = int16_t(q[AmplitudeEGDecayTime]); s.stored[DAmpRelease] = int16_t(q[AmplitudeEGReleaseTime]);
        s.stored[DPitchDepth3] = int16_t(q[Osc3OscillatorPitchLFODepth]);
        s.stored[DPw1] = int16_t(q[Osc1OscillatorPulseWidth]); s.stored[DPw2] = int16_t(q[Osc2OscillatorPulseWidth]);
        s.stored[DLfoCutDepth] = int16_t(q[Filter1FilterCutoffLFODepth]); s.stored[DLfoSpeed] = int16_t(q[PitchLFOSpeed]);
        for (int n = 0; n < 3; ++n) s.stored[DWave1 + n] = int16_t(q[Param(Osc1OscillatorWave + n * stride)]);
        s.stored[DPorta] = int16_t(q[PartPortamentoTime]);
    }
    const int bs = Box2ControllerSetSwitch - Box1ControllerSetSwitch;
    for (int b = 0; b < 16; ++b) {
        auto B = [&](Param p) { return q[Param(p + b * bs)]; };
        if (!B(Box1ControllerSetSwitch)) continue;
        const int src = B(Box1ControllerSetSource), dest = B(Box1ControllerSetDestination);
        const int curve = findCurve(B(Box1ControllerSetCurveBank), B(Box1ControllerSetCurveType), B(Box1ControllerSetCurveParameter1), B(Box1ControllerSetCurveParameter2));
        if (src >= kSources) continue;
        const uint8_t before = s.targets;
        auto aim = [&](Dest d) { if (s.targets < 32) s.target[s.targets++] = {s.boxes, uint8_t(d)}; };
        auto oscOn = [&](int n) { return q[Param(Osc1OscillatorControllerSet1Switch + n * stride + b)] != 0; };
        const bool f1 = q[Param(Filter1FilterControllerBox1Switch + b)] != 0, f2 = q[Param(Filter2FilterControllerBox1Switch + b)] != 0;
        switch (dest) {
        case kDestPitchDepth: for (int n = 0; n < 2; ++n) if (oscOn(n)) aim(Dest(DPitchDepth1 + n)); if (oscOn(2)) aim(DPitchDepth3); break;
        case kDestOscLevel: for (int n = 0; n < 2; ++n) if (oscOn(n)) aim(Dest(DOscLevel1 + n)); break;
        case kDestCutoff: if (f1) aim(DCutoff); if (f2) aim(DCutoff2); break;
        case kDestFegDepth: if (f1) aim(DFegDepth); if (f2) aim(DFegDepth2); break;
        case kDestReso: if (f1) aim(DReso); if (f2) aim(DReso2); break;
        case kDestFegAttack: aim(DFegAttack); break;
        case kDestFegDecay: aim(DFegDecay); break;
        case kDestFegRelease: aim(DFegRelease); break;
        case kDestAmpAttack: aim(DAmpAttack); break;
        case kDestAmpDecay: aim(DAmpDecay); break;
        case kDestAmpRelease: aim(DAmpRelease); break;
        case kDestPw: for (int n = 0; n < 2; ++n) if (oscOn(n)) aim(Dest(DPw1 + n)); break;
        case kDestFreq: for (int n = 0; n < 3; ++n) if (oscOn(n)) aim(Dest(DFreq1 + n)); break;
        case kDestLfoCutDepth: if (f1) aim(DLfoCutDepth); break;  // filter 2 has no cutoff LFO here
        case kDestPitchLfoSpeed: aim(DLfoSpeed); break;
        case kDestWave: for (int n = 0; n < 3; ++n) if (oscOn(n)) aim(Dest(DWave1 + n)); break;
        case kDestPorta: if (s.porta) aim(DPorta); break;
        default: break;
        }
        if (s.targets == before) continue;
        const int r = B(Box1ControllerSetRatio) - 128;
        s.box[s.boxes++] = {uint8_t(src), uint8_t(curve < 0 ? 0 : curve), float(r == 127 ? 128 : r), B(Box1ControllerSetPolarity) ? 512.f : 1024.f};
    }
    for (int k = 0; k < 8; ++k) s.knob[k] = float(q[Param(PartAssignableKnob1Value + k)]) / 1023.f;

    // Level: amplitude level law (dB), filter out level 0.375 dB a step about 64, calibration.
    double db = at(kAmpLevel, float(q[AmplitudeLevel])) + (q[Filter1FilterOutLevel] - 64) * 0.375 + kCalibrationDb;
    if (highPassAsGain(q)) {
        const int k = q[Filter2FilterType] - 5;
        db += at(Law{kHpTabX[k], kHpTabY[k], kHpTabN[k], 0, 0}, float(12 * std::log2(filterFc(q[Filter2FilterCutoff]))));
    }
    // the saturators in series (filter 1, filter 2, then the amplitude's after the amplitude EG: va-round7 S2, its h2 falls with the EG's
    // level, the filters' does not), each on the one before's output: their gains multiply, their x^2 terms add (a later one's scaled by
    // the gains before it, the amplitude's also by the EG at the control step; the x^3 and x^4 cross terms, under -75 dB to drive 23,
    // are dropped)
    double g = 1, c = 0;
    for (int f = 0; f < 2; ++f) {
        const int i = q[f ? Filter2FilterType : Filter1FilterType] ? satIndex(q[f ? Filter2FilterSaturatorDrive : Filter1FilterSaturatorDrive]) : 0;
        if (i > 0) { c += kSatFiltA2[i] * g; g *= std::pow(10.0, kSatFiltDb[i] / 20); }
    }
    const int ia = satIndex(q[AmplitudeSaturatorDrive]);
    const double ca = ia > 0 ? kSatAmpA2[ia] * g : 0;
    if (ia > 0) g *= std::pow(10.0, kSatAmpDb[ia] / 20);
    s.sat = c != 0 || ca != 0; s.satC = float(c / kSatUnit); s.satCa = float(ca / kSatUnit);
    s.level = float(std::pow(10.0, db / 20) * g);
}

}  // namespace vcore
