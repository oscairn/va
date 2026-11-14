// SPDX-License-Identifier: AGPL-3.0-or-later
// The engine: voices of each sound in structure-of-arrays lanes of W, control steps every at most
// kSub frames with linear ramps between, and one kernel per plan (template instances of the stage
// functions below; a stage a plan lacks is not in its loops). No allocation, no locks, no libm.
#include <cstring>

#include "vcore.h"
#include "vsimd.h"

namespace vcore {

void applyLaws(Sound&, const Patch&, double sampleRate);  // laws.cpp
const float* bandTables();                                 // laws.cpp

namespace {

struct Ctl {  // one sound's constants for one control step of n frames
    int n; float rn;
    float ka, kd, kr, krl, fa, fd, fr, frl;  // envelope coefficients over n frames
    float piOverSr;
    float cut[2], feg[2], res[2];             // each filter's cutoff, EG move at level 1 (semitones) and resonance, with the boxes now
    float lfoPitch[3], osc[3];                // pitch LFO depth (semitones), oscillator out level (linear)
    float freq[3], pwRaw[2], lfoCut, lfoRate; // the boxes' frequency offsets (semitones), pulse widths (raw), filter LFO depth
                                              // (semitones), pitch LFO speed (cycles a frame)
    const float* tab[3]; float stride[3];     // each oscillator's table set and level stride (a box may change its wave)
    float portaRate;                          // the glide's rate (semitones a frame; time modes 0 and 2)
    alignas(16) float bend[16];               // semitones per channel
};

struct Lane {
    Pool* p; int o;
    VC_INLINE vf operator()(int k) const { return load(&p->f[k][o]); }
    VC_INLINE void set(int k, vf x) const { store(&p->f[k][o], x); }
    // start a linear ramp over the step toward t; a fresh voice starts at t
    VC_INLINE void ramp(int k, int dk, vf t, vm fresh, vf rn) const {
        vf s = sel(fresh, t, (*this)(k));
        set(k, s); set(dk, (t - s) * rn);
    }
};

// ---- envelopes ----------------------------------------------------------------------------------
// attack charges toward 1.5 and stops at 1; decay toward the sustain; release toward a
// target below zero chosen at note-off (field `end`), so the level reaches exactly zero and stays.
VC_INLINE void envStep(const Lane& l, int LV, int ST, int END, int LOW, float ka, float kd, float kr, float krl, float sus) {
    vf lvl = l(LV), st = l(ST);
    vm atk = st < splat(0.5f), dec = st < splat(1.5f), low = splat(0.5f) < l(LOW);
    vf t = sel(atk, splat(1.5f), sel(dec, splat(sus), splat(0.f) - l(END)));
    vf k = sel(atk, splat(ka), sel(dec, splat(kd), sel(low, splat(krl), splat(kr))));
    lvl = fma(lvl - t, k, t);
    const vf one = splat(1.f);
    const vf hit = and0(atk, and0(lvl >= one, one));  // 1 where an attack reached the top
    lvl = vmax(lvl - hit * (lvl - one), splat(0.f));
    st = st + hit;
    l.set(LV, lvl); l.set(ST, st);
}

// ---- oscillators: band-limited tables ---------------------------------------------------------
// One table per level n (0..kLevels-1) of floor(2^(n/4)) saw harmonics (at most kTabN/2 - 1), read with
// linear interpolation; two adjacent levels are crossfaded by pitch (bandTables(), laws.cpp: Sound::tabU).
VC_INLINE vf tabRead(const float* t, vf x, vf oa, vf ob, vf w) {
    const vf fl = vfloor(x), fr = x - fl;
    vf a0, a1, b0, b1;
    gather2(t, fl + oa, a0, a1); gather2(t, fl + ob, b0, b1);
    const vf ya = fma(fr, a1 - a0, a0), yb = fma(fr, b1 - b0, b0);
    return fma(w, ya - yb, yb);
}
VC_INLINE vf wrap(vf p) { return p - and0(p >= splat(1.f), splat(1.f)); }

template <Wave O>
VC_INLINE vf oscSample(const float* t, vf p, vf oa, vf ob, vf w, vf pw) {
    const vf n = splat(float(kTabN));
    if constexpr (O == Wave::Saw) return tabRead(t, p * n, oa, ob, w);
    vf q = p - pw; q = q + and0(q < splat(0.f), splat(1.f));
    return tabRead(t, p * n, oa, ob, w) - tabRead(t, q * n, oa, ob, w);  // zero mean, as the reference's (va-round4 W1: no DC at any width)
}

template <Wave O1, Wave O2, Wave O3>
VC_INLINE void oscillators(const Lane& l, const Ctl& c, int n, vf* buf) {
    const float *t1 = c.tab[0], *t2 = c.tab[1], *t3 = c.tab[2];
    vf p1 = l(PH1), i1 = l(INC1), di1 = l(DINC1), w1 = l(TW1), dw1 = l(DTW1);
    vf p2 = l(PH2), i2 = l(INC2), di2 = l(DINC2), w2 = l(TW2), dw2 = l(DTW2);
    vf p3 = l(PH3), i3 = l(INC3), di3 = l(DINC3), w3 = l(TW3), dw3 = l(DTW3);
    const vf oa1 = l(TA1), ob1 = l(TB1), oa2 = l(TA2), ob2 = l(TB2), oa3 = l(TA3), ob3 = l(TB3);
    const vf a1 = splat(c.osc[0]), a2 = splat(c.osc[1]), a3 = splat(c.osc[2]);
    vf pw1 = l(PW1), dpw1 = l(DPW1), pw2 = l(PW2), dpw2 = l(DPW2);
    for (int k = 0; k < n; ++k) {
        vf y = splat(0.f);
        if constexpr (O1 == Wave::Pulse) pw1 = pw1 + dpw1;
        if constexpr (O2 == Wave::Pulse) pw2 = pw2 + dpw2;
        if constexpr (O1 != Wave::Off) { i1 = i1 + di1; w1 = w1 + dw1; p1 = wrap(p1 + i1); y = a1 * oscSample<O1>(t1, p1, oa1, ob1, w1, pw1); }
        if constexpr (O2 != Wave::Off) { i2 = i2 + di2; w2 = w2 + dw2; p2 = wrap(p2 + i2); y = fma(a2, oscSample<O2>(t2, p2, oa2, ob2, w2, pw2), y); }
        if constexpr (O3 != Wave::Off) { i3 = i3 + di3; w3 = w3 + dw3; p3 = wrap(p3 + i3); y = fma(a3, oscSample<Wave::Saw>(t3, p3, oa3, ob3, w3, pw1), y); }
        buf[k] = y;
    }
    l.set(PH1, p1); l.set(INC1, i1); l.set(TW1, w1); l.set(PH2, p2); l.set(INC2, i2); l.set(TW2, w2);
    if constexpr (O3 != Wave::Off) { l.set(PH3, p3); l.set(INC3, i3); l.set(TW3, w3); }
    if constexpr (O1 == Wave::Pulse) l.set(PW1, pw1);
    if constexpr (O2 == Wave::Pulse) l.set(PW2, pw2);
}

// ---- filters: the reference's sections (va-round5 F1: every type's response is these, within 0.001 dB) -------------------
// A state-variable section with a unit delay in its loop (lp += F bp; hp = x - lp - q bp; bp += F hp), and a one-pole
// (y += c (x - y)). The types: LPF24 = section (q of the resonance) then section (q = 1), low-pass outputs; LPF18 = section then
// one-pole; LPF12 = section; LPF6 = one-pole; the high-passes the same with high-pass outputs. Per filter and voice: F, q of the
// first section, q of the second or the one-pole's c, ramped over the control step; N lanes interleaved.
#define VC_N(...) for (int m = 0; m < N; ++m) __VA_ARGS__
enum : int { SNone, SLow, SHigh, OLow, OHigh };  // a part of a filter: none, a section (low or high output), a one-pole (low or high)
struct FilterKind { int a, b; };
constexpr FilterKind kKinds[kFilterTypes] = {{SNone, SNone}, {SLow, SLow}, {SLow, OLow}, {SLow, SNone}, {OLow, SNone},
                                             {SHigh, SHigh}, {SHigh, OHigh}, {SHigh, SNone}, {OHigh, SNone}};
template <int N, int A, int B>
VC_INLINE void filterRun(const Lane* l, int S, int n, vf (*buf)[kSub]) {
    vf s1[N], s2[N], s3[N], s4[N], F[N], dF[N], qa[N], dqa[N], qb[N], dqb[N], Fb[N], dFb[N];
    VC_N({ s1[m] = l[m](S); s2[m] = l[m](S + 1); s3[m] = l[m](S + 2); s4[m] = l[m](S + 3);
           F[m] = l[m](S + 4); dF[m] = l[m](S + 5); qa[m] = l[m](S + 6); dqa[m] = l[m](S + 7); qb[m] = l[m](S + 8); dqb[m] = l[m](S + 9);
           Fb[m] = l[m](S + 10); dFb[m] = l[m](S + 11); });
    for (int k = 0; k < n; ++k) {
        VC_N({
            F[m] = F[m] + dF[m]; qa[m] = qa[m] + dqa[m]; qb[m] = qb[m] + dqb[m];
            if constexpr (B == SLow || B == SHigh) Fb[m] = Fb[m] + dFb[m];
            vf x = buf[m][k];
            if constexpr (A == SLow || A == SHigh) {
                s1[m] = fma(F[m], s2[m], s1[m]);
                const vf hp = x - s1[m] - qa[m] * s2[m];
                s2[m] = fma(F[m], hp, s2[m]);
                x = A == SLow ? s1[m] : hp;
            } else {  // a one-pole; its c rides in qa
                s1[m] = fma(qa[m], x - s1[m], s1[m]);
                x = A == OLow ? s1[m] : x - s1[m];
            }
            if constexpr (B == SLow || B == SHigh) {
                s3[m] = fma(Fb[m], s4[m], s3[m]);
                const vf hp = x - s3[m] - qb[m] * s4[m];
                s4[m] = fma(Fb[m], hp, s4[m]);
                x = B == SLow ? s3[m] : hp;
            } else if constexpr (B == OLow || B == OHigh) {
                s3[m] = fma(qb[m], x - s3[m], s3[m]);
                x = B == OLow ? s3[m] : x - s3[m];
            }
            buf[m][k] = x;
        });
    }
    VC_N({ l[m].set(S, s1[m]); l[m].set(S + 1, s2[m]); l[m].set(S + 2, s3[m]); l[m].set(S + 3, s4[m]);
           l[m].set(S + 4, F[m]); l[m].set(S + 6, qa[m]); l[m].set(S + 8, qb[m]); l[m].set(S + 10, Fb[m]); });
}
// Shared by every plan's kernel (not inlined into each: the code stays small; one call a control step); filterSwitch is its body,
// inlined into the kernels the layout makes hot (VC_LAYOUT below).
template <int N>
VC_INLINE void filterSwitch(int type, const Lane* l, int S, int n, vf (*buf)[kSub]) {
    switch (type) {  // once a control step: the loops have no branch
    case 1: filterRun<N, SLow, SLow>(l, S, n, buf); break;
    case 2: filterRun<N, SLow, OLow>(l, S, n, buf); break;
    case 3: filterRun<N, SLow, SNone>(l, S, n, buf); break;
    case 4: filterRun<N, OLow, SNone>(l, S, n, buf); break;
    case 5: filterRun<N, SHigh, SHigh>(l, S, n, buf); break;
    case 6: filterRun<N, SHigh, OHigh>(l, S, n, buf); break;
    case 7: filterRun<N, SHigh, SNone>(l, S, n, buf); break;
    case 8: filterRun<N, OHigh, SNone>(l, S, n, buf); break;
    default: break;
    }
}
template <int N>
VC_NOINLINE void filterStage(int type, const Lane* l, int S, int n, vf (*buf)[kSub]) { filterSwitch<N>(type, l, S, n, buf); }

// asin(x), 0 <= x <= 1: its series under 0.5 (relative 1e-4 there), pi/2 - acos above. Control rate.
VC_INLINE vf asinv(vf x) {
    const vf x2 = x * x;
    const vf s = x * fma(x2, fma(x2, fma(x2, fma(x2, splat(35.f / 1152), splat(15.f / 336)), splat(3.f / 40)), splat(1.f / 6)), splat(1.f));
    return sel(x < splat(0.5f), s, splat(1.5707963f) - acosv(x));
}
// -ln(1 - u) for 0 <= u < 1: its series under 0.25 (1e-7 relative), log2 above.
VC_INLINE vf nlog1m(vf u) {
    vf p = splat(1.f / 9);
    for (int k = 8; k >= 1; --k) p = fma(p, u, splat(1.f / float(k)));
    return sel(u < splat(0.25f), u * p, log2v(splat(1.f) - u) * splat(-0.69314718f));
}
// 1 - e^-x for x >= 0: its series under 0.25, exp2 above.
VC_INLINE vf omexp(vf x) {
    vf p = splat(1.f);
    for (int k = 7; k >= 2; --k) p = splat(1.f) - x * splat(1.f / float(k)) * p;
    return sel(x < splat(0.25f), x * p, splat(1.f) - exp2v(x * splat(-1.4426950f)));
}
// sinh(x) and sin(x) for small x >= 0 (series), asinh(x) (series under 0.5, log above)
VC_INLINE vf sinhv(vf x) { const vf x2 = x * x; return x * fma(x2, fma(x2, fma(x2, splat(1.f / 5040), splat(1.f / 120)), splat(1.f / 6)), splat(1.f)); }
VC_INLINE vf sinv(vf x) { return cosv(splat(1.5707963f) - x); }
VC_INLINE vf asinhv(vf x) {
    const vf x2 = x * x;
    const vf s = x * fma(x2, fma(x2, fma(x2, splat(-5.f / 112), splat(3.f / 40)), splat(-1.f / 6)), splat(1.f));
    return sel(x < splat(0.5f), s, log2v(x + vsqrt(x2 + splat(1.f))) * splat(0.69314718f));
}
// A section's F and q (at 44.1 kHz) -> the F and q at the prepared rate with the same poles in seconds (z' = z^a, a = 44100 / rate),
// written in small quantities so that low cutoffs keep their precision: r = sqrt(1 - qF) the poles' radius, 1 - r, and
// s2 = (F^2 - (1 - r)^2) / 4r = sin^2(theta / 2) of the pole angle (negative: real poles, -sinh^2).
VC_NOINLINE void matchPoles(vf& F, vf& q, float a) {
    const vf one = splat(1.f), A = splat(a);
    const vf u = vmin(q * F, splat(0.999f)), r = vsqrt(one - u), om = u / (one + r);
    const vf s2 = vmin((F * F - om * om) / (splat(4.f) * r), one);
    const vf sig = splat(0.5f) * nlog1m(u) * A;  // the poles' decay a sample at the new rate
    const vm cplx = splat(0.f) < s2;
    const vf h = A * sel(cplx, asinv(vsqrt(vmax(s2, splat(0.f)))), asinhv(vsqrt(vmax(splat(0.f) - s2, splat(0.f)))));
    const vf sh = sel(cplx, sinv(vmin(h, splat(1.5707963f))), sinhv(h)), s2n = sh * sh;
    const vf omn = omexp(sig), rn = one - omn;
    const vf F2 = vmax(fma(omn, omn, sel(cplx, splat(4.f), splat(-4.f)) * rn * s2n), splat(1e-12f));
    F = vsqrt(F2);
    q = omn * (one + rn) / F;
}
// The two table levels and their crossfade for log2(increment) lg: u = tabU - 4 lg (u = 4 log2(F / f0));
// level floor(u) + 1 weighs u - floor(u). The weight ramps over the step unless the pair changed.
VC_INLINE void level(const Lane& l, int A, int B, int Wt, int DW, vf lg, float tabU, float stride, vm fresh, vf rn) {
    const vf u = vmax(vmin(fma(lg, splat(-4.f), splat(tabU)), splat(float(kLevels - 1))), splat(0.f));
    const vf m = vmin(vfloor(u), splat(float(kLevels - 2))), w = vmin(u - m, splat(1.f));
    const vf ob = m * splat(stride), d = ob - l(B);
    const vm keep = d * d < splat(0.25f);
    const vf start = sel(fresh, w, sel(keep, l(Wt), w));
    l.set(A, ob + splat(stride)); l.set(B, ob); l.set(Wt, start); l.set(DW, (w - start) * rn);
}

// An envelope's coefficient over a control step, 2^x for x <= 0: exp2s's polynomial is 2e-4 off near x = 0, which made
// every envelope segment about 0.3 per second fast (long times far too short); a series in double for small |x|.
inline float stepCoef(float x) {
    if (x < -0.5f) return exp2s(x);
    const double y = double(x) * 0.69314718055994531;
    return float(1 + y * (1 + y / 2 * (1 + y / 3 * (1 + y / 4 * (1 + y / 5 * (1 + y / 6 * (1 + y / 7)))))));
}

// uniform in [-1, 1) from a voice's xorshift state
VC_INLINE float unitRand(uint32_t& r) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return float(r >> 8) * (2.f / 16777216.f) - 1.f; }

// ---- LFOs (control rate) ------------------------------------------------------------------------
// One LFO of a lane advanced over the step: its value at the step's end (-1..1); m: its delay and fade-in level.
VC_INLINE vf lfoStep(const Lane& l, const Lfo& o, float rate, int n, int LP, int LR, vf t, vf& m) {
    const vf one = splat(1.f);
    vf ph = l(LP) + splat(float(n) * rate);
    const vm wrapped = ph >= one;
    ph = ph - vfloor(ph);
    l.set(LP, ph);
    if (o.wave == 4) {  // a new uniform value each cycle
        alignas(16) float w[W]; store(w, and0(wrapped, one));
        for (int i = 0; i < W; ++i) if (w[i] != 0.f) l.p->f[LR][l.o + i] = unitRand(l.p->rng[l.o + i]);
    }
    vf v;
    const vf tri = one - splat(4.f) * vabs(ph + splat(0.25f) - vfloor(ph + splat(0.25f)) - splat(0.5f));
    switch (o.wave) {
    case 0: v = ph + ph - and0(ph >= splat(0.5f), splat(2.f)); break;
    case 1: v = sel(ph < splat(0.5f), one, splat(-1.f)); break;
    case 2: v = tri; break;
    case 3: { const vf x2 = tri * tri;  // sin(pi/2 x) on the triangle: odd polynomial, 2e-6
              v = tri * fma(x2, fma(x2, fma(x2, splat(-4.6817e-3f), splat(7.96926e-2f)), splat(-0.645964f)), splat(1.5707963f)); break; }
    default: v = l(LR); break;
    }
    const vf u = vmin(vmax((t - splat(o.delay)) * splat(o.fadeRate), splat(0.f)), one), r = one - u;
    m = one - r * r;
    return v;
}

// ---- a filter's control step ------------------------------------------------------------------------------------------
// The cutoff as the log of the reference's frequency fc (semitones re 1 Hz: the patch's by the cutoff law, with the boxes; the EG,
// key follow, velocity and the LFO add semitones), stopped at 22050 Hz; the reference's coefficients there (laws.cpp:
// Ft = 2 sin(pi fc / 44100), the damping q = 2^(1 - r / 32) of the resonance r, F = Ft (1 - min(q, 1) Ft / 4), q at most 3 - 2F,
// the one-pole's c = 1 - e^(-2 pi fc / 44100)), mapped to the prepared rate, ramped over the step. Returns the filter's output
// gain (the resonance's pass-band gain, the high-pass sections' gain), which the amplitude applies.
VC_INLINE vf filterControlBody(const Lane& l, const Sound& s, const Ctl& c, int f, vf eg, vf lfo, vm fresh, vf rn) {
    const int S = f ? H1 : L1, type = s.flt[f].type;
    const FilterKind k = kKinds[type];
    const LawTables& t = lawTables();
    const vf one = splat(1.f);
    const vf semi = vmin(fma(eg, splat(c.feg[f]) * l(f ? VFEG2 : VFEG), splat(c.cut[f])) + l(f ? DCUT2 : DCUT) + lfo, splat(kTopSemi));
    const vf x = exp2v(semi * splat(1.f / 12)) * splat(3.14159265f / 44100);  // pi fc / 44100, at most pi / 2
    const vf Ft = splat(2.f) * cosv(splat(1.5707963f) - x);
    const vf r = vmax(vmin(splat(c.res[f]) + l(f ? VRES2 : VRES), splat(255.f)), splat(0.f));
    vf q = exp2v(one - r * splat(1.f / 32));
    // F under 1.95 and q at least 0: the section stays stable (F^2 + 2qF < 4, qF < 2) at every cutoff and resonance
    vf F = vmin(Ft * (one - vmin(q, one) * Ft * splat(0.25f)), splat(1.95f));
    const vf qm = vmax(splat(3.f) - F - F, splat(0.f));  // the damping's ceiling at high cutoffs (F3: q = 3 - 2F from cutoff 864 up, within 0.01 %)
    q = vmin(q, qm);
    vf qb = vmin(one, qm), Fb = Ft * (one - Ft * splat(0.25f));  // the second section: its own F at its q of 1 (F1: LPF24 at 600-900)
    const float a = s.rateRatio;
    if (a != 1.f) {
        if (k.a == SLow || k.a == SHigh) matchPoles(F, q, a);
        if (k.b == SLow || k.b == SHigh) matchPoles(Fb, qb, a);
    }
    const vf w = x * splat(2.f * a);  // the one-pole's pole e^-w at the prepared rate
    if (k.a == OLow || k.a == OHigh) q = omexp(w);
    if (k.b == OLow || k.b == OHigh) qb = omexp(w);
    l.ramp(S + 4, S + 5, F, fresh, rn); l.ramp(S + 6, S + 7, q, fresh, rn); l.ramp(S + 8, S + 9, qb, fresh, rn); l.ramp(S + 10, S + 11, Fb, fresh, rn);
    vf g = one;
    if (k.a == SLow || k.a == SHigh) { const vf ri = vmin(vfloor(r), splat(254.f)); vf g0, g1; gather2(t.resGain, ri, g0, g1); g = fma(r - ri, g1 - g0, g0); }
    if (type >= 5) {  // the high-pass's gain by the log of fc (laws.cpp: a semitone apart from 1 Hz)
        const vf u = vmax(vmin(semi, splat(float(kHpN - 2))), splat(0.f)), ui = vfloor(u);
        vf h0, h1; gather2(t.hpGain[type - 5], ui, h0, h1);
        g = g * fma(u - ui, h1 - h0, h0);
    }
    return g;
}
VC_NOINLINE vf filterControl(const Lane& l, const Sound& s, const Ctl& c, int f, vf eg, vf lfo, vm fresh, vf rn) {
    return filterControlBody(l, s, c, f, eg, lfo, fresh, rn);
}

// ---- control step for one lane ------------------------------------------------------------------
// Hot: inlined into its kernel with the filters' control (the layout's hot kernels); else one call a lane a step, shared by the
// lane variants.
template <bool LP, bool HP, bool O3, bool Hot>
VC_INLINE void controlBody(const Lane& l, const Sound& s, const Ctl& c) {
    const vm fresh = splat(0.5f) < l(FRESH);
    const vf rn = splat(c.rn);
    envStep(l, AENV, ASTG, AEND, ALOW, c.ka, c.kd, c.kr, c.krl, s.amp.sustain);
    if constexpr (LP || HP) envStep(l, FENV, FSTG, FEND, FLOW, c.fa, c.fd, c.fr, c.frl, s.feg.sustain);
    // pitch: key (with the sound's offsets) + drift + the channel's bend
    alignas(16) float bend[W];
    for (int i = 0; i < W; ++i) bend[i] = c.bend[l.p->channel[l.o + i] & 15];
    vf semis = l(KEY) + load(bend) + l(UDET);  // a unison copy's detune (0 for a note of one voice)
    if (s.porta) {  // the glide's offset toward 0, linearly
        const vf rate = s.portaTimeMode & 1 ? l(GRATE) : splat(c.portaRate);  // a box on the time moves a glide under way (va-round7 A2)
        const vf g = l(GOFF), a = vmax(vabs(g) - rate * splat(float(c.n)), splat(0.f));
        const vf ng = sel(g < splat(0.f), splat(0.f) - a, a);
        l.set(GOFF, ng);
        semis = semis + ng;
    }
    vf lp1 = splat(0.f), lp2 = splat(0.f), lp3 = splat(0.f), lcut = splat(0.f), lamp = splat(0.f);
    if (s.plan.lfo[0] | s.plan.lfo[1] | s.plan.lfo[2] | s.plan.oscLfo[0] | s.plan.oscLfo[1]) {
        const vf t = l(LT) + splat(float(c.n));
        l.set(LT, t);
        vf m;
        if (s.plan.lfo[0]) {
            vf v = lfoStep(l, s.lfo[0], c.lfoRate, c.n, LP0, LR0, t, m);  // m is the call's output: read only after it (argument order is unspecified)
            v = v * m;
            lp1 = v * splat(c.lfoPitch[0]); lp2 = v * splat(c.lfoPitch[1]); lp3 = v * splat(c.lfoPitch[2]);
        }
        for (int o = 0; o < 2; ++o)  // the pulse width, moved by the oscillator's own LFO: raw + depth x value through the width law
            if (s.plan.oscLfo[o]) {
                vf v = lfoStep(l, s.oscLfo[o], s.oscLfo[o].rate, c.n, LO1 + o, LOR1 + o, t, m);
                v = v * m;
                const vf w = vmax(vmin(fma(fma(v, splat(s.pwDepth[o]), splat(c.pwRaw[o])), splat(0.98f / 256.f), splat(0.01f)), splat(0.99f)), splat(0.01f));
                l.ramp(PW1 + 2 * o, DPW1 + 2 * o, w, fresh, rn);
            }
        if (s.plan.lfo[1]) { const vf v = lfoStep(l, s.lfo[1], s.lfo[1].rate, c.n, LP1, LR1, t, m); lcut = v * m * splat(c.lfoCut); }
        // amplitude: dB = -depth (1 - value): the top is the level (measured); the delay and fade-in scale the depth (not measured)
        if (s.plan.lfo[2]) { const vf v = lfoStep(l, s.lfo[2], s.lfo[2].rate, c.n, LP2, LR2, t, m); lamp = (v * splat(s.lfoAmp) - splat(s.lfoAmp < 0 ? -s.lfoAmp : s.lfoAmp)) * m; }
    }
    // drift: advance each oscillator's knot phase; a new uniform knot when it wraps (per lane, control rate)
    for (int i = 0; i < W; ++i) {
        const int v = l.o + i;
        float* f[3][3] = {{&l.p->f[DA1][v], &l.p->f[DB1][v], &l.p->f[DP1][v]}, {&l.p->f[DA2][v], &l.p->f[DB2][v], &l.p->f[DP2][v]},
                          {&l.p->f[DA3][v], &l.p->f[DB3][v], &l.p->f[DP3][v]}};
        for (int o = 0; o < (O3 ? 3 : 2); ++o) {
            float* const* d = f[o];
            *d[2] += float(c.n) * s.driftRate;
            if (*d[2] >= 1.f) { *d[2] -= 1.f; *d[0] = *d[1]; *d[1] = unitRand(l.p->rng[v]); }
        }
    }
    const vf amp = splat(s.driftAmp);
    const vf dt1 = fma(amp, fma(l(DB1) - l(DA1), l(DP1), l(DA1)), l(DS1)), dt2 = fma(amp, fma(l(DB2) - l(DA2), l(DP2), l(DA2)), l(DS2));
    const vf lg1 = fma(semis + dt1 + lp1 + splat(c.freq[0]), splat(1.f / 12), splat(s.pitch[0]));
    const vf lg2 = fma(semis + dt2 + lp2 + splat(c.freq[1]), splat(1.f / 12), splat(s.pitch[1]));
    l.ramp(INC1, DINC1, vmin(exp2v(lg1), splat(0.45f)), fresh, rn);
    l.ramp(INC2, DINC2, vmin(exp2v(lg2), splat(0.45f)), fresh, rn);
    level(l, TA1, TB1, TW1, DTW1, lg1, s.tabU, c.stride[0], fresh, rn);
    level(l, TA2, TB2, TW2, DTW2, lg2, s.tabU, c.stride[1], fresh, rn);
    if constexpr (O3) {
        const vf dt3 = fma(amp, fma(l(DB3) - l(DA3), l(DP3), l(DA3)), l(DS3));
        const vf lg3 = fma(semis + dt3 + lp3 + splat(c.freq[2]), splat(1.f / 12), splat(s.pitch[2]));
        l.ramp(INC3, DINC3, vmin(exp2v(lg3), splat(0.45f)), fresh, rn);
        level(l, TA3, TB3, TW3, DTW3, lg3, s.tabU, c.stride[2], fresh, rn);
    }
    vf fg = splat(1.f);  // the filters' output gains
    if constexpr (LP) fg = Hot ? filterControlBody(l, s, c, 0, l(FENV), lcut, fresh, rn) : filterControl(l, s, c, 0, l(FENV), lcut, fresh, rn);
    if constexpr (HP) fg = fg * (Hot ? filterControlBody(l, s, c, 1, l(FENV), splat(0.f), fresh, rn) : filterControl(l, s, c, 1, l(FENV), splat(0.f), fresh, rn));
    // amplitude: EG x volume x fade x the sound's level and gain, equal-power pan
    vf fade = vmax(l(FADE) - l(DFADE) * splat(float(c.n)), splat(0.f));
    l.set(FADE, fade);
    vf g = l(AENV) * l(VOL) * fade * splat(s.level * s.gain) * fg;
    if (s.plan.lfo[2]) g = g * exp2v(lamp);
    l.ramp(GL, DGL, g * splat(s.panL) * l(UGL), vm{}, rn);  // a unison copy's level and place in the spread (1: one voice)
    l.ramp(GR, DGR, g * splat(s.panR) * l(UGR), vm{}, rn);
    if (s.sat) l.set(SATC, fma(splat(s.satCa), l(AENV), splat(s.satC)) * fg);  // the x^2 term on the buffer (the filters' gain and the EG come later)
    l.set(FRESH, splat(0.f));
}
template <bool LP, bool HP, bool O3>
VC_NOINLINE void control(const Lane& l, const Sound& s, const Ctl& c) { controlBody<LP, HP, O3, false>(l, s, c); }

// The saturators after the filters (laws.cpp applyLaws): y = x + c x^2 - m per sample, c per voice, m the x^2 term's running mean
// (va-round7 X1: the reference's output has no DC at any drive, under 1e-5 of the fundamental where c x^2 would give 6e-3).
VC_NOINLINE void saturate(const Lane& l, float kk, int n, vf* buf) {
    const vf c = l(SATC), k = splat(kk);
    vf m = l(SDC);
    for (int i = 0; i < n; ++i) { const vf q = c * buf[i] * buf[i]; m = fma(k, q - m, m); buf[i] = buf[i] + (q - m); }
    l.set(SDC, m);
}

VC_INLINE void amp(const Lane& l, const vf* buf, int n, vf* accL, vf* accR) {
    vf gl = l(GL), dgl = l(DGL), gr = l(GR), dgr = l(DGR);
    for (int k = 0; k < n; ++k) {
        gl = gl + dgl; gr = gr + dgr;
        accL[k] = fma(buf[k], gl, accL[k]); accR[k] = fma(buf[k], gr, accR[k]);
    }
    l.set(GL, gl); l.set(GR, gr);
}

}  // namespace

