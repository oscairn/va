// SPDX-License-Identifier: AGPL-3.0-or-later
// The patch: defaults, text form, coverage and the plan a patch needs.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "vcore.h"

namespace vcore {
double cutoffHz(int raw);  // laws.cpp: the cutoff law
int satIndex(int drive);   // laws.cpp: a measured saturator drive's index (-1: not measured)

const ParamDef kParamDefs[kParamCount] = {
#define X(id, name, lo, hi, def) {#id, name, lo, hi, def},
#include "params.inc"
#undef X
};

Patch::Patch() { for (int i = 0; i < kParamCount; ++i) v[i] = kParamDefs[i].def; }
bool Patch::set(Param p, int x) {
    if (p >= kParamCount || x < kParamDefs[p].lo || x > kParamDefs[p].hi) return false;
    v[p] = int16_t(x);
    return true;
}
bool operator==(const Patch& a, const Patch& b) { return !std::memcmp(a.v, b.v, sizeof a.v); }

int findParam(const char* id, size_t n) {
    for (int i = 0; i < kParamCount; ++i)
        if (std::strlen(kParamDefs[i].id) == n && !std::memcmp(kParamDefs[i].id, id, n)) return i;
    return -1;
}

static const char kHeader[] = "vcore-patch/1";

size_t writePatch(const Patch& p, char* out, size_t cap) {
    size_t need = 0;
    auto put = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i, ++need) if (need + 1 < cap) out[need] = s[i];
    };
    put(kHeader, sizeof kHeader - 1);
    char buf[96];
    for (int i = 0; i < kParamCount; ++i) {
        if (p.v[i] == kParamDefs[i].def) continue;
        const int n = std::snprintf(buf, sizeof buf, " %s=%d", kParamDefs[i].id, p.v[i]);
        put(buf, size_t(n));
    }
    if (cap) out[need < cap ? need : cap - 1] = 0;
    return need;
}

bool readPatch(const char* text, Patch& out, char* error, size_t cap) {
    auto fail = [&](const char* why, const char* tok, size_t n) {
        if (error && cap) std::snprintf(error, cap, "%s '%.*s'", why, int(n > 60 ? 60 : n), tok);
        return false;
    };
    Patch p;
    const char* s = text ? text : "";
    bool first = true;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
        if (!*s) break;
        const char* tok = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') ++s;
        const size_t n = size_t(s - tok);
        if (first) {
            first = false;
            if (n != sizeof kHeader - 1 || std::memcmp(tok, kHeader, n)) return fail("not a vcore-patch/1 text:", tok, n);
            continue;
        }
        const char* eq = static_cast<const char*>(std::memchr(tok, '=', n));
        if (!eq) return fail("malformed", tok, n);
        const int id = findParam(tok, size_t(eq - tok));
        if (id < 0) return fail("unknown parameter in", tok, n);
        char num[16] = {};
        const size_t nl = size_t(s - eq - 1);
        if (!nl || nl >= sizeof num) return fail("bad value in", tok, n);
        std::memcpy(num, eq + 1, nl);
        char* end = nullptr;
        const long x = std::strtol(num, &end, 10);
        if (*end || !p.set(Param(id), int(x))) return fail("bad value in", tok, n);
    }
    if (first) return fail("empty patch text", "", 0);
    out = p;
    return true;
}

