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
    float fegSemis;                           // filter EG depth (semitones), with the boxes now
    float lpSemi, k1, k2, lpGain;             // cutoff (semitones re 1 Hz), the sections' 1/Q and pass-band gain
    float lfoPitch[3], osc[3];                // pitch LFO depth (semitones), oscillator out level (linear)
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
VC_INLINE void oscillators(const Lane& l, const Sound& s, const Ctl& c, int n, vf* buf) {
    const float *t1 = s.tab[0], *t2 = s.tab[1], *t3 = s.tab[2];
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

// ---- filters: two TPT state-variable sections in series, N lanes interleaved ---------------------
#define VC_N(...) for (int m = 0; m < N; ++m) __VA_ARGS__
// The sections' normalisers 1 / (1 + g (g + k)) are computed from g every sample: ramping them separately from g
// leaves the sections unstable while g moves fast (a cutoff sweep of many octaves within one control step).
template <int N, bool HighPass>
VC_INLINE void svf4(const Lane* l, int S1, int G, float k1, float k2, float gain, int n, vf (*buf)[kSub]) {
    vf s1[N], s2[N], s3[N], s4[N], g[N], dg[N];
    VC_N({ s1[m] = l[m](S1); s2[m] = l[m](S1 + 1); s3[m] = l[m](S1 + 2); s4[m] = l[m](S1 + 3);
           g[m] = l[m](G); dg[m] = l[m](G + 1); });
    const vf K1 = splat(k1), K2 = splat(k2), gn = splat(gain), one = splat(1.f);
    for (int k = 0; k < n; ++k) {
        VC_N({
            g[m] = g[m] + dg[m];
            const vf gg = g[m];
            const vf d1 = one / fma(gg, gg + K1, one), d2 = one / fma(gg, gg + K2, one);
            vf hp = (buf[m][k] - fma(gg + K1, s1[m], s2[m])) * d1;
            vf v = gg * hp, bp = v + s1[m]; s1[m] = bp + v;
            v = gg * bp; vf lp = v + s2[m]; s2[m] = lp + v;
            vf x = HighPass ? hp : lp;
            hp = (x - fma(gg + K2, s3[m], s4[m])) * d2;
            v = gg * hp; bp = v + s3[m]; s3[m] = bp + v;
            v = gg * bp; lp = v + s4[m]; s4[m] = lp + v;
            buf[m][k] = (HighPass ? hp : lp) * gn;
        });
    }
    VC_N({ l[m].set(S1, s1[m]); l[m].set(S1 + 1, s2[m]); l[m].set(S1 + 2, s3[m]); l[m].set(S1 + 3, s4[m]);
           l[m].set(G, g[m]); });
}

constexpr float kHpK1 = 1 / 0.5412f, kHpK2 = 1 / 1.3066f;  // Butterworth pair (HPF24 stand-in, not covered)

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
VC_INLINE vf lfoStep(const Lane& l, const Lfo& o, int n, int LP, int LR, vf t, vf& m) {
    const vf one = splat(1.f);
    vf ph = l(LP) + splat(float(n) * o.rate);
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

// ---- control step for one lane ------------------------------------------------------------------
template <bool LP, bool HP, bool O3>
VC_INLINE void control(const Lane& l, const Sound& s, const Ctl& c) {
    const vm fresh = splat(0.5f) < l(FRESH);
    const vf rn = splat(c.rn);
    envStep(l, AENV, ASTG, AEND, ALOW, c.ka, c.kd, c.kr, c.krl, s.amp.sustain);
    if constexpr (LP) envStep(l, FENV, FSTG, FEND, FLOW, c.fa, c.fd, c.fr, c.frl, s.feg.sustain);
    // pitch: key (with the sound's offsets) + drift + the channel's bend
    alignas(16) float bend[W];
    for (int i = 0; i < W; ++i) bend[i] = c.bend[l.p->channel[l.o + i] & 15];
    vf semis = l(KEY) + load(bend);
    if (s.porta) {  // the glide's offset toward 0, linearly
        const vf g = l(GOFF), a = vmax(vabs(g) - l(GRATE) * splat(float(c.n)), splat(0.f));
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
            const vf v = lfoStep(l, s.lfo[0], c.n, LP0, LR0, t, m) * m;
            lp1 = v * splat(c.lfoPitch[0]); lp2 = v * splat(c.lfoPitch[1]); lp3 = v * splat(c.lfoPitch[2]);
        }
        for (int o = 0; o < 2; ++o)  // the pulse width, moved by the oscillator's own LFO: raw + depth x value through the width law
            if (s.plan.oscLfo[o]) {
                const vf v = lfoStep(l, s.oscLfo[o], c.n, LO1 + o, LOR1 + o, t, m) * m;
                const vf w = vmax(vmin(fma(fma(v, splat(s.pwDepth[o]), splat(s.pwRaw[o])), splat(0.98f / 256.f), splat(0.01f)), splat(0.99f)), splat(0.01f));
                l.ramp(PW1 + 2 * o, DPW1 + 2 * o, w, fresh, rn);
            }
        if (s.plan.lfo[1]) { const vf v = lfoStep(l, s.lfo[1], c.n, LP1, LR1, t, m); lcut = v * m * splat(s.lfoCut); }
        // amplitude: dB = -depth (1 - value): the top is the level (measured); the delay and fade-in scale the depth (not measured)
        if (s.plan.lfo[2]) { const vf v = lfoStep(l, s.lfo[2], c.n, LP2, LR2, t, m); lamp = (v * splat(s.lfoAmp) - splat(s.lfoAmp < 0 ? -s.lfoAmp : s.lfoAmp)) * m; }
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
    const vf lg1 = fma(semis + dt1 + lp1, splat(1.f / 12), splat(s.pitch[0])), lg2 = fma(semis + dt2 + lp2, splat(1.f / 12), splat(s.pitch[1]));
    l.ramp(INC1, DINC1, vmin(exp2v(lg1), splat(0.45f)), fresh, rn);
    l.ramp(INC2, DINC2, vmin(exp2v(lg2), splat(0.45f)), fresh, rn);
    level(l, TA1, TB1, TW1, DTW1, lg1, s.tabU, s.stride[0], fresh, rn);
    level(l, TA2, TB2, TW2, DTW2, lg2, s.tabU, s.stride[1], fresh, rn);
    if constexpr (O3) {
        const vf dt3 = fma(amp, fma(l(DB3) - l(DA3), l(DP3), l(DA3)), l(DS3));
        const vf lg3 = fma(semis + dt3 + lp3, splat(1.f / 12), splat(s.pitch[2]));
        l.ramp(INC3, DINC3, vmin(exp2v(lg3), splat(0.45f)), fresh, rn);
        level(l, TA3, TB3, TW3, DTW3, lg3, s.tabU, s.stride[2], fresh, rn);
    }
    const vf maxw = splat(1.41f);
    if constexpr (LP) {
        // the cutoff stops at the filter's top (raw 1023, open) however far the EG and the boxes drive it
        vf semi = vmin(fma(l(FENV), splat(c.fegSemis), splat(c.lpSemi)) + l(DCUT) + lcut, splat(s.lpTop));
        vf g = tanv(vmin(exp2v(semi * splat(1.f / 12)) * splat(c.piOverSr), maxw));
        l.ramp(LG, DLG, g, fresh, rn);
    }
    if constexpr (HP) {
        vf g = tanv(vmin(exp2v(splat(s.hpSemi / 12)) * splat(c.piOverSr), maxw));
        l.ramp(HG, DHG, g, fresh, rn);
    }
    // amplitude: EG x volume x fade x the sound's level and gain, equal-power pan
    vf fade = vmax(l(FADE) - l(DFADE) * splat(float(c.n)), splat(0.f));
    l.set(FADE, fade);
    vf g = l(AENV) * l(VOL) * fade * splat(s.level * s.gain);
    if (s.plan.lfo[2]) g = g * exp2v(lamp);
    l.ramp(GL, DGL, g * splat(s.panL), vm{}, rn);
    l.ramp(GR, DGR, g * splat(s.panR), vm{}, rn);
    l.set(FRESH, splat(0.f));
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
template <Wave O1, Wave O2, Wave O3, bool LP, bool HP>
struct Plan {
    template <int N>
    static VC_INLINE void lanes(Pool& p, const Sound& s, const Ctl& c, int j, vf* aL, vf* aR) {
        Lane l[N]; vf buf[N][kSub];
        for (int m = 0; m < N; ++m) {
            l[m] = Lane{&p, (j + m) * W};
            control<LP, HP, O3 != Wave::Off>(l[m], s, c);
            oscillators<O1, O2, O3>(l[m], s, c, c.n, buf[m]);
        }
        if constexpr (LP) svf4<N, false>(l, L1, LG, c.k1, c.k2, c.lpGain, c.n, buf);
        if constexpr (HP) svf4<N, true>(l, H1, HG, kHpK1, kHpK2, 1.f, c.n, buf);
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
    return kKernels[((int(s.plan.osc[0]) * 3 + int(s.plan.osc[1])) * 2 + (s.plan.osc[2] != Wave::Off)) * 4 + s.plan.lowPass * 2 + s.plan.highPass];
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
    p.f[ASTG][i] = p.f[FSTG][i] = 2.f; p.f[VOL][i] = p.f[FADE][i] = 1.f; p.f[FRESH][i] = 1.f;
    p.voice[i] = 0xff; p.channel[i] = 0; p.rng[i] = 1;
}
}  // namespace

Core::Core() {
    (void)bandTables();  // built once per process, here on the caller's thread, never in render
    (void)lawTables();
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

void Core::setPanel(uint32_t i, int source, float x) {
    const int k = source == 6 ? 0 : source == 16 ? 1 : source == 17 ? 2 : -1;
    if (i < kMaxSounds && k >= 0) panel_[i][k] = float(int((x < 0 ? 0.f : x > 1 ? 1.f : x) * 1023.f + 0.5f));
}

void Core::setDestination(uint32_t i, int d, int v) {
    if (i >= kMaxSounds || d < 0 || d >= kDests) return;
    if (d == DCutoff) v += sound_[i].cutOffset;
    if (d == DFegDepth) v -= 256;
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
    // The sound's voice that takes the note over: a mono sound's voice (if one sounds and is not fading), or with single key
    // assignment a voice already on this key. Its earlier note ends (a mono sound keeps it held, to come back to).
    int take = -1;
    for (uint32_t i = 0; i < p.n && take < 0; ++i)
        if (p.f[DFADE][i] == 0.f && (s.mono || (s.single && p.f[KEY][i] == k))) take = int(i);
    bool keyHeld = s.mono ? nheld_[si] > 0 : false;
    if (!s.mono) for (uint32_t i = 0; i < p.n; ++i) keyHeld |= p.f[ASTG][i] < kStageRelease && p.f[DFADE][i] == 0.f;
    const float from = pitchNow_(si);
    if (take >= 0) {
        const uint32_t i = uint32_t(take), u = p.voice[i];
        const bool uHeld = s.mono ? nheld_[si] && held_[si][nheld_[si] - 1].voice == u : p.f[ASTG][i] < kStageRelease;
        if (uHeld) park_(si, u, v);  // its key is still down: its note-off then only lets it go
        else { for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == u) x.owner = uint8_t(v); slot_[u] = Slot{}; }
        p.voice[i] = uint8_t(v); p.channel[i] = uint8_t(ch & 15);
        p.f[KEY][i] = k; p.f[GOFF][i] = 0;
        if (!(s.mono && s.porta && keyHeld)) retrigger_(si, i);  // legato only in mono with portamento, a key held
        if (s.porta && (s.portaFull || keyHeld)) glide_(si, i, from);
        slot_[v] = {int8_t(si), uint8_t(i), false, 0, 0};
    } else {
        if (p.n >= kMaxVoices) return;
    const uint32_t i = p.n++;
    clearPos(p, i);
    auto rnd = [this] { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return float(rng_ >> 8) * (1.f / 16777216.f); };
    p.voice[i] = uint8_t(v); p.channel[i] = uint8_t(ch & 15);
    p.f[KEY][i] = float(key) + s.keyOffset;
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
    for (int k = 0; k < 3; ++k) {  // key-on reset: the cycle starts at the phase; else anywhere (not measured how it continues)
        if (!s.plan.lfo[k]) continue;
        p.f[LP0 + k][i] = s.lfo[k].reset ? s.lfo[k].phase0 : rnd();
        p.f[LR0 + k][i] = rnd() * 2 - 1;
    }
    for (int k = 0; k < 2; ++k) {  // the oscillators' own LFOs (the pulse width's), the same way
        if (!s.plan.oscLfo[k]) continue;
        p.f[LO1 + k][i] = s.oscLfo[k].reset ? s.oscLfo[k].phase0 : rnd();
        p.f[LOR1 + k][i] = rnd() * 2 - 1;
    }
    p.f[ASTG][i] = p.f[FSTG][i] = kStageAttack;
    if (s.porta && (s.portaFull || keyHeld) && lastVoice_[si] >= 0) glide_(si, i, from);
    slot_[v] = {int8_t(si), uint8_t(i), false, 0, 0};
    }
    (void)vel;  // velocity has no effect on the covered laws (measured at sensitivity 0)
    if (s.mono && nheld_[si] < kMaxVoices) held_[si][nheld_[si]++] = {uint8_t(v), k};
    lastKey_[si] = k; lastVoice_[si] = int16_t(v);
    chan_[si] = uint8_t(ch & 15);
}

void Core::noteOff(uint32_t v) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    const uint32_t si = uint32_t(slot_[v].sound);
    Pool& p = pool_[si]; const Sound& s = sound_[si]; const uint32_t i = slot_[v].pos;
    if (slot_[v].parked) { unhold_(si, v); slot_[v] = Slot{}; return; }  // a key whose voice another note took over
    if (s.mono) {  // the key comes off the held list; the voice goes back to the newest key still held, else releases
        unhold_(si, v);
        if (nheld_[si]) {
            const Held h = held_[si][nheld_[si] - 1];
            const float from = p.f[KEY][i] + p.f[GOFF][i];
            for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == v) x.owner = h.voice;
            slot_[v] = Slot{};
            p.voice[i] = h.voice; p.f[KEY][i] = h.key; p.f[GOFF][i] = 0;
            slot_[h.voice] = {int8_t(si), uint8_t(i), false, 0, 0};
            if (s.porta) glide_(si, i, from); else retrigger_(si, i);
            lastKey_[si] = h.key; lastVoice_[si] = int16_t(h.voice);
            return;
        }
    }
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

void Core::choke(uint32_t v, uint32_t frames) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    if (slot_[v].parked) { unhold_(uint32_t(slot_[v].sound), v); slot_[v] = Slot{}; return; }
    Pool& p = pool_[slot_[v].sound]; const uint32_t i = slot_[v].pos;
    if (p.f[DFADE][i] == 0.f) p.f[DFADE][i] = 1.f / float(frames ? frames : 1);
}

void Core::free_(uint32_t si, uint32_t i) {
    Pool& p = pool_[si];
    const uint8_t owner = p.voice[i];
    slot_[owner] = Slot{};
    for (auto& x : slot_) if (x.parked && x.sound == int(si) && x.owner == owner) x = Slot{};  // the keys it took over end with it
    if (sound_[si].mono) nheld_[si] = 0;
    const uint32_t last = --p.n;
    if (i != last) {
        for (int k = 0; k < NFIELDS; ++k) p.f[k][i] = p.f[k][last];
        p.voice[i] = p.voice[last]; p.channel[i] = p.channel[last]; p.rng[i] = p.rng[last];
        slot_[p.voice[i]].pos = uint8_t(i);
    }
    clearPos(p, last);
}

void Core::kill(uint32_t v) {
    if (v >= kMaxVoices || slot_[v].sound < 0) return;
    const uint32_t si = uint32_t(slot_[v].sound);
    unhold_(si, v);
    if (slot_[v].parked) { slot_[v] = Slot{}; return; }
    free_(si, slot_[v].pos);
}

void Core::setVolume(uint32_t v, float g) {
    if (v < kMaxVoices && slot_[v].sound >= 0 && !slot_[v].parked) pool_[slot_[v].sound].f[VOL][slot_[v].pos] = g;
}
void Core::setPitchBend(int ch, float x) {
    if (ch < 0 || ch >= 16) return;
    bend_[ch] = x < -1 ? -1 : x > 1 ? 1 : x;
    const float v = float(int((bend_[ch] + 1) * 512.f));  // as a box source: the 14-bit bend / 16, whole (va-round3 R1 C_pb)
    src_[ch][0] = v > 1023.f ? 1023.f : v;
}
void Core::setController(int ch, int source, float x) {
    if (ch < 0 || ch >= 16 || source <= 0 || source >= kSources || source == 6 || source >= 8) return;  // knobs, ribbon, switches: the sound's
    src_[ch][source] = float(int((x < 0 ? 0.f : x > 1 ? 1.f : x) * 1023.f + 0.5f));  // a 7-bit controller: value x 1023 / 127, rounded (R1 C_*)
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
    c[3] = c[4] = c[7] = 1023;  // foot controllers and breath rest at the top (the reference's reset all controllers)
}

// a voice is done when its release reached exactly zero, or its fade ended
void Core::reap_(uint32_t si) {
    Pool& p = pool_[si];
    for (uint32_t i = p.n; i-- > 0;)
        if ((p.f[ASTG][i] >= kStageRelease && p.f[AENV][i] <= 0.f) || (p.f[DFADE][i] > 0.f && p.f[FADE][i] <= 0.f)) free_(si, i);
}

void Core::render(float* L, float* R, uint32_t frames) {
    bool any = false;
    for (auto& p : pool_) any |= p.n != 0;
    if (!any) return;
    FtzScope ftz;
    for (uint32_t done = 0; done < frames;) {
        const int n = int(frames - done < uint32_t(kSub) ? frames - done : uint32_t(kSub));
        for (uint32_t si = 0; si < kMaxSounds; ++si) {
            Pool& p = pool_[si]; const Sound& s = sound_[si];
            if (!p.n) continue;
            Ctl c;
            c.n = n; c.rn = 1.f / float(n); c.piOverSr = float(3.14159265358979 / sr_);
            const float fn = float(n);
            float la = s.amp.la, ld = s.amp.ld, lr = s.amp.lr, lrl = s.amp.lrLow, fla = s.feg.la, fld = s.feg.ld, flr = s.feg.lr, flrl = s.feg.lrLow;
            c.fegSemis = s.fegSemis; c.lpSemi = s.lpSemi; c.k1 = s.lpK1; c.k2 = s.lpK2; c.lpGain = s.lpGain;
            c.lfoPitch[0] = s.lfoPitch[0]; c.lfoPitch[1] = s.lfoPitch[1]; c.lfoPitch[2] = s.lfoPitch[2];
            c.osc[0] = s.osc[0]; c.osc[1] = s.osc[1]; c.osc[2] = s.osc[2];
            if (s.boxes || live_[si]) {  // the destinations the boxes (or the host) move, at the sources' values now (tables: no libm)
                int16_t raw[kDests];
                float src[kSources];
                for (int k = 0; k < kSources; ++k) src[k] = src_[chan_[si]][k];
                src[6] = panel_[si][0]; src[16] = panel_[si][1]; src[17] = panel_[si][2];
                boxRaw(s, src, knob_[si], raw, dest_[si], mod_[si]);
                const LawTables& t = lawTables();
                c.lfoPitch[0] = t.pitchSemi[raw[DPitchDepth1]]; c.lfoPitch[1] = t.pitchSemi[raw[DPitchDepth2]]; c.lfoPitch[2] = t.pitchSemi[raw[DPitchDepth3]];
                if (s.osc[0] > 0) c.osc[0] = t.oscLevel[raw[DOscLevel1]];  // an oscillator at level 0 is not in the plan
                if (s.osc[1] > 0) c.osc[1] = t.oscLevel[raw[DOscLevel2]];
                c.lpSemi = t.cutSemi[raw[DCutoff]];
                const float steps = float(raw[DFegDepth]) + s.fegOffSteps;  // the part's offset, then the depth's stop (+-192)
                c.fegSemis = kFegSemisPerStep * (steps < -192.f ? -192.f : steps > 192.f ? 192.f : steps);
                c.k1 = t.k1[raw[DReso]]; c.k2 = t.k2[raw[DReso]]; c.lpGain = t.gain[raw[DReso]];
                const float k = -s.invSrLn2;
                auto tau = [&](int kind, int d, float stored) { return raw[d] == s.stored[d] ? stored : k * t.invTau[kind][raw[d]]; };
                la = tau(TAttack, DAmpAttack, la); ld = tau(TAmpDecay, DAmpDecay, ld);
                if (raw[DAmpRelease] != s.stored[DAmpRelease]) { lr = k * t.invTau[TAmpRelease][raw[DAmpRelease]]; lrl = lr * (s.amp.lrLow / s.amp.lr); }  // the slower law keeps its ratio
                fla = tau(TAttack, DFegAttack, fla); fld = tau(TFegDecay, DFegDecay, fld);
                if (raw[DFegRelease] != s.stored[DFegRelease]) { flr = k * t.invTau[TFegRelease][raw[DFegRelease]]; flrl = flr * (s.feg.lrLow / s.feg.lr); }
            }
            c.ka = stepCoef(la * fn); c.kd = stepCoef(ld * fn); c.kr = stepCoef(lr * fn); c.krl = stepCoef(lrl * fn);
            c.fa = stepCoef(fla * fn); c.fd = stepCoef(fld * fn); c.fr = stepCoef(flr * fn); c.frl = stepCoef(flrl * fn);
            for (int ch = 0; ch < 16; ++ch) c.bend[ch] = bend_[ch] * (bend_[ch] >= 0 ? s.bendUp : -s.bendDown);
            vf accL[kSub], accR[kSub];
            for (int k = 0; k < n; ++k) accL[k] = accR[k] = splat(0.f);
            s.kernel->run(p, s, c, accL, accR, interleave_);
            for (int k = 0; k < n; ++k) { L[done + k] += hsum(accL[k]); R[done + k] += hsum(accR[k]); }
            for (uint32_t i = 0; i < p.n; ++i) slot_[p.voice[i]].age += uint64_t(n);
            reap_(si);
        }
        done += uint32_t(n);
    }
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
    constexpr double kBase = 2780, kRead = 1670, kOsc3 = 2225, kLowPass = 1295, kHighPass = 1155, kLfo = 130, kOscLfo = 416, kBox = 47, kRest = 216;
    double v = kBase + kBox * d.boxes;  // the boxes run once per sound a step: their share at 8 voices
    for (int n = 0; n < 2; ++n) v += d.osc[n] == Wave::Saw ? kRead : d.osc[n] == Wave::Pulse ? 2 * kRead : 0;
    if (d.osc[2] != Wave::Off) v += kOsc3;  // its read, pitch and drift (plan 10: 2.2k a voice)
    for (bool l : d.oscLfo) v += l ? kOscLfo : 0;  // the LFO and the width's ramp (plan 11: with the second read, 2.1k)
    if (d.lowPass) v += kLowPass;
    if (d.highPass) v += kHighPass;
    for (bool l : d.lfo) v += l ? kLfo : 0;
    return {v, kRest};  // rest: the whole core with no voice sounding (a sound adds nothing at rest)
}
}  // namespace vcore