struct Kernel { void (*run)(Pool&, const Sound&, const Ctl&, vf*, vf*, int interleave); };

namespace {
// The code layout, a build-time choice (CMake VA_KERNEL_LAYOUT; docs/cost.md): 0 every kernel calls the shared control step and
// filters (the smallest code), 1 the hot kernels (saw oscillators 1-2, optional oscillator 3, low-pass filter 1: the plans of most
// of the parts measured) inline them, their filters' usual types (filter 1 LPF24, filter 2 HPF24) included, 2 every kernel
// inlines everything (the largest code).
#ifndef VC_LAYOUT
#define VC_LAYOUT 0
#endif
template <int N, int T>
VC_INLINE void filterHot(int type, const Lane* l, int S, int n, vf (*buf)[kSub]) {
    if constexpr (VC_LAYOUT == 2) filterSwitch<N>(type, l, S, n, buf);
    else if (type == T) filterRun<N, kKinds[T].a, kKinds[T].b>(l, S, n, buf);
    else filterStage<N>(type, l, S, n, buf);
}
template <Wave O1, Wave O2, Wave O3, bool LP, bool HP>
struct Plan {
    static constexpr bool kHot = VC_LAYOUT == 2 || (VC_LAYOUT == 1 && O1 == Wave::Saw && O2 != Wave::Pulse && LP);
    template <int N>
    static VC_INLINE void lanes(Pool& p, const Sound& s, const Ctl& c, int j, vf* aL, vf* aR) {
        Lane l[N]; vf buf[N][kSub];
        for (int m = 0; m < N; ++m) {
            l[m] = Lane{&p, (j + m) * W};
            if constexpr (kHot) controlBody<LP, HP, O3 != Wave::Off, true>(l[m], s, c);
            else control<LP, HP, O3 != Wave::Off>(l[m], s, c);
            oscillators<O1, O2, O3>(l[m], c, c.n, buf[m]);
        }
        if constexpr (LP) { if constexpr (kHot) filterHot<N, 1>(s.flt[0].type, l, L1, c.n, buf); else filterStage<N>(s.flt[0].type, l, L1, c.n, buf); }
        if constexpr (HP) { if constexpr (kHot) filterHot<N, 5>(s.flt[1].type, l, H1, c.n, buf); else filterStage<N>(s.flt[1].type, l, H1, c.n, buf); }
        if (s.sat) for (int m = 0; m < N; ++m) saturate(l[m], s.satK, c.n, buf[m]);
        for (int m = 0; m < N; ++m) amp(l[m], buf[m], c.n, aL, aR);
    }
    static void run(Pool& p, const Sound& s, const Ctl& c, vf* aL, vf* aR, int interleave) {
        const int lanes_ = int(p.n + W - 1) / W;
        int j = 0;
        if (interleave == 2) for (; j + 2 <= lanes_; j += 2) lanes<2>(p, s, c, j, aL, aR);
        for (; j < lanes_; ++j) lanes<1>(p, s, c, j, aL, aR);
    }
};

template <Wave O1, Wave O2, Wave O3, bool LP, bool HP> constexpr Kernel kK{&Plan<O1, O2, O3, LP, HP>::run};
#define VC_F(O1, O2, O3) &kK<O1, O2, O3, false, false>, &kK<O1, O2, O3, false, true>, &kK<O1, O2, O3, true, false>, &kK<O1, O2, O3, true, true>
#define VC_3(O1, O2) VC_F(O1, O2, Wave::Off), VC_F(O1, O2, Wave::Saw)
#define VC_O(O1) VC_3(O1, Wave::Off), VC_3(O1, Wave::Saw), VC_3(O1, Wave::Pulse)
const Kernel* const kKernels[72] = {VC_O(Wave::Off), VC_O(Wave::Saw), VC_O(Wave::Pulse)};
// A table read for every wave (a square of width 0.5 has its own tables); two reads for any other pulse (describe()).
const Kernel* kernelFor(const Sound& s) {
    return kKernels[((int(s.plan.osc[0]) * 3 + int(s.plan.osc[1])) * 2 + (s.plan.osc[2] != Wave::Off)) * 4 + (s.plan.filter[0] != 0) * 2 + (s.plan.filter[1] != 0)];
}

struct FtzScope {  // flush denormals to zero while rendering (speed only: no result relies on denormals)
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    uint64_t old;
    FtzScope() { __asm__ volatile("mrs %0, fpcr" : "=r"(old)); __asm__ volatile("msr fpcr, %0" ::"r"(old | (1ull << 24))); }
    ~FtzScope() { __asm__ volatile("msr fpcr, %0" ::"r"(old)); }
#elif VC_SSE2
    unsigned old;
    FtzScope() { old = _mm_getcsr(); _mm_setcsr(old | 0x8040); }
    ~FtzScope() { _mm_setcsr(old); }
#endif
};

void clearPos(Pool& p, uint32_t i) {
    for (int k = 0; k < NFIELDS; ++k) p.f[k][i] = 0.f;
    p.f[INC1][i] = p.f[INC2][i] = p.f[INC3][i] = 0.01f;
    p.f[ASTG][i] = p.f[FSTG][i] = 2.f; p.f[VOL][i] = p.f[FADE][i] = 1.f; p.f[FRESH][i] = 1.f; p.f[VFEG][i] = p.f[VFEG2][i] = 1.f;
    p.f[UGL][i] = p.f[UGR][i] = 1.f;
    p.voice[i] = 0xff; p.channel[i] = 0; p.rng[i] = 1;
}
}  // namespace