// ---- coverage -----------------------------------------------------------------------------------
namespace {
// The filters' measured reach (va-round5; docs/coverage.md). Resonance: any value while the cutoff's highest frequency (the patch's
// with its boxes, the EG at its peak, key follow up to the zone's top key, velocity) is at most kResAnyHz, up to 64 to kRes64Hz, up
// to 32 anywhere (LPF12 and LPF18 within 1.3 dB at the peak harmonic there, LPF24 within 0.7 dB). HPF18 and HPF6 measured from
// cutoff 340 to 960.
constexpr float kResAnyHz[2] = {2100, 700}, kRes64Hz[2] = {5900, 2100};  // LPF12 / LPF18, LPF24
struct Gaps {
    Gap* out; uint32_t cap, n = 0;
    void add(Param p, const char* why) { if (n < cap) out[n] = {p, why}; ++n; }
    // the value must be within [lo, hi]
    void need(const Patch& q, Param p, int lo, int hi, const char* why) { if (q[p] < lo || q[p] > hi) add(p, why); }
};
// Per-oscillator and per-filter parameter blocks are laid out alike: an offset from the first.
constexpr int kOscStride = Osc2OscillatorWave - Osc1OscillatorWave;
constexpr int kFilterStride = Filter2FilterType - Filter1FilterType;
constexpr int kBoxStride = Box2ControllerSetSwitch - Box1ControllerSetSwitch;
Param osc(int n, Param p1) { return Param(p1 + n * kOscStride); }
Param flt(int n, Param p1) { return Param(p1 + n * kFilterStride); }
Param box(int n, Param p1) { return Param(p1 + n * kBoxStride); }

// Why a box destination the core does not move is not played, by the reference's destination numbers.
const char* unbuiltDest(int d) {
    if (d <= 51 || (d >= 151 && d <= 374) || (d >= 377 && d <= 400))
        return "controller box on an effect, send, arpeggio, motion sequence or part mixing parameter: the host's, not the engine's";
    if ((d >= 128 && d <= 135) || d == 137 || d == 138 || d == 403 || d >= 406)
        return "controller box on self sync, wave shaper, FM, ring, noise or the modifier: out of scope";
    if (d == 120 || d == 121 || d == 126) return "controller box on the pitch or pulse width EG: not built";
    if (d >= 56 && d <= 59) return "controller box on the part LFO: not built";
    return "controller box destination: not built";
}

// The raw range each box destination reaches: a knob source at the patch's own knob value, any other source over its
// whole range (a performance controller); the boxes' outputs summed as the reference does (laws.cpp boxRaw).
struct Reach { int lo[kDests], hi[kDests]; bool moved[kDests]; };
void reach(const Patch& q, Gaps& g, Reach& r) {
    float lo[kDests] = {}, hi[kDests] = {};
    bool any[kDests] = {};
    for (int b = 0; b < 16; ++b) {
        if (!q[box(b, Box1ControllerSetSwitch)]) continue;
        if (q[box(b, Box1ControllerSetRatio)] == 128) continue;  // ratio 0: its output, (c + o - 1024) x ratio / 8, is 0 whatever it drives
        const int src = q[box(b, Box1ControllerSetSource)], dest = q[box(b, Box1ControllerSetDestination)];
        Dest d[4]; int nd = 0;
        auto oscOn = [&](int n) { return q[osc(n, Osc1OscillatorOutLevel)] && q[Param(osc(n, Osc1OscillatorControllerSet1Switch) + b)]; };
        const bool f1 = q[Filter1FilterType] && q[Param(Filter1FilterControllerBox1Switch + b)];
        const bool f2 = q[Filter2FilterType] && q[Param(Filter2FilterControllerBox1Switch + b)];
        switch (dest) {
        case kDestPitchDepth: for (int n = 0; n < 2; ++n) if (oscOn(n)) d[nd++] = Dest(DPitchDepth1 + n); if (oscOn(2)) d[nd++] = DPitchDepth3; break;
        case kDestOscLevel: for (int n = 0; n < 2; ++n) if (q[Param(osc(n, Osc1OscillatorControllerSet1Switch) + b)]) d[nd++] = Dest(DOscLevel1 + n); break;
        case kDestCutoff: case kDestFegDepth: case kDestReso:
            if (f1) d[nd++] = dest == kDestCutoff ? DCutoff : dest == kDestFegDepth ? DFegDepth : DReso;
            if (f2) d[nd++] = dest == kDestCutoff ? DCutoff2 : dest == kDestFegDepth ? DFegDepth2 : DReso2;
            break;
        case kDestFegAttack: d[nd++] = DFegAttack; break;
        case kDestFegDecay: d[nd++] = DFegDecay; break;
        case kDestFegRelease: d[nd++] = DFegRelease; break;
        case kDestAmpAttack: d[nd++] = DAmpAttack; break;
        case kDestAmpDecay: d[nd++] = DAmpDecay; break;
        case kDestAmpRelease: d[nd++] = DAmpRelease; break;
        case kDestFreq: for (int n = 0; n < 3; ++n) if (oscOn(n)) d[nd++] = Dest(DFreq1 + n); break;
        case kDestPw:
            for (int n = 0; n < 2; ++n) if (oscOn(n)) d[nd++] = Dest(DPw1 + n);
            if (oscOn(2)) g.add(box(b, Box1ControllerSetDestination), "controller box on oscillator 3's pulse width: not built");
            break;
        case kDestLfoCutDepth:
            if (f1) d[nd++] = DLfoCutDepth;
            if (f2) g.add(box(b, Box1ControllerSetDestination), "controller box on filter 2's cutoff LFO depth: not measured");
            break;
        case kDestPitchLfoSpeed: d[nd++] = DLfoSpeed; break;
        case kDestWave: for (int n = 0; n < 3; ++n) if (oscOn(n)) d[nd++] = Dest(DWave1 + n); break;
        case kDestPorta: if (q[PortamentoMaster] && q[PartPortamentoSwitch]) d[nd++] = DPorta; break;
        case kDestPitch: d[nd++] = DPitch; break;
        case kDestLfoAll: d[nd++] = DLfoAll; break;
        case kDestAmpLfo: d[nd++] = DAmpLfo; break;
        default: g.add(box(b, Box1ControllerSetDestination), unbuiltDest(dest)); continue;
        }
        if (!nd) continue;  // reaches no unit that sounds
        const bool knob = src >= 8 && src <= 15;
        if (src == 4 || src == 5) { g.add(box(b, Box1ControllerSetSource), "controller box source foot controller 2 / foot switch: their rest values measured (top / 0), their value map not"); continue; }
        if (src >= kSources) { g.add(box(b, Box1ControllerSetSource), "controller box source: not built (sequenced and envelope-following sources)"); continue; }
        const int curve = findCurve(q[box(b, Box1ControllerSetCurveBank)], q[box(b, Box1ControllerSetCurveType)],
                                    q[box(b, Box1ControllerSetCurveParameter1)], q[box(b, Box1ControllerSetCurveParameter2)]);
        if (curve < 0) { g.add(box(b, Box1ControllerSetCurveType), "controller box curve: not measured"); continue; }
        float clo, chi;
        if (knob) {
            const float s = float(q[Param(PartAssignableKnob1Value + src - 8)]);
            if (curve > 0) {  // a measured curve holds at its knots only
                bool onKnot = false;
                for (int v = 0; v < 128; ++v) { const float k = curveKnot(v); onKnot |= s > k - 2.5f && s < k + 2.5f; }
                if (!onKnot) g.add(box(b, Box1ControllerSetCurveType), "controller box curve: measured at the mod wheel's 128 values, the knob is between them");
            }
            clo = chi = curveAt(curve, s);
        } else {  // the 7-bit sources land on the knots; the bend (v / 16) between them
            if (curve > 0 && src == 0) g.add(box(b, Box1ControllerSetCurveType), "controller box curve: measured at the mod wheel's 128 values, the pitch bend moves between them");
            clo = 1e9f; chi = -1e9f;
            for (int v = 0; v < 128; ++v) { const float c = curveAt(curve, curveKnot(v)); clo = c < clo ? c : clo; chi = c > chi ? c : chi; }
        }
        const int rr = q[box(b, Box1ControllerSetRatio)] - 128;
        const float ratio = float(rr == 127 ? 128 : rr), off = q[box(b, Box1ControllerSetPolarity)] ? 512.f : 1024.f;
        auto out = [&](float c) { const float o = (c + off - 1024) * ratio / 8; return o < -8192 ? -8192.f : o > 8192 ? 8192.f : o; };
        const float a = out(clo), z = out(chi);
        for (int i = 0; i < nd; ++i) { lo[d[i]] += a < z ? a : z; hi[d[i]] += a < z ? z : a; any[d[i]] = true; }
    }
    const int st[kDests] = {q[Osc1OscillatorPitchLFODepth], q[Osc2OscillatorPitchLFODepth], q[Osc1OscillatorOutLevel], q[Osc2OscillatorOutLevel],
                            [&] { const int c = q[Filter1FilterCutoff] + 16 * (q[PartFilterCutoffFrequency] - 64); return c < 0 ? 0 : c > 1023 ? 1023 : c; }(),  // the part's offset
                            q[Filter1FilterCutoffEGDepth] - 256 + 3 * (q[PartFEGDepth] - 64), q[Filter1FilterResonance], q[FilterCutoffEGAttackTime],
                            q[FilterCutoffEGDecayTime], q[FilterCutoffEGReleaseTime], q[AmplitudeEGAttackTime], q[AmplitudeEGDecayTime],
                            q[AmplitudeEGReleaseTime], q[Osc3OscillatorPitchLFODepth], q[Filter2FilterCutoff], q[Filter2FilterCutoffEGDepth] - 256,
                            q[Filter2FilterResonance], q[Osc1OscillatorPulseWidth], q[Osc2OscillatorPulseWidth], q[Filter1FilterCutoffLFODepth],
                            q[PitchLFOSpeed], 0, 0, 0, q[PartPortamentoTime], 0, 0, q[AmplitudeLevelLFODepth], q[Osc1OscillatorWave], q[Osc2OscillatorWave], q[Osc3OscillatorWave]};
    for (int d = 0; d < kDests; ++d) {
        auto at = [&](float t) {
            t = t < -8192 ? -8192 : t > 8192 ? 8192 : t;
            const float x = t * kDestDefs[d].span / 8192;
            const int v = st[d] + int(d < DWave1 ? x : x + (x < 0 ? -0.5f : 0.5f));  // as boxRaw
            return v < kDestDefs[d].lo ? int(kDestDefs[d].lo) : v > kDestDefs[d].hi ? int(kDestDefs[d].hi) : v;
        };
        r.lo[d] = any[d] ? at(lo[d]) : st[d]; r.hi[d] = any[d] ? at(hi[d]) : st[d];
        r.moved[d] = any[d] && (r.lo[d] != st[d] || r.hi[d] != st[d]);
    }
}
}  // namespace