Core::Core() {
    (void)bandTables();  // built once per process, here on the caller's thread, never in render
    (void)lawTables();
    for (auto& p : panel_) p[3] = 1024;  // foot controller 2 rests at the top
    reset();
}

void Core::prepare(double sr) {
    sr_ = sr > 0 ? sr : 48000;
    reset();
    for (auto& s : sound_) s.loaded = false;
}

void Core::compile(Sound& s, const Patch& q) const {
    s = Sound{};
    applyLaws(s, q, sr_);
    s.kernel = kernelFor(s);
    s.loaded = true;
}

void Core::install(uint32_t i, const Sound& in) {
    if (i >= kMaxSounds) return;
    Sound& s = sound_[i];
    const float gain = s.gain, l = s.panL, r = s.panR;
    s = in;
    s.gain = gain; s.panL = l; s.panR = r;
    for (int k = 0; k < 8; ++k) knob_[i][k] = s.knob[k];
    for (int d = 0; d < kDests; ++d) dest_[i][d] = s.stored[d];
    bool m = false;
    for (float x : mod_[i]) m |= x != 0.f;
    live_[i] = m;
}

// A 7-bit controller value v (0..127) as a box source (va-round5 C1: the mod wheel through a cutoff box lands on the static cutoff of
// this map at 11 values within 1e-5 of F, where round(v x 1023 / 127) misses by 0.4-1.9 %): 8 v up to 64, then 512 + (v - 64) 512 / 63.
static float sevenBit(float x) {
    const int v = int((x < 0 ? 0.f : x > 1 ? 1.f : x) * 127.f + 0.5f);
    return v <= 64 ? float(8 * v) : 512.f + float(v - 64) * (512.f / 63);
}

void Core::setPanel(uint32_t i, int source, float x) {
    const int k = source == 6 ? 0 : source == 16 ? 1 : source == 17 ? 2 : source == 4 ? 3 : source == 5 ? 4 : -1;
    if (i < kMaxSounds && k >= 0) panel_[i][k] = sevenBit(x);
}

void Core::setDestination(uint32_t i, int d, int v) {
    if (i >= kMaxSounds || d < 0 || d >= kDests) return;
    if (d == DCutoff) v += sound_[i].cutOffset;
    if (d == DFegDepth || d == DFegDepth2) v -= 256;
    dest_[i][d] = int16_t(v < kDestDefs[d].lo ? kDestDefs[d].lo : v > kDestDefs[d].hi ? kDestDefs[d].hi : v);
    live_[i] = true;
}

void Core::modulateDestination(uint32_t i, int d, float steps) {
    if (i >= kMaxSounds || d < 0 || d >= kDests || !(steps == steps)) return;  // NaN ignored
    const float o = steps * 8192.f / float(kDestDefs[d].span);
    mod_[i][d] = o < -16384.f ? -16384.f : o > 16384.f ? 16384.f : o;
    live_[i] = true;
}

void Core::setKnob(uint32_t i, int k, float x) {
    if (i < kMaxSounds && k >= 0 && k < 8) knob_[i][k] = x < 0 ? 0.f : x > 1 ? 1.f : x;
}