uint32_t notCovered(const Patch& q, Gap* out, uint32_t cap) {
    Gaps g{out, cap};
    Reach r;
    reach(q, g, r);
    // common
    g.need(q, RandomPanDepth, 0, 0, "pan modulation: not built");
    g.need(q, AlternatePanDepth, 64, 64, "pan modulation: not built");
    g.need(q, ScalingPanDepth, 64, 64, "pan modulation: not built");
    g.need(q, KeyOnDelayTimeLength, 0, 0, "key-on delay: not built");
    g.need(q, KeyOnDelayTempoSyncSwitch, 0, 0, "key-on delay: not built");
    g.need(q, OSCReset, 0, 0, "oscillator reset modes: not built (free running only)");
    // ageing (va-round7 X1 / G2, va-round8 D / R2): no effect on the drift found from 0 to 200 at drift 0, nor from 100 to 200 at drift 64
    // and 100 (12 voices each); it moves the voice-to-voice spread of short releases (measured at 55), which the engine does not have
    if (q[Ageing] != 100 && ((q[VoltageDrift] && q[Ageing] < 100) || r.lo[DAmpRelease] < 100 || ((q[Filter1FilterType] || q[Filter2FilterType]) && r.lo[DFegRelease] < 100)))
        g.add(Ageing, "ageing: no effect on the drift (0-200 at drift 0, 100-200 at drift 64 and 100); with a release under 100 it moves the voices' spread of release times, not built");
    g.need(q, NoiseGeneratorOutLevel, 0, 0, "noise: out of scope");
    // oscillators
    for (int n = 0; n < 3; ++n) {
        const int level = q[osc(n, Osc1OscillatorOutLevel)];
        if (!level) { if (n < 2 && r.hi[DOscLevel1 + n] > 0) g.add(osc(n, Osc1OscillatorOutLevel), "a box raising a silent oscillator: not built"); continue; }
        const int w = q[osc(n, Osc1OscillatorWave)];
        // a box on the wave (va-round7 A2): the table swapped at a control step, so only where every wave it reaches is one table read
        if (r.moved[DWave1 + n] && (q[osc(n, Osc1OscillatorPulseWidth)] != 128 || (n < 2 && (q[osc(n, Osc1OscillatorPulseWidthLFODepth)] != 128 || r.moved[DPw1 + n]))))
            g.add(osc(n, Osc1OscillatorWave), "controller box on the wave with a pulse width other than 128 or moving: not built");
        if (n == 2) {  // oscillator 3: a table wave (its pulse only at width 128), its own LFO and the boxes not built
            if (w == 2 && (q[osc(n, Osc1OscillatorPulseWidth)] != 128 || q[osc(n, Osc1OscillatorPulseWidthLFODepth)] != 128))
                g.add(osc(n, Osc1OscillatorPulseWidth), "oscillator 3's pulse: only width 128 is built");
            if (level < 90) g.add(osc(n, Osc1OscillatorOutLevel), "out level under 90: no measured law");
            for (int b = 0; b < 16; ++b) {
                const int dd = q[box(b, Box1ControllerSetDestination)];
                if (q[box(b, Box1ControllerSetSwitch)] && dd == kDestOscLevel && q[Param(osc(n, Osc1OscillatorControllerSet1Switch) + b)]) {
                    g.add(box(b, Box1ControllerSetDestination), "controller box on oscillator 3's level: not built"); break;
                }
            }
        } else {
            if (r.lo[DOscLevel1 + n] < 90) g.add(osc(n, Osc1OscillatorOutLevel), "out level under 90 (with its boxes): no measured law");
            const int pwd = q[osc(n, Osc1OscillatorPulseWidthLFODepth)];
            if (w == 2 && pwd != 128) {  // measured: depth 136-192 with a sine at speed 150
                if (pwd < 128 || pwd > 192) g.add(osc(n, Osc1OscillatorPulseWidthLFODepth), "pulse width LFO depth: measured 128-192");
                g.need(q, osc(n, Osc1OscillatorLFOSpeed), 0, 320, "oscillator LFO speed over 320: audio rate, not built");
                g.need(q, osc(n, Osc1OscillatorLFODelayTime), 0, 0, "oscillator LFO delay: not measured");
                g.need(q, osc(n, Osc1OscillatorLFOFadeInTime), 0, 0, "oscillator LFO fade-in: not measured");
            }
        }
        g.need(q, osc(n, Osc1OscillatorOutSelect), 0, 0, "output to the amplifier: not built");
        g.need(q, osc(n, Osc1OscillatorOutInvertEnable), 0, 0, "invert: not built");
        g.need(q, osc(n, Osc1OscillatorPitchEGDepth), 247, 247, "pitch EG: not built");
        g.need(q, osc(n, Osc1OscillatorSelfSyncPitch), 0, 0, "sync: out of scope");
        g.need(q, osc(n, Osc1OscillatorSelfSyncEGDepth), 256, 256, "sync: out of scope");
        g.need(q, osc(n, Osc1OscillatorSelfSyncLFODepth), 256, 256, "sync: out of scope");
        g.need(q, osc(n, Osc1OscillatorPulseWidthEGDepth), 128, 128, "pulse width EG: not built");

        g.need(q, osc(n, Osc1OscillatorWaveShaper), 0, 0, "wave shaper: out of scope");
        g.need(q, osc(n, Osc1OscillatorWaveShaperEGDepth), 128, 128, "wave shaper: out of scope");
        g.need(q, osc(n, Osc1OscillatorWaveShaperLFODepth), 128, 128, "wave shaper: out of scope");
        g.need(q, osc(n, Osc1OscillatorFMLevel), 0, 0, "FM: out of scope");
        g.need(q, osc(n, Osc1OscillatorRingLevel), 0, 0, "ring: out of scope");
        for (Param p : {Osc1OscillatorPitchEGDepthVelocitySensitivity, Osc1OscillatorSelfSyncPitchVelocitySensitivity,
                        Osc1OscillatorPulseWidthVelocitySensitivity, Osc1OscillatorWaveShaperVelocitySensitivity,
                        Osc1OscillatorFMLevelVelocitySensitivity, Osc1OscillatorRingLevelVelocitySensitivity,
                        Osc1OscillatorOutLevelVelocitySensitivity})
            g.need(q, osc(n, p), 256, 256, "velocity sensitivity: no measured law");
    }
    // filters (va-round5: the reference's sections and their laws, docs/coverage.md)
    for (int f = 0; f < 2; ++f) {
        const int t = q[flt(f, Filter1FilterType)];
        if (!t) continue;
        const Param type = flt(f, Filter1FilterType);
        if (t > 8) { g.add(type, "band-pass filters: not built"); continue; }
        const bool high = t >= 5;
        const Dest dc = f ? DCutoff2 : DCutoff, dr = f ? DReso2 : DReso, dd = f ? DFegDepth2 : DFegDepth;
        if (high && r.hi[dr] > 0) g.add(flt(f, Filter1FilterResonance), "high-pass resonance: its pass-band gain not measured");
        if ((t == 6 || t == 8) && (r.lo[dc] < 340 || r.hi[dc] > 960)) g.add(flt(f, Filter1FilterCutoff), "HPF18 / HPF6: measured at cutoffs 340-960");
        if (!high && t != 4 && r.hi[dr] > 32) {
            const double depth = 0.5 * std::max(0, r.hi[dd]) + (f ? 0.0 : 1.5 * (q[PartFEGDepth] - 64) * (q[PartFEGDepth] > 64));
            const int top = q[ZoneZoneNoteLimitHigh] < q[PartNoteLimitHigh] ? q[ZoneZoneNoteLimitHigh] : q[PartNoteLimitHigh];
            static const double kf[6] = {0, 1 / 3.0, 0.5, 2 / 3.0, 1, 2};
            const double vs = (q[flt(f, Filter1FilterCutoffVelocitySensitivity)] - 256) * 0.065 * (q[flt(f, Filter1FilterCutoffVelocitySensitivity)] > 256);
            const double peak = cutoffHz(r.hi[dc]) * std::exp2((depth + kf[q[flt(f, Filter1FilterCutoffKeyFollow)]] * std::max(0, top - 60) + vs) / 12);
            const int k = t == 1;
            if (peak > kResAnyHz[k] && (r.hi[dr] > 64 || peak > kRes64Hz[k]))
                g.add(flt(f, Filter1FilterResonance), "resonance at high cutoffs: the fit misses by over 1 dB there (any value to 2.1 kHz, LPF24 0.7 kHz; to 64 up to 5.9 / 2.1 kHz; 32 anywhere)");
        }
        if (f == 1 && q[PartFilterCutoffFrequency] != 64 && !(q[Filter2FilterCutoff] == 0 && q[PartFilterCutoffFrequency] < 64))
            g.add(PartFilterCutoffFrequency, "the part's cutoff offset on filter 2: not measured");
        if (r.lo[dd] < -160) g.add(flt(f, Filter1FilterCutoffEGDepth), "EG depth (with its boxes) under 96: law measured 96-448 (it stops at 448)");
        if (f == 1) g.need(q, Filter2FilterCutoffLFODepth, 256, 256, "filter 2 LFO depth: not measured");
        if (satIndex(q[flt(f, Filter1FilterSaturatorDrive)]) < 0) g.add(flt(f, Filter1FilterSaturatorDrive), "saturator drive: measured at 0, 2, 4, 7, 8, 12, 15, 16 and 23");
        else if (!f && q[Filter1FilterSaturatorDrive] && q[Filter2FilterType] && !highPassAsGain(q))
            g.add(Filter1FilterSaturatorDrive, "filter 1's saturator with filter 2 a filter: the saturators run after both filters, so not built");
        g.need(q, flt(f, Filter1FilterOutLevel), 64, 64, "filter out level: no measured law");
        g.need(q, flt(f, Filter1FilterSaturatorDriveVelocitySensitivity), 256, 256, "saturator velocity sensitivity: not built");
        g.need(q, flt(f, Filter1FilterResonanceVelocitySensitivity), 256, 256, "resonance velocity sensitivity: measured at velocities 1 and 127 only");
    }
    g.need(q, ModifierWaveFolder, 0, 0, "wave folder: out of scope");
    // envelopes
    const int t1 = q[Filter1FilterType] || q[Filter2FilterType];
    if (t1 && (r.lo[DFegDecay] < 100 || r.hi[DFegDecay] > 255)) g.add(FilterCutoffEGDecayTime, "decay (with its boxes): law measured 100-255");
    if (t1 && r.lo[DFegRelease] < 55) g.add(FilterCutoffEGReleaseTime, "release (with its boxes): law measured 55-255");
    g.need(q, FilterCutoffEGTimeVelocitySensitivity, 256, 256, "EG time velocity sensitivity: no measured law");
    g.need(q, AmplitudeLevel, 56, 511, "amplitude level: law measured 56-511");
    g.need(q, AmplitudeLevelVelocitySensitivity, 256, 256, "velocity sensitivity: measured only at 0 (no effect)");
    // LFOs: speeds above 320 (108 Hz) are audio rate, which the control-rate LFO does not reproduce
    const PlanDesc d = describe(q);
    if (d.lfo[0]) {
        g.need(q, PitchLFOSpeed, 0, 320, "pitch LFO speed over 320: audio rate, not built");
        g.need(q, PitchLFODelayTime, 0, 96, "pitch LFO delay over 96: not measured (law extrapolated past 5.3 s)");
        if (q[PitchLFOFadeInTime] && q[PitchLFOFadeInTime] < 96) g.add(PitchLFOFadeInTime, "pitch LFO fade-in under 96: shorter than 76 ms, not resolved");
    }
    if (d.lfo[1]) {  // depth verified 8-128 steps either way (va-round8 F, round 7 A3: within 0.83 % of the law)
        g.need(q, Filter1FilterCutoffLFODepth, 256 - 128, 256 + 128, "filter LFO depth: verified up to 128 steps from 256");
        g.need(q, FilterCutoffLFOSpeed, 0, 320, "filter LFO speed over 320: audio rate, not built");
        g.need(q, FilterCutoffLFOWave, 3, 3, "filter LFO wave: only the sine verified on the filter LFO");
        g.need(q, FilterCutoffLFODelayTime, 0, 0, "filter LFO delay: not measured on the filter LFO");
        g.need(q, FilterCutoffLFOFadeInTime, 0, 0, "filter LFO fade-in: not measured on the filter LFO");
        if (r.moved[DLfoCutDepth] && (r.lo[DLfoCutDepth] < 128 || r.hi[DLfoCutDepth] > 384))
            g.add(Filter1FilterCutoffLFODepth, "filter LFO depth (with its boxes): verified up to 128 steps from 256");
    }
    if (d.lfo[0] && r.moved[DLfoSpeed] && r.hi[DLfoSpeed] > 320) g.add(PitchLFOSpeed, "pitch LFO speed (with its boxes) over 320: audio rate, not built");
    if (r.moved[DLfoAll]) {  // every LFO's speed (119) adds to each one's own
        const bool on[5] = {d.lfo[0], d.lfo[1], d.lfo[2], d.oscLfo[0], d.oscLfo[1]};
        const Param sp[5] = {PitchLFOSpeed, FilterCutoffLFOSpeed, AmplitudeLFOSpeed, Osc1OscillatorLFOSpeed, Osc2OscillatorLFOSpeed};
        for (int k = 0; k < 5; ++k)
            if (on[k] && (k ? q[sp[k]] : r.hi[DLfoSpeed]) + r.hi[DLfoAll] > 320) g.add(sp[k], "LFO speed (with its boxes) over 320: audio rate, not built");
    }
    if (d.lfo[2]) {
        g.need(q, AmplitudeLFOSpeed, 0, 320, "amplitude LFO speed over 320: audio rate, not built");
        g.need(q, AmplitudeLFOWave, 3, 3, "amplitude LFO wave: only the sine's phase is measured");
        g.need(q, AmplitudeLFOPhase, 0, 0, "amplitude LFO phase: measured at 0 only");
        g.need(q, AmplitudeLFODelayTime, 0, 0, "amplitude LFO delay: not measured");
        g.need(q, AmplitudeLFOFadeInTime, 0, 0, "amplitude LFO fade-in: not measured");
    }
    g.need(q, AmplitudeLevelKeyFollow, 0, 0, "amplitude key follow: no measured law");
    if (satIndex(q[AmplitudeSaturatorDrive]) < 0) g.add(AmplitudeSaturatorDrive, "saturator drive: measured at 0, 2, 4, 7, 8, 12, 15, 16 and 23");
    if (r.lo[DAmpDecay] < 100) g.add(AmplitudeEGDecayTime, "decay (with its boxes): law measured 100-255");
    g.need(q, AmplitudeEGTimeVelocitySensitivity, 256, 256, "EG time velocity sensitivity: no measured law");
    // part
    if (q[PortamentoMaster] && q[PartPortamentoSwitch]) {
        if (r.moved[DPorta] && (q[PartPortamentoTimeMode] & 1)) g.add(PartPortamentoTime, "controller box on the portamento time in time modes 1 and 3: not built");
        g.need(q, PartLegatoSlope, 0, 0, "legato slope: measured at 0 only");
    }
    g.need(q, PartVelocitySensitivityDepth, 64, 64, "velocity curve: measured only at depth 64");
    g.need(q, PartVelocitySensitivityOffset, 64, 64, "velocity curve: measured only at offset 64");
    g.need(q, PartVolume, 100, 100, "part volume: measured only at 100");
    g.need(q, PartPan, 64, 64, "part pan: measured only at centre");
    g.need(q, PartFEGDepth, 56, 80, "part FEG depth offset: law measured 56-80");
    g.need(q, PartFilterCutoffFrequency, 48, 96, "part cutoff offset: law measured 48-96");
    for (Param p : {PartAEGAttackTime, PartAEGDecayTime, PartAEGSustainLevel, PartAEGReleaseTime, PartFEGAttackTime,
                    PartFEGDecayTime, PartFEGSustainLevel, PartFEGReleaseTime, PartFilterResonanceWidth})
        g.need(q, p, 64, 64, "part offset: not built");
    g.need(q, PartDetune, 128, 128, "part detune: not built");
    g.need(q, PartMicroTuningScale, 0, 0, "micro tuning: not built");
    for (Param p : {PartLfoLFODepth1, PartLfoLFODepth2, PartLfoLFODepth3}) g.need(q, p, 0, 0, "part LFO: not built");
    for (Param p : {PartKeyControllerSet1Switch, PartKeyControllerSet2Switch, PartKeyControllerSet3Switch, PartKeyControllerSet4Switch})
        g.need(q, p, 0, 0, "key controller boxes: not built");
    return g.n;
}