void Core::setGain(uint32_t i, float gain, float pan) {
    if (i >= kMaxSounds) return;
    // equal power without libm: a quarter sine by its odd polynomial on [0, pi/2]
    auto qsin = [](float x) { const float x2 = x * x; return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72)))); };
    const float a = (pan < -1 ? -1 : pan > 1 ? 1 : pan) * 0.78539816f + 0.78539816f;
    sound_[i].gain = gain; sound_[i].panL = qsin(1.5707963f - a); sound_[i].panR = qsin(a);
}

float Core::pitchNow_(uint32_t si) const {
    const int u = lastVoice_[si];
    if (u >= 0 && slot_[u].sound == int(si) && !slot_[u].parked) {
        const Pool& p = pool_[si]; const uint32_t i = slot_[u].pos;
        return p.f[KEY][i] + p.f[GOFF][i];
    }
    return lastKey_[si];
}

void Core::glide_(uint32_t si, uint32_t i, float from) {
    Pool& p = pool_[si]; const Sound& s = sound_[si];
    float off = from - p.f[KEY][i];
    if (s.portaTimeMode >= 2) off = off < -12.f ? -12.f : off > 12.f ? 12.f : off;
    p.f[GOFF][i] = off;
    p.f[GRATE][i] = s.portaTimeMode & 1 ? (off < 0 ? -off : off) * s.portaRate / 12.f : s.portaRate;
}

void Core::retrigger_(uint32_t si, uint32_t i) {
    Pool& p = pool_[si]; const Sound& s = sound_[si];
    p.f[ASTG][i] = p.f[FSTG][i] = kStageAttack;  // from the level they are at
    for (int k = 0; k < 3; ++k) if (s.plan.lfo[k] && s.lfo[k].reset) p.f[LP0 + k][i] = s.lfo[k].phase0;
    for (int k = 0; k < 2; ++k) if (s.plan.oscLfo[k] && s.oscLfo[k].reset) p.f[LO1 + k][i] = s.oscLfo[k].phase0;
}

void Core::park_(uint32_t si, uint32_t from, uint32_t to) {
    for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == from) x.owner = uint8_t(to);
    slot_[from].parked = true; slot_[from].owner = uint8_t(to);
}

void Core::unhold_(uint32_t si, uint32_t v) {
    uint8_t& n = nheld_[si];
    for (uint32_t k = 0; k < n; ++k)
        if (held_[si][k].voice == v) { for (uint32_t j = k + 1; j < n; ++j) held_[si][j - 1] = held_[si][j]; --n; return; }
}

void Core::noteOn(uint32_t v, uint32_t si, int ch, int key, float vel) {
    if (v >= kMaxVoices || si >= kMaxSounds || !sound_[si].loaded) return;
    kill(v);
    Pool& p = pool_[si]; const Sound& s = sound_[si];
    const float k = float(key) + s.keyOffset;
    // The sound's note that takes the new one over: a mono sound's (if one sounds and is not fading), or with single key
    // assignment one already on this key. Its earlier note ends (a mono sound keeps it held, to come back to); every voice of it
    // (a unison note's copies) goes over to the new note together.
    int take = -1;
    for (uint32_t i = 0; i < p.n && take < 0; ++i)
        if (p.f[DFADE][i] == 0.f && (s.mono || (s.single && p.f[KEY][i] == k))) take = int(i);
    bool keyHeld = s.mono ? nheld_[si] > 0 : false;
    if (!s.mono) for (uint32_t i = 0; i < p.n; ++i) keyHeld |= p.f[ASTG][i] < kStageRelease && p.f[DFADE][i] == 0.f;
    const float from = pitchNow_(si);
    if (take >= 0) {
        const uint32_t u = p.voice[take];
        const bool uHeld = s.mono ? nheld_[si] && held_[si][nheld_[si] - 1].voice == u : p.f[ASTG][take] < kStageRelease;
        if (uHeld) park_(si, u, v);  // its key is still down: its note-off then only lets it go
        else { for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == u) x.owner = uint8_t(v); slot_[u] = Slot{}; }
        for (uint32_t i = 0; i < p.n; ++i) {
            if (p.voice[i] != u) continue;
            p.voice[i] = uint8_t(v); p.channel[i] = uint8_t(ch & 15);
            p.f[KEY][i] = k; p.f[GOFF][i] = 0;
            if (!(s.mono && s.porta && keyHeld)) retrigger_(si, i);  // legato only in mono with portamento, a key held
            if (s.porta && (s.portaFull || keyHeld)) glide_(si, i, from);
        }
        slot_[v] = {int8_t(si), uint8_t(take), false, 0, 0};
    } else {
        // room for every voice of the note: a note already fading out (the furthest gone first) is cut, as a full pool does
        while (p.n + s.unison > kMaxVoices) {
            int cut = -1;
            for (uint32_t i = 0; i < p.n; ++i) if (p.f[DFADE][i] > 0.f && (cut < 0 || p.f[FADE][i] < p.f[FADE][cut])) cut = int(i);
            if (cut < 0) return;  // no room: the note does not sound (voice(v) is not sounding; the caller ends it)
            free_(si, p.voice[cut]);
        }
        auto rnd = [this] { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return float(rng_ >> 8) * (1.f / 16777216.f); };
        const int vi = int(vel * 127.f + 0.5f) < 0 ? 0 : int(vel * 127.f + 0.5f) > 127 ? 127 : int(vel * 127.f + 0.5f);
        for (uint32_t c = 0; c < s.unison; ++c) {  // each voice of the note: its own free-running phases, drift and LFO phases
            const uint32_t i = p.n++;
            clearPos(p, i);
            p.voice[i] = uint8_t(v); p.channel[i] = uint8_t(ch & 15);
            p.f[KEY][i] = float(key) + s.keyOffset;
            p.f[UDET][i] = s.uniDet[c]; p.f[UGL][i] = s.uniL[c]; p.f[UGR][i] = s.uniR[c];
            p.f[PH1][i] = rnd(); p.f[PH2][i] = rnd();  // free-running oscillators
            p.f[PW1][i] = s.pw[0]; p.f[PW2][i] = s.pw[1];
            // drift: the process runs on in the reference's voices between notes; a fresh voice starts anywhere in it
            p.rng[i] = (rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5, rng_ | 1u);
            p.f[DS1][i] = s.driftMean + (rnd() * 2 - 1) * s.driftStatic; p.f[DS2][i] = s.driftMean + (rnd() * 2 - 1) * s.driftStatic;
            p.f[DA1][i] = rnd() * 2 - 1; p.f[DB1][i] = rnd() * 2 - 1; p.f[DP1][i] = rnd();
            p.f[DA2][i] = rnd() * 2 - 1; p.f[DB2][i] = rnd() * 2 - 1; p.f[DP2][i] = rnd();
            if (s.plan.osc[2] != Wave::Off) {
                p.f[PH3][i] = rnd();
                p.f[DS3][i] = s.driftMean + (rnd() * 2 - 1) * s.driftStatic;
                p.f[DA3][i] = rnd() * 2 - 1; p.f[DB3][i] = rnd() * 2 - 1; p.f[DP3][i] = rnd();
            }
            p.f[DCUT][i] = s.driftCutMean + (rnd() * 2 - 1) * s.driftCutSpread;
            for (int f = 0; f < 2; ++f) {  // key follow from key 60 (the played key; with a zone or part shift not measured) and velocity, semitones
                p.f[f ? DCUT2 : DCUT][i] += s.flt[f].keyFollow * float(key - 60) + float(s.velCut[f][vi]) * 0.125f;
                p.f[f ? VFEG2 : VFEG][i] = float(s.velFeg[f][vi]) * (1.f / 128);
                p.f[f ? VRES2 : VRES][i] = float(s.velRes[f][vi]);
            }
            for (int n = 0; n < 3; ++n) {  // key-on reset: the cycle starts at the phase; else anywhere (not measured how it continues)
                if (!s.plan.lfo[n]) continue;
                p.f[LP0 + n][i] = s.lfo[n].reset ? s.lfo[n].phase0 : rnd();
                p.f[LR0 + n][i] = rnd() * 2 - 1;
            }
            for (int n = 0; n < 2; ++n) {  // the oscillators' own LFOs (the pulse width's), the same way
                if (!s.plan.oscLfo[n]) continue;
                p.f[LO1 + n][i] = s.oscLfo[n].reset ? s.oscLfo[n].phase0 : rnd();
                p.f[LOR1 + n][i] = rnd() * 2 - 1;
            }
            p.f[ASTG][i] = p.f[FSTG][i] = kStageAttack;
            if (s.porta && (s.portaFull || keyHeld) && lastVoice_[si] >= 0) glide_(si, i, from);
            slot_[v] = {int8_t(si), uint8_t(i), false, 0, 0};
        }
    }
    if (s.mono && nheld_[si] < kMaxVoices) held_[si][nheld_[si]++] = {uint8_t(v), k};
    lastKey_[si] = k; lastVoice_[si] = int16_t(v);
    chan_[si] = uint8_t(ch & 15);
}