bool keySounds(const Patch& q, int key, int vel) {
    auto in = [](int x, int lo, int hi) { return lo <= hi ? x >= lo && x <= hi : x >= lo || x <= hi; };
    return in(key, q[ZoneZoneNoteLimitLow], q[ZoneZoneNoteLimitHigh]) && in(key, q[PartNoteLimitLow], q[PartNoteLimitHigh]) &&
           in(vel, q[ZoneZoneVelocityLimitLow], q[ZoneZoneVelocityLimitHigh]) && in(vel, q[PartVelocityLimitLow], q[PartVelocityLimitHigh]);
}

// Filter 2 a high-pass at cutoff 64 or under (fc at most 5.2 Hz) with no resonance, EG depth, key follow, velocity or box moving it:
// the stage is left out and its pass-band gain applied to the level (va-round5 F3: HPF24 at 0 is -0.65 dB from 20 Hz up).
bool highPassAsGain(const Patch& q) {
    if (q[Filter2FilterType] < 5 || q[Filter2FilterType] > 8 || q[Filter2FilterCutoff] > 64 || q[Filter2FilterResonance] ||
        q[Filter2FilterCutoffEGDepth] != 256 || q[Filter2FilterCutoffKeyFollow] || q[Filter2FilterCutoffVelocitySensitivity] != 256 ||
        q[Filter2FilterCutoffEGDepthVelocitySensitivity] != 256 || q[Filter2FilterResonanceVelocitySensitivity] != 256)
        return false;
    for (int b = 0; b < 16; ++b) {
        const int d = q[box(b, Box1ControllerSetDestination)];
        if (q[box(b, Box1ControllerSetSwitch)] && q[Param(Filter2FilterControllerBox1Switch + b)] && (d == kDestCutoff || d == kDestFegDepth || d == kDestReso))
            return false;
    }
    return true;
}