void Core::noteOff(uint32_t v) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    const uint32_t si = uint32_t(slot_[v].sound);
    Pool& p = pool_[si]; const Sound& s = sound_[si];
    if (slot_[v].parked) { unhold_(si, v); slot_[v] = Slot{}; return; }  // a key whose voice another note took over
    if (s.mono) {  // the key comes off the held list; the note's voices go back to the newest key still held, else release
        unhold_(si, v);
        if (nheld_[si]) {
            const Held h = held_[si][nheld_[si] - 1];
            const uint32_t lead = slot_[v].pos;
            const float from = p.f[KEY][lead] + p.f[GOFF][lead];
            for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == v) x.owner = h.voice;
            slot_[v] = Slot{};
            for (uint32_t i = 0; i < p.n; ++i) {
                if (p.voice[i] != v) continue;
                p.voice[i] = h.voice; p.f[KEY][i] = h.key; p.f[GOFF][i] = 0;
                if (s.porta) glide_(si, i, from); else retrigger_(si, i);
            }
            slot_[h.voice] = {int8_t(si), uint8_t(lead), false, 0, 0};
            lastKey_[si] = h.key; lastVoice_[si] = int16_t(h.voice);
            return;
        }
    }
    for (uint32_t i = 0; i < p.n; ++i) {
        if (p.voice[i] != v) continue;
        auto rel = [&](int LV, int ST, int END, int LOW, const Env& e) {
            if (p.f[ST][i] >= kStageRelease) return;
            const float lvl = p.f[LV][i];
            const bool low = lvl < e.threshold;
            p.f[ST][i] = kStageRelease; p.f[LOW][i] = low ? 1.f : 0.f;
            p.f[END][i] = lvl * (low ? e.endLow : e.endHigh);
        };
        rel(AENV, ASTG, AEND, ALOW, s.amp);
        rel(FENV, FSTG, FEND, FLOW, s.feg);
    }
    ending_[si] = true;
}

void Core::choke(uint32_t v, uint32_t frames) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    if (slot_[v].parked) { unhold_(uint32_t(slot_[v].sound), v); slot_[v] = Slot{}; return; }
    Pool& p = pool_[slot_[v].sound];
    for (uint32_t i = 0; i < p.n; ++i)  // every voice of the note fades together
        if (p.voice[i] == v && p.f[DFADE][i] == 0.f) p.f[DFADE][i] = 1.f / float(frames ? frames : 1);
    ending_[slot_[v].sound] = true;
}

// Frees every voice of a note (all its unison copies) at once: a note never loses a part of itself.
void Core::free_(uint32_t si, uint32_t owner) {
    Pool& p = pool_[si];
    slot_[owner] = Slot{};
    for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == owner) x = Slot{};  // the keys it took over end with it
    if (sound_[si].mono) nheld_[si] = 0;
    for (uint32_t i = p.n; i-- > 0;) {
        if (p.voice[i] != owner) continue;
        const uint32_t last = --p.n;  // the positions above i are not the owner's: the one moved down is another note's
        if (i != last) {
            for (int k = 0; k < NFIELDS; ++k) p.f[k][i] = p.f[k][last];
            p.voice[i] = p.voice[last]; p.channel[i] = p.channel[last]; p.rng[i] = p.rng[last];
        }
        clearPos(p, last);
    }
    for (uint32_t i = 0; i < p.n; ++i) slot_[p.voice[i]].pos = uint8_t(i);
}

void Core::kill(uint32_t v) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    const uint32_t si = uint32_t(slot_[v].sound);
    unhold_(si, v);
    if (slot_[v].parked) { slot_[v] = Slot{}; return; }
    free_(si, v);
}

void Core::setVolume(uint32_t v, float g) {
    if (v >= kMaxVoices || slot_[v].sound < 0 || slot_[v].parked) return;
    Pool& p = pool_[slot_[v].sound];
    for (uint32_t i = 0; i < p.n; ++i) if (p.voice[i] == v) p.f[VOL][i] = g;
}
void Core::setPitchBend(int ch, float x) {
    if (ch < 0 || ch >= 16) return;
    bend_[ch] = x < -1 ? -1 : x > 1 ? 1 : x;
    const float v = float(int((bend_[ch] + 1) * 512.f));  // as a box source: the 14-bit bend / 16, whole (va-round3 R1 C_pb)
    src_[ch][0] = v > 1023.f ? 1023.f : v;
}
void Core::setController(int ch, int source, float x) {
    if (ch < 0 || ch >= 16 || source <= 0 || source >= kSources || source == 4 || source == 5 || source == 6 || source >= 8) return;  // the sound's
    src_[ch][source] = sevenBit(x);
}

void Core::reset() {
    for (uint32_t si = 0; si < kMaxSounds; ++si) { pool_[si].n = 0; for (uint32_t i = 0; i < kMaxVoices; ++i) clearPos(pool_[si], i); }
    for (auto& s : slot_) s = Slot{};
    for (auto& b : bend_) b = 0;
    for (uint32_t si = 0; si < kMaxSounds; ++si) { nheld_[si] = 0; lastVoice_[si] = -1; lastKey_[si] = 0; }
    for (int ch = 0; ch < 16; ++ch) resetControllers(ch);
}
void Core::resetControllers(int ch) {
    if (ch < 0 || ch >= 16) return;
    float* c = src_[ch];
    for (int i = 0; i < kSources; ++i) c[i] = 0;
    c[0] = float(int((bend_[ch] + 1) * 512.f)); if (c[0] > 1023.f) c[0] = 1023.f;
    c[3] = c[4] = c[7] = 1024;  // foot controllers and breath rest at the top (the reference's reset all controllers)
}

// a note is done when a voice of it reached exactly zero in its release, or its fade ended (its voices do so together)
void Core::reap_(uint32_t si) {
    Pool& p = pool_[si];
    bool ending = false;
    for (uint32_t i = 0; i < p.n;) {
        const bool rel = p.f[ASTG][i] >= kStageRelease, fade = p.f[DFADE][i] > 0.f;
        if ((rel && p.f[AENV][i] <= 0.f) || (fade && p.f[FADE][i] <= 0.f)) { free_(si, p.voice[i]); i = 0; ending = false; continue; }  // the whole note
        ending |= rel || fade;
        ++i;
    }
    ending_[si] = ending;
}

void Core::render(float* L, float* R, uint32_t frames) {
    uint8_t busy[kMaxSounds]; uint32_t nb = 0;  // the sounds with voices: no voice starts or ends within a call
    for (uint32_t si = 0; si < kMaxSounds; ++si) if (pool_[si].n) busy[nb++] = uint8_t(si);
    if (!nb) return;
    FtzScope ftz;
    alignas(16) float bend[kMaxSounds][16];  // each sound's bend per channel: fixed for this call, so once per call
    for (uint32_t b = 0; b < nb; ++b) {
        const uint32_t si = busy[b];
        for (int ch = 0; ch < 16; ++ch) bend[si][ch] = bend_[ch] * (bend_[ch] >= 0 ? sound_[si].bendUp : -sound_[si].bendDown);
    }
    for (uint32_t done = 0; done < frames;) {
        const int n = int(frames - done < uint32_t(kSub) ? frames - done : uint32_t(kSub));
        for (uint32_t b = 0; b < nb; ++b) {
            const uint32_t si = busy[b];
            Pool& p = pool_[si]; const Sound& s = sound_[si];
            if (!p.n) continue;  // its last voice ended at an earlier step
            Ctl c;
            c.n = n; c.rn = 1.f / float(n); c.piOverSr = float(3.14159265358979 / sr_);
            const float fn = float(n);
            float la = s.amp.la, ld = s.amp.ld, lr = s.amp.lr, lrl = s.amp.lrLow, fla = s.feg.la, fld = s.feg.ld, flr = s.feg.lr, flrl = s.feg.lrLow;
            for (int f = 0; f < 2; ++f) { c.cut[f] = s.flt[f].cut; c.feg[f] = s.flt[f].feg; c.res[f] = s.flt[f].res; }
            c.lfoPitch[0] = s.lfoPitch[0]; c.lfoPitch[1] = s.lfoPitch[1]; c.lfoPitch[2] = s.lfoPitch[2];
            c.osc[0] = s.osc[0]; c.osc[1] = s.osc[1]; c.osc[2] = s.osc[2];
            for (int o = 0; o < 3; ++o) { c.tab[o] = s.tab[o]; c.stride[o] = s.stride[o]; }
            c.portaRate = s.portaRate;
            c.freq[0] = c.freq[1] = c.freq[2] = 0; c.pwRaw[0] = s.pwRaw[0]; c.pwRaw[1] = s.pwRaw[1]; c.lfoCut = s.lfoCut; c.lfoRate = s.lfo[0].rate;
            if (s.boxes || live_[si]) {  // the destinations the boxes (or the host) move, at the sources' values now (tables: no libm)
                int16_t raw[kDests];
                float src[kSources];
                for (int k = 0; k < kSources; ++k) src[k] = src_[chan_[si]][k];
                src[6] = panel_[si][0]; src[16] = panel_[si][1]; src[17] = panel_[si][2]; src[4] = panel_[si][3]; src[5] = panel_[si][4];
                boxRaw(s, src, knob_[si], raw, dest_[si], mod_[si]);
                const LawTables& t = lawTables();
                c.lfoPitch[0] = t.pitchSemi[raw[DPitchDepth1]]; c.lfoPitch[1] = t.pitchSemi[raw[DPitchDepth2]]; c.lfoPitch[2] = t.pitchSemi[raw[DPitchDepth3]];
                if (s.osc[0] > 0) c.osc[0] = t.oscLevel[raw[DOscLevel1]];  // an oscillator at level 0 is not in the plan
                if (s.osc[1] > 0) c.osc[1] = t.oscLevel[raw[DOscLevel2]];
                for (int f = 0; f < 2; ++f) {
                    const Dest dc = f ? DCutoff2 : DCutoff, dd = f ? DFegDepth2 : DFegDepth, dr = f ? DReso2 : DReso;
                    c.cut[f] = t.cutSemi[raw[dc]];
                    const float steps = float(raw[dd]) + (f ? 0.f : s.fegOffSteps);  // the part's offset (filter 1), then the depth's stop (+-192)
                    c.feg[f] = kFegSemisPerStep * (steps < -192.f ? -192.f : steps > 192.f ? 192.f : steps);
                    c.res[f] = float(raw[dr]);
                }
                for (int n = 0; n < 3; ++n) c.freq[n] = float(raw[DFreq1 + n]) * 0.01f;
                c.pwRaw[0] = float(raw[DPw1]); c.pwRaw[1] = float(raw[DPw2]);
                if (raw[DLfoCutDepth] != s.stored[DLfoCutDepth]) c.lfoCut = float(raw[DLfoCutDepth] - 256) * s.lfoCutPerStep;
                for (int o = 0; o < 3; ++o)  // a wave the boxes moved (coverage: only where no pulse of another width than 0.5 is involved)
                    if (raw[DWave1 + o] != s.stored[DWave1 + o]) { c.tab[o] = waveTable(raw[DWave1 + o], true); c.stride[o] = raw[DWave1 + o] == 4 ? 0.f : float(kTabN + 1); }
                if (raw[DPorta] != s.stored[DPorta]) c.portaRate = t.portaRate[raw[DPorta]] * float(1 / sr_);
                if (raw[DLfoSpeed] != s.stored[DLfoSpeed]) { const float r = t.lfoHz[raw[DLfoSpeed]] * float(1 / sr_); c.lfoRate = r < kLfoMaxRate ? r : kLfoMaxRate; }
                const float k = -s.invSrLn2;
                auto tau = [&](int kind, int d, float stored) { return raw[d] == s.stored[d] ? stored : k * t.invTau[kind][raw[d]]; };
                la = tau(TAttack, DAmpAttack, la); ld = tau(TAmpDecay, DAmpDecay, ld);
                if (raw[DAmpRelease] != s.stored[DAmpRelease]) { lr = k * t.invTau[TAmpRelease][raw[DAmpRelease]]; lrl = lr * (s.amp.lrLow / s.amp.lr); }  // the slower law keeps its ratio
                fla = tau(TAttack, DFegAttack, fla); fld = tau(TFegDecay, DFegDecay, fld);
                if (raw[DFegRelease] != s.stored[DFegRelease]) { flr = k * t.invTau[TFegRelease][raw[DFegRelease]]; flrl = flr * (s.feg.lrLow / s.feg.lr); }
            }
            c.ka = stepCoef(la * fn); c.kd = stepCoef(ld * fn); c.kr = stepCoef(lr * fn); c.krl = stepCoef(lrl * fn);
            c.fa = stepCoef(fla * fn); c.fd = stepCoef(fld * fn); c.fr = stepCoef(flr * fn); c.frl = stepCoef(flrl * fn);
            std::memcpy(c.bend, bend[si], sizeof c.bend);
            vf accL[kSub], accR[kSub];
            for (int k = 0; k < n; ++k) accL[k] = accR[k] = splat(0.f);
            s.kernel->run(p, s, c, accL, accR, interleave_);
            for (int k = 0; k < n; ++k) { L[done + k] += hsum(accL[k]); R[done + k] += hsum(accR[k]); }
            if (ending_[si]) reap_(si);  // only a sound with a voice releasing or fading can lose one
        }
        done += uint32_t(n);
    }
    for (uint32_t b = 0; b < nb; ++b) { const Pool& p = pool_[busy[b]]; for (uint32_t i = 0; i < p.n; ++i) slot_[p.voice[i]].age += frames; }
}

uint32_t Core::voiceCount() const { uint32_t n = 0; for (auto& p : pool_) n += p.n; return n; }

VoiceState Core::voice(uint32_t v) const {
    if (v >= kMaxVoices || slot_[v].sound < 0) return {false, false, 0.f, 0};
    if (slot_[v].parked) return {true, false, 0.f, slot_[v].age};  // a mono sound's held key: sounding through the newer note
    const Pool& p = pool_[slot_[v].sound]; const uint32_t i = slot_[v].pos;
    return {true, p.f[ASTG][i] >= kStageRelease || p.f[DFADE][i] > 0.f, p.f[AENV][i] * p.f[VOL][i] * p.f[FADE][i], slot_[v].age};
}

}  // namespace vcore

namespace vcore {
// Instructions per 128-frame block, measured with callgrind on aarch64 (g++ 13 -O3 -mcpu=cortex-a53, NEON,
// two lanes interleaved; tools/cost/cg.sh, plan-* runs: 8 voices held, docs/cost.md). Per voice at full
// lanes of 4; a partly filled lane costs as a full one. A table read (a saw, or a pulse of width 0.5)
// is measured; any other pulse width is two reads, counted as such (not measured).
Cost estimateCost(const PlanDesc& d) {
    constexpr double kBase = 3010, kRead = 1670, kOsc3 = 2225, kTwoSections = 2810, kOneSection = 2175, kLfo = 130, kOscLfo = 416, kBox = 47, kRest = 216,
                     kSat = 228;  // the saturators' stage (round 7 plan 15 against plan 10: 1.8k a block at 8 voices)
    double v = kBase + kBox * d.boxes;  // the boxes run once per sound a step: their share at 8 voices
    for (int n = 0; n < 2; ++n) v += d.osc[n] == Wave::Saw ? kRead : d.osc[n] == Wave::Pulse ? 2 * kRead : 0;
    if (d.osc[2] != Wave::Off) v += kOsc3;  // its read, pitch and drift (plan 10: 2.2k a voice)
    for (bool l : d.oscLfo) v += l ? kOscLfo : 0;  // the LFO and the width's ramp (plan 11: with the second read, 2.1k)
    // a filter of two sections (LPF24, HPF24; round 5 plans 4 and 14: 2.8k a voice at 48 kHz, its poles mapped), any other type as a
    // section and a one-pole (LPF18, plan 12: 2.2k; LPF12, LPF6 and their high-passes not measured, counted so); the base 3.0k: the
    // control step is one call a lane (round 5: shared by the lane variants, for code size)
    for (int f = 0; f < 2; ++f) if (d.filter[f]) v += d.filter[f] == 1 || d.filter[f] == 5 ? kTwoSections : kOneSection;
    for (bool l : d.lfo) v += l ? kLfo : 0;
    if (d.sat) v += kSat;
    return {v, kRest};  // rest: the whole core with no voice sounding (a sound adds nothing at rest)
}
}  // namespace vcore