PlanDesc describe(const Patch& q) {
    PlanDesc d{};
    // a table read for every wave but a pulse whose width is not 0.5 or moves (oscillator 3: a table read always; its LFO not built)
    // a box (ratio not 0) on a destination, reaching oscillator n (its switch for the box on), or any unit (n < 0)
    auto boxOn = [&](int dest, int n) {
        for (int b = 0; b < 16; ++b)
            if (q[box(b, Box1ControllerSetSwitch)] && q[box(b, Box1ControllerSetRatio)] != 128 && q[box(b, Box1ControllerSetDestination)] == dest &&
                (n < 0 || q[Param(osc(n, Osc1OscillatorControllerSet1Switch) + b)])) return true;
        return false;
    };
    for (int n = 0; n < 3; ++n) {
        const bool pwm = n < 2 && (q[osc(n, Osc1OscillatorPulseWidthLFODepth)] != 128 || boxOn(kDestPw, n));  // the width moves
        d.osc[n] = !q[osc(n, Osc1OscillatorOutLevel)] ? Wave::Off
                 : q[osc(n, Osc1OscillatorWave)] == 2 && n < 2 && (q[osc(n, Osc1OscillatorPulseWidth)] != 128 || pwm) ? Wave::Pulse : Wave::Saw;
        if (n < 2) d.oscLfo[n] = d.osc[n] == Wave::Pulse && pwm;
    }
    for (int f = 0; f < 2; ++f) { const int t = q[flt(f, Filter1FilterType)]; d.filter[f] = uint8_t(t < kFilterTypes ? t : 0); }
    if (highPassAsGain(q)) d.filter[1] = 0;  // filter 2 a high-pass under 5.2 Hz that nothing moves: its pass-band gain only (laws.cpp)
    for (int n = 0; n < 3; ++n) d.lfo[0] |= d.osc[n] != Wave::Off && q[osc(n, Osc1OscillatorPitchLFODepth)] != 247;
    for (int b = 0; b < 16; ++b)  // a box on an oscillator's pitch LFO depth
        if (q[box(b, Box1ControllerSetSwitch)] && q[box(b, Box1ControllerSetDestination)] == kDestPitchDepth)
            for (int n = 0; n < 3; ++n) d.lfo[0] |= d.osc[n] != Wave::Off && q[Param(osc(n, Osc1OscillatorControllerSet1Switch) + b)];
    d.lfo[1] = d.filter[0] && (q[Filter1FilterCutoffLFODepth] != 256 || boxOn(kDestLfoCutDepth, -1));
    for (int b = 0; b < 16; ++b) d.boxes += q[box(b, Box1ControllerSetSwitch)] ? 1 : 0;
    d.lfo[2] = q[AmplitudeLevelLFODepth] != 128 || boxOn(kDestAmpLfo, -1);
    d.sat = q[AmplitudeSaturatorDrive] || (q[Filter1FilterType] && q[Filter1FilterSaturatorDrive]) || (q[Filter2FilterType] && q[Filter2FilterSaturatorDrive]);
    auto add = [&](Stage s) { d.stage[d.count++] = s; };
    if (d.osc[0] != Wave::Off) add(d.osc[0] == Wave::Saw ? Stage::Saw1 : Stage::Pulse1);
    if (d.osc[1] != Wave::Off) add(d.osc[1] == Wave::Saw ? Stage::Saw2 : Stage::Pulse2);
    if (d.osc[2] != Wave::Off) add(Stage::Table3);
    if (d.filter[0]) add(Stage::Filter1);
    if (d.filter[1]) add(Stage::Filter2);
    add(Stage::Amp);
    return d;
}

}  // namespace vcore
