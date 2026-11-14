// SPDX-License-Identifier: AGPL-3.0-or-later
// The DSP core's own tests (core/, linked directly; no plugin): patch text, coverage, plans, voices,
// and the cross-build statistics of a fixed sequence (every target must stay within kTolerance of
// the stored values). VC_WRITE_STATS=1 prints the statistics in the form of probeA_stats.inc instead.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vcore.h"

using namespace vcore;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The "probe A" patch: a two-oscillator arpeggiated probe part, alone (tools/vcbench.cpp has the same text).
static const char* kProbeA =
    "vcore-patch/1 UnisonDetune=7 UnisonSpread=2 FilterCutoffEGAttackTime=30 FilterCutoffEGDecayTime=117 "
    "FilterCutoffEGSustainLevel=41 FilterCutoffEGReleaseTime=145 AmplitudeEGDecayTime=150 AmplitudeEGSustainLevel=140 "
    "AmplitudeEGReleaseTime=145 Osc2OscillatorWave=2 Osc2OscillatorPitch=554 Osc2OscillatorOutLevel=330 Osc3OscillatorWave=4 "
    "Osc3OscillatorOutLevel=0 Filter1FilterCutoff=636 Filter1FilterCutoffEGDepth=257 Filter1FilterResonance=50 "
    "Filter2FilterControllerBox2Switch=0 Filter2FilterControllerBox4Switch=0 PartPortamentoSwitch=0 PartAssignableKnob2Value=1023 "
    "PartAssignableKnob3Value=473 PartAssignableKnob4Value=0 PartFEGDepth=65 PartFilterCutoffFrequency=63 ZoneZoneOctaveShift=61 ZoneZoneNoteLimitLow=84 ZoneZoneNoteLimitHigh=96 "
    "Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetDestination=143 Box1ControllerSetPolarity=1 "
    "Box2ControllerSetSwitch=1 Box2ControllerSetSource=9 Box2ControllerSetDestination=140 Box2ControllerSetPolarity=1 "
    "Box3ControllerSetSource=10 Box3ControllerSetDestination=142 Box3ControllerSetPolarity=1 Box4ControllerSetSource=11 "
    "Box4ControllerSetDestination=143 Box4ControllerSetPolarity=1 Box4ControllerSetRatio=212";

static Patch probeA() { Patch p; char e[200]; if (!readPatch(kProbeA, p, e, sizeof e)) { std::printf("probeA: %s\n", e); std::exit(1); } return p; }

// The pattern (keys 84 88 91 88, sixteenths at 84 BPM, gate 0.9), split at every event; 3 s at 48 kHz.
static std::vector<float> renderProbeA(int interleave, uint32_t seed, uint32_t blockFrames) {
    auto core = std::make_unique<Core>();
    core->prepare(48000); core->load(0, probeA()); core->setGain(0, 1, 0); core->seed(seed); core->setInterleave(interleave);
    const double step = 60.0 / 84 / 4 * 48000;
    const int keys[] = {84, 88, 91, 88};
    struct Ev { long at; bool on; uint32_t v; int key; };
    std::vector<Ev> ev;
    for (int k = 0; k * step < 3 * 48000; ++k) {
        ev.push_back({long(k * step), true, uint32_t(k % 8), keys[k % 4]});
        ev.push_back({long(k * step + 0.9 * step), false, uint32_t(k % 8), 0});
    }
    std::vector<float> l(3 * 48000), r(3 * 48000);
    long at = 0; size_t e = 0;
    while (at < long(l.size())) {
        long next = std::min<long>(long(l.size()), (at / long(blockFrames) + 1) * long(blockFrames));
        if (e < ev.size() && ev[e].at < next) next = ev[e].at;
        if (next > at) core->render(&l[size_t(at)], &r[size_t(at)], uint32_t(next - at));
        at = next;
        while (e < ev.size() && ev[e].at <= at) {
            if (ev[e].on) { core->kill(ev[e].v); core->noteOn(ev[e].v, 0, 0, ev[e].key, 0.8f); } else core->noteOff(ev[e].v);
            ++e;
        }
    }
    l.insert(l.end(), r.begin(), r.end());
    return l;
}

static void testText() {
    Patch p = probeA(), q;
    char buf[4096];
    const size_t n = writePatch(p, buf, sizeof buf);
    CHECK(n < sizeof buf && std::strlen(buf) == n);
    char err[100];
    CHECK(readPatch(buf, q, err, sizeof err) && q == p);
    CHECK(!readPatch("vcore-patch/1 Filter1FilterCutoff=2000", q, err, sizeof err));  // out of range
    CHECK(!readPatch("vcore-patch/1 NoSuchThing=1", q, err, sizeof err));
    CHECK(!readPatch("Filter1FilterCutoff=1", q, err, sizeof err));                    // no header
    CHECK(writePatch(Patch(), buf, sizeof buf) == std::strlen("vcore-patch/1"));    // defaults: header only
}

// Probe A with its decay box off and the decay at 176: covered whatever the decay law's range (the generic checks below).
static Patch plain() { Patch p = probeA(); p.set(Box2ControllerSetSwitch, 0); p.set(FilterCutoffEGDecayTime, 176); return p; }

static void testCoverage() {
    Gap g[64];
    const uint32_t n = notCovered(probeA(), g, 64);
    CHECK(n == 0);  // its decay box takes 117 to 244 (the "hold"): measured
    CHECK(notCovered(plain(), g, 64) == 0);
    Patch p = plain();
    p.set(Filter1FilterResonance, 200); p.set(Filter1FilterCutoff, 500);
    CHECK(notCovered(p, g, 64) == 0);  // resonance law measured at cutoffs 400-800
    p.set(Filter1FilterCutoff, 900);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterResonance);
    p.set(Filter1FilterCutoff, 1000);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterResonance);  // the cutoff law itself reaches 1023
    p = plain(); p.set(Osc3OscillatorOutLevel, 50); p.set(Unison, 1);  // oscillator 3 under 90: no measured level law
    CHECK(notCovered(p, g, 64) == 2);
    p = plain(); p.set(Osc1OscillatorPitchLFODepth, 300); p.set(PitchLFOWave, 3);
    CHECK(notCovered(p, g, 64) == 0 && describe(p).lfo[0]);  // the pitch LFO: measured laws
    p.set(PitchLFOSpeed, 330);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == PitchLFOSpeed);  // audio rate: not built
    p = plain(); p.set(Box1ControllerSetRatio, 0); p.set(Box1ControllerSetPolarity, 0); p.set(PartAssignableKnob1Value, 1023);
    // the depth with its boxes under 96 (-192 steps), on filter 1 and on filter 2 (its box switch is on)
    CHECK(notCovered(p, g, 64) == 2 && g[0].param == Filter1FilterCutoffEGDepth && g[1].param == Filter2FilterCutoffEGDepth);
    p.set(Filter2FilterControllerBox1Switch, 0);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterCutoffEGDepth);
    p.set(Box1ControllerSetRatio, 255);  // past +192 steps it stops at 448, as the reference; the EG's peak then takes resonance 50 to the top
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterResonance);
    p.set(Filter1FilterResonance, 32);
    CHECK(notCovered(p, g, 64) == 0);
    p = plain(); p.set(Filter1FilterCutoffLFODepth, 280); p.set(FilterCutoffLFOWave, 3);
    CHECK(notCovered(p, g, 64) == 0 && describe(p).lfo[1]);  // the filter LFO: depth verified to 40 steps
    p.set(Filter1FilterCutoffLFODepth, 300);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterCutoffLFODepth);
    p = plain(); p.set(VoltageDrift, 65);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == VoltageDrift);
    // live sources: a mod wheel box on the cutoff reaches the whole box range (-24: 620 down to 236; the cutoff law is measured
    // 0-1023); aftertouch takes the 7-bit map (va-round7 A3P); an unmeasured curve is reported
    p = plain(); p.set(Box3ControllerSetSwitch, 1); p.set(Box3ControllerSetSource, 1); p.set(Box3ControllerSetPolarity, 0); p.set(Box3ControllerSetRatio, 104);
    p.set(Filter2FilterControllerBox3Switch, 0); p.set(Filter1FilterResonance, 0);
    CHECK(notCovered(p, g, 64) == 0);
    p.set(Box3ControllerSetRatio, 136);  // +8: 620 to 747
    CHECK(notCovered(p, g, 64) == 0);
    p.set(Box3ControllerSetSource, 2);
    CHECK(notCovered(p, g, 64) == 0);
    p.set(Box3ControllerSetSource, 1); p.set(Box3ControllerSetCurveType, 5);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetCurveType);
    p.set(Box3ControllerSetCurveType, 0); p.set(Box3ControllerSetDestination, 150);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetDestination);
    p.set(Box3ControllerSetRatio, 128);  // ratio 0 drives nothing, whatever the destination
    CHECK(notCovered(p, g, 64) == 0);
    CHECK(keySounds(probeA(), 84, 100) && keySounds(probeA(), 96, 1) && !keySounds(probeA(), 60, 100) && !keySounds(probeA(), 97, 100));
    const PlanDesc d = describe(probeA());
    CHECK(d.osc[0] == Wave::Saw && d.osc[1] == Wave::Saw && d.osc[2] == Wave::Off && d.filter[0] == 1 && d.filter[1] == 5 && d.count == 5);  // the square of width 0.5: a table
    CHECK(estimateCost(d).voiceBlock > 5000 && estimateCost(d).voiceBlock < 15000 && estimateCost(d).rest < 4000);
}

// Oscillator out level against the reference (one oscillator in probe A, key 84): 330 is 7.6 dB under 511
// (the earlier table gave 2.6 dB), 160 is 20.2 dB under.
static void testOscLevel() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    p.set(Osc1OscillatorOutLevel, 330); p.set(Osc2OscillatorOutLevel, 160); core->compile(s, p);
    CHECK(std::fabs(20 * std::log10(s.osc[0]) + 7.59) < 0.05 && std::fabs(20 * std::log10(s.osc[1]) + 20.15) < 0.05);
}

// The cutoff against the reference by its sound: a saw at key 60 through LPF24 (resonance 0, drift 0, 44.1 kHz), harmonics h1-h12
// in dB re h1 over 0.5-0.95 s, the engine against the reference's (va-round4: the static ladder of run filter-eg-sustain s1, and K3's
// part offsets held at full EG sustain), RMS over the harmonics within 80 dB of h1. The earlier law (noise through the filter)
// put 636 at 597 Hz where the reference sounds at 686: 3-9 dB off on h3-h12.
static double cutoffMiss(const char* extra, const float* ref) {
    auto core = std::make_unique<Core>(); core->prepare(44100);
    Patch p; char e[200];
    std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGAttackTime=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 "
                                "Filter2FilterType=0 Osc1OscillatorWave=0 Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=1 "
                                "Filter1FilterResonance=0 FilterCutoffEGAttackTime=0 FilterCutoffEGDecayTime=255 FilterCutoffEGSustainLevel=511 ") + extra;
    if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("patch: %s\n", e); return 99; }
    core->load(0, p); core->setGain(0, 1, 0); core->noteOn(0, 0, 0, 60, 0.8f);
    std::vector<float> L(44100), R(44100);
    core->render(L.data(), R.data(), uint32_t(L.size()));
    const int a = 22050, n = 19845;  // 0.5-0.95 s
    const double f0 = 440 * std::exp2(-9 / 12.0) / 44100, pi = 3.14159265358979;
    double h[12], d = 0; int used = 0;
    for (int k = 0; k < 12; ++k) {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * pi * i / n), ph = 2 * pi * (k + 1) * f0 * (a + i); re += L[a + i] * w * std::cos(ph); im += L[a + i] * w * std::sin(ph); }
        h[k] = 10 * std::log10(re * re + im * im + 1e-30);
    }
    for (int k = 1; k < 12; ++k) if (ref[k] > -80) { const double x = (h[k] - h[0]) - ref[k]; d += x * x; ++used; }
    return std::sqrt(d / used);
}
static void testCutoffAgainstReference() {
    static const float k500[] = {0.00f, -27.52f, -44.72f, -57.06f, -66.68f, -74.55f, -81.21f, -86.97f, -92.06f, -96.60f, -100.70f, -104.47f};
    static const float k636[] = {0.00f, -8.48f, -18.14f, -28.35f, -37.16f, -44.62f, -51.02f, -56.61f, -61.58f, -66.03f, -70.07f, -73.77f};
    static const float k700[] = {0.00f, -6.94f, -12.02f, -17.37f, -23.51f, -29.74f, -35.52f, -40.75f, -45.46f, -49.73f, -53.63f, -57.21f};
    static const float kFeg72[] = {0.00f, -6.71f, -11.28f, -15.42f, -19.97f, -25.02f, -30.14f, -35.00f, -39.49f, -43.62f, -47.40f, -50.90f};
    static const float kCut72[] = {0.00f, -6.50f, -10.74f, -14.18f, -17.43f, -20.86f, -24.62f, -28.55f, -32.47f, -36.22f, -39.75f, -43.06f};
    static const float kCut60[] = {0.00f, -16.84f, -32.71f, -44.67f, -54.10f, -61.86f, -68.46f, -74.18f, -79.24f, -83.76f, -87.85f, -91.59f};
    const struct { const char* extra; const float* ref; } cases[] = {
        {"Filter1FilterCutoff=500", k500}, {"Filter1FilterCutoff=636", k636}, {"Filter1FilterCutoff=700", k700},
        {"Filter1FilterCutoff=636 PartFEGDepth=72", kFeg72}, {"Filter1FilterCutoff=636 PartFilterCutoffFrequency=72", kCut72},
        {"Filter1FilterCutoff=636 PartFilterCutoffFrequency=60", kCut60}};
    for (const auto& c : cases) {
        const double m = cutoffMiss(c.extra, c.ref);
        std::printf("cutoff against the reference, %s: %.2f dB RMS\n", c.extra, m);
        CHECK(m < 1.0);
    }
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    core->compile(s, p);
    CHECK(p[Filter1FilterCutoff] == 1023 && std::exp2(s.flt[0].cut / 12) > 20000);  // the default is open
}

// The absolute level at part volume 127 (the host applies the part's volume): one held saw (key 60, out level 511, filters thru,
// amplitude level 511) reads -20.150 dB (power of both channels, 0.5-1.5 s; va-round5 V1, three takes within 0.01 dB; at part
// volume 100 -24.27 dB, where the earlier calibration was fitted).
static void testCalibration() {
    Patch p; char e[200];
    readPatch("vcore-patch/1 VoltageDrift=0 AmplitudeEGAttackTime=0 AmplitudeLevel=511 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 "
              "Osc1OscillatorWave=0 Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=0 Filter2FilterType=0", p, e, sizeof e);
    auto core = std::make_unique<Core>(); core->prepare(44100); core->load(0, p); core->setGain(0, 1, 0); core->noteOn(0, 0, 0, 60, 100.f / 127);
    std::vector<float> L(66150), R(L.size()); core->render(L.data(), R.data(), uint32_t(L.size()));
    double sum = 0; for (size_t i = 22050; i < 66150; ++i) sum += 0.5 * (double(L[i]) * L[i] + double(R[i]) * R[i]);
    const double db = 10 * std::log10(sum / 44100);
    std::printf("level at part volume 127: %.3f dB (reference -20.150)\n", db);
    CHECK(std::fabs(db + 20.150) < 0.05);
}

// The filter types against the reference (va-round5 F1): a saw of 32.7 Hz (key 36, 16') held through one filter, each harmonic's
// level re the same saw through thru (dB), at harmonics 1, 2, 4, ... 512; the engine renders the same at 44.1 kHz. The earlier
// engine had no LPF18 / LPF12 / LPF6 (thru), its LPF24 law was 2-4 dB off at 840-900 and its HPF24 a fixed stand-in.
static void filterResponse(const char* extra, const int* K, int n, double* out, int key = 36, int octave = 2, int vel = 100) {
    double ref[16], h[16];
    auto run = [&](const char* x, double* r) {
        std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGAttackTime=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 "
                                    "FilterCutoffEGAttackTime=0 FilterCutoffEGDecayTime=255 FilterCutoffEGSustainLevel=511 Osc1OscillatorWave=0 "
                                    "Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 "
                                    "Filter1FilterType=0 Filter2FilterType=0 Osc1OscillatorOctave=") + std::to_string(octave) + " " + x;
        Patch p; char e[200]; if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("filter: %s\n", e); ++failures; return; }
        auto core = std::make_unique<Core>(); core->prepare(44100); core->load(0, p); core->setGain(0, 1, 0); core->noteOn(0, 0, 0, key, float(vel) / 127);
        std::vector<float> L(66150), R(L.size()); core->render(L.data(), R.data(), uint32_t(L.size()));
        const double f0 = 440 * std::exp2((key - 69) / 12.0 + octave - 3) / 44100, pi = 3.14159265358979;
        for (int k = 0; k < n; ++k) {
            double re = 0, im = 0;
            for (size_t i = 0; i < 44100; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * pi * double(i) / 44100), ph = 2 * pi * K[k] * f0 * double(22050 + i);
                re += L[22050 + i] * w * std::cos(ph); im += L[22050 + i] * w * std::sin(ph); }
            r[k] = 10 * std::log10(re * re + im * im + 1e-30);
        }
    };
    run("", ref); run(extra, h);
    for (int k = 0; k < n; ++k) out[k] = h[k] - ref[k];
}
static void testFilterTypes() {
    static const int K[] = {1, 2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
    static const float kLp18c660[] = {-0.03f, -0.10f, -0.40f, -1.49f, -5.02f, -9.21f, -13.35f, -20.74f, -26.84f, -36.20f, -43.11f, -52.89f, -59.59f, -68.06f, -72.64f};
    static const float kLp12c840[] = {-0.01f, -0.01f, -0.03f, -0.10f, -0.38f, -0.82f, -1.36f, -2.63f, -3.96f, -6.48f, -8.65f, -12.15f, -14.87f, -18.71f, -20.97f};
    static const float kLp24c900[] = {-0.00f, -0.01f, -0.01f, -0.03f, -0.10f, -0.21f, -0.36f, -0.76f, -1.25f, -2.41f, -3.86f, -7.95f, -12.70f, -19.82f, -23.49f};
    static const float kLp6c720[] = {-0.01f, -0.01f, -0.04f, -0.17f, -0.62f, -1.28f, -2.06f, -3.73f, -5.33f, -8.04f, -10.18f, -13.31f, -15.50f, -18.29f, -19.79f};
    static const float kHp24c600[] = {-98.50f, -74.50f, -50.72f, -27.79f, -11.01f, -8.15f, -7.37f, -6.81f, -6.60f, -6.45f, -6.39f, -6.35f, -6.34f, -6.33f, -6.32f};
    static const float kLp24c780r64[] = {-3.74f, -3.73f, -3.70f, -3.57f, -3.05f, -2.19f, -1.04f, 1.68f, 1.56f, -12.75f, -24.36f, -39.28f, -48.89f, -60.68f, -66.88f};
    static const float kLp12c600r128[] = {-5.77f, -5.63f, -5.09f, -2.47f, 3.95f, -11.47f, -18.22f, -26.31f, -31.63f, -38.83f, -43.80f, -50.58f, -55.15f, -60.88f, -63.94f};
    const struct { const char* extra; const float* ref; } cases[] = {
        {"Filter1FilterType=2 Filter1FilterCutoff=660", kLp18c660}, {"Filter1FilterType=3 Filter1FilterCutoff=840", kLp12c840},
        {"Filter1FilterType=1 Filter1FilterCutoff=900", kLp24c900}, {"Filter1FilterType=4 Filter1FilterCutoff=720", kLp6c720},
        {"Filter2FilterType=5 Filter2FilterCutoff=600", kHp24c600}, {"Filter1FilterType=1 Filter1FilterCutoff=780 Filter1FilterResonance=64", kLp24c780r64},
        {"Filter1FilterType=3 Filter1FilterCutoff=600 Filter1FilterResonance=128", kLp12c600r128}};
    for (const auto& c : cases) {
        double d[15], worst = 0;
        filterResponse(c.extra, K, 15, d);
        for (int k = 0; k < 15; ++k) if (c.ref[k] > -70) worst = std::fmax(worst, std::fabs(d[k] - c.ref[k]));
        std::printf("filter against the reference, %s: worst harmonic %.2f dB\n", c.extra, worst);
        CHECK(worst < 0.3);
    }
    // key follow and velocity (F2: LPF24, an 8.2 Hz-based saw (64') at the key, against thru at the same key and velocity)
    static const int K2[] = {1, 2, 4, 8, 16, 32, 48, 64, 96, 128};
    static const float kKf4k84[] = {-0.02f, -0.05f, -0.20f, -0.73f, -2.30f, -7.07f, -14.24f, -20.58f, -28.95f, -33.40f};
    static const float kKf2k36[] = {-0.00f, -0.01f, -0.03f, -0.08f, -0.30f, -1.11f, -2.56f, -5.37f, -14.39f, -23.09f};
    static const float kCv384v32[] = {-1.81f, -11.58f, -33.30f, -56.90f, -80.90f, -103.98f, -121.30f, -132.76f, -123.30f, -127.11f};
    static const float kCv192v96[] = {-0.10f, -0.39f, -1.50f, -9.02f, -29.73f, -53.13f, -67.07f, -77.02f, -90.87f, -100.78f};
    static const float kEv511v64[] = {-0.02f, -0.07f, -0.25f, -0.92f, -4.06f, -19.74f, -32.72f, -42.27f, -55.90f, -65.56f};
    static const float kEv384v1[] = {-0.02f, -0.07f, -0.28f, -1.02f, -4.71f, -21.54f, -34.65f, -44.24f, -57.91f, -67.58f};
    const struct { const char* extra; int key, vel; const float* ref; double tol; } cases2[] = {
        {"Filter1FilterType=1 Filter1FilterCutoff=700 Filter1FilterCutoffKeyFollow=4", 84, 100, kKf4k84, 0.3},
        {"Filter1FilterType=1 Filter1FilterCutoff=700 Filter1FilterCutoffKeyFollow=2", 36, 100, kKf2k36, 0.3},
        {"Filter1FilterType=1 Filter1FilterCutoff=700 Filter1FilterCutoffVelocitySensitivity=384", 60, 32, kCv384v32, 1.5},
        {"Filter1FilterType=1 Filter1FilterCutoff=700 Filter1FilterCutoffVelocitySensitivity=192", 60, 96, kCv192v96, 1.5},
        {"Filter1FilterType=1 Filter1FilterCutoff=540 Filter1FilterCutoffEGDepth=300 Filter1FilterCutoffEGDepthVelocitySensitivity=511", 60, 64, kEv511v64, 0.5},
        {"Filter1FilterType=1 Filter1FilterCutoff=540 Filter1FilterCutoffEGDepth=300 Filter1FilterCutoffEGDepthVelocitySensitivity=384", 60, 1, kEv384v1, 0.5}};
    for (const auto& c : cases2) {
        double d[10], worst = 0;
        filterResponse(c.extra, K2, 10, d, c.key, 0, c.vel);
        for (int k = 0; k < 10; ++k) if (c.ref[k] > -70) worst = std::fmax(worst, std::fabs(d[k] - c.ref[k]));
        std::printf("filter against the reference, %s at key %d velocity %d: worst harmonic %.2f dB\n", c.extra, c.key, c.vel, worst);
        CHECK(worst < c.tol);
    }
}

// Energy off the harmonics of one oscillator alone (filters thru, drift 0) at 44.1 kHz, dB re the total:
// each harmonic projected out of a Hann-windowed 0.5 s. The reference: -98 (saw) / -101 (pulse) dB at 1047 Hz.
static double aliasDb(int wave, int key) {
    auto core = std::make_unique<Core>(); core->prepare(44100);
    Patch p; char e[200];
    readPatch("vcore-patch/1 Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=0 "
              "Filter2FilterType=0 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255", p, e, sizeof e);
    p.set(Osc1OscillatorWave, wave);
    core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> l(44100), r(44100);
    core->noteOn(0, 0, 0, key, 1.f); core->render(l.data(), r.data(), 44100);
    const size_t n = 22050, o = 11025; const double pi = 3.14159265358979;
    double f0 = 440 * std::exp2((key - 69) / 12.0) / 44100;
    for (int it = 0; it < 3; ++it) {  // the core's pitch is within a cent of the nominal: refine f0 from the fundamental's phase
        double ph[2];
        for (int hh = 0; hh < 2; ++hh) {
            double c = 0, s = 0;
            for (size_t i = 0; i < n / 2; ++i) { const double a = 2 * pi * f0 * double(i), wv = 0.5 - 0.5 * std::cos(2 * pi * double(i) / double(n / 2));
                c += l[o + hh * n / 2 + i] * wv * std::cos(a); s += l[o + hh * n / 2 + i] * wv * std::sin(a); }
            ph[hh] = std::atan2(s, c);
        }
        double d = ph[0] - ph[1] - 2 * pi * f0 * double(n / 2); d -= 2 * pi * std::round(d / (2 * pi));
        f0 += d / (2 * pi * double(n / 2));
    }
    std::vector<double> x(n), w(n);
    double tot = 0;
    for (size_t i = 0; i < n; ++i) { w[i] = 0.5 - 0.5 * std::cos(2 * pi * double(i) / double(n)); x[i] = l[o + i] * w[i]; tot += x[i] * x[i]; }
    for (int h = 1; h * f0 < 0.5; ++h) {
        double c = 0, s = 0, cc = 0, ss = 0, cs = 0;
        for (size_t i = 0; i < n; ++i) { const double a = 2 * pi * h * f0 * double(i + o), u = std::cos(a) * w[i], v = std::sin(a) * w[i];
            c += x[i] * u; s += x[i] * v; cc += u * u; ss += v * v; cs += u * v; }
        const double det = cc * ss - cs * cs, A = (c * ss - s * cs) / det, B = (s * cc - c * cs) / det;
        for (size_t i = 0; i < n; ++i) { const double a = 2 * pi * h * f0 * double(i + o); x[i] -= (A * std::cos(a) + B * std::sin(a)) * w[i]; }
    }
    double res = 0; for (double v : x) res += v * v;
    return 10 * std::log10(res / tot);
}

static void testAlias() {
    const double saw = aliasDb(0, 84), pulse = aliasDb(2, 84);
    std::printf("alias at 1047 Hz: saw %.1f dB, pulse %.1f dB\n", saw, pulse);
    CHECK(saw < -85 && pulse < -85);
}

static void testInterleaveIdentical() {
    const auto a = renderProbeA(1, 7, 128), b = renderProbeA(2, 7, 128);
    CHECK(a.size() == b.size() && !std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
}

static void testVoiceLifetime() {
    auto core = std::make_unique<Core>();
    core->prepare(48000); core->load(0, probeA()); core->setGain(0, 1, 0);
    std::vector<float> l(48000), r(48000);
    core->render(l.data(), r.data(), 37);
    core->noteOn(3, 0, 0, 84, 1.f);  // starts on frame 37 exactly
    core->render(&l[37], &r[37], 48000 - 37);  // one second: the amplitude EG reaches its sustain (140/511)
    bool silentBefore = true; for (int i = 0; i < 37; ++i) silentBefore &= l[size_t(i)] == 0.f;
    bool soundsAt = false; for (int i = 38; i < 44; ++i) soundsAt |= l[size_t(i)] != 0.f;  // the filters' sections delay a sample each
    CHECK(silentBefore && soundsAt);
    CHECK(core->voice(3).sounding && !core->voice(3).releasing && core->voiceCount() == 1);
    core->noteOff(3);
    CHECK(core->voice(3).releasing);
    uint32_t frames = 0;
    while (core->voice(3).sounding && frames < 48000 * 3) { std::vector<float> a(128), b(128); core->render(a.data(), b.data(), 128); frames += 128; }
    // the release (raw 145 from sustain 140, the low-release law) ends at exactly zero about 1.03 s after the note-off
    CHECK(!core->voice(3).sounding && frames > 48000 * 9 / 10 && frames < 48000 * 12 / 10);
    core->noteOn(5, 0, 0, 90, 1.f); core->noteOn(6, 0, 0, 91, 1.f);
    core->choke(5, 144);
    { std::vector<float> a(256), b(256); core->render(a.data(), b.data(), 140); CHECK(core->voice(5).sounding); core->render(a.data(), b.data(), 8); }
    CHECK(!core->voice(5).sounding && core->voice(6).sounding);
    core->kill(6);
    CHECK(!core->voice(6).sounding && core->voiceCount() == 0);
}

// Voltage drift moves: one saw (key 60, filters thru) held 12 s; its pitch in 1 s windows (rising zero
// crossings). At drift 64 the reference's pitch moves with a within-voice sd of about 1.7 cents (30 s holds);
// at drift 0 it stays.
static double pitchRange(int drift, uint32_t seed) {
    auto core = std::make_unique<Core>(); core->prepare(48000); core->seed(seed);
    Patch p; char e[200];
    readPatch("vcore-patch/1 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=0 Filter2FilterType=0 "
              "AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255", p, e, sizeof e);
    p.set(VoltageDrift, drift); core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> l(48000 * 12), r(48000 * 12);
    core->noteOn(0, 0, 0, 60, 1.f); core->render(l.data(), r.data(), uint32_t(l.size()));
    double lo = 1e9, hi = -1e9;
    for (size_t w = 1; w < 12; ++w) {
        double first = -1, last = 0; int n = 0;
        for (size_t i = w * 48000; i < (w + 1) * 48000; ++i)
            if (l[i - 1] < 0 && l[i] >= 0) { const double t = double(i - 1) + l[i - 1] / (l[i - 1] - l[i]); if (first < 0) first = t; last = t; ++n; }
        const double cents = 1200 * std::log2((n - 1) / (last - first) * 48000 / 261.6256);
        lo = std::fmin(lo, cents); hi = std::fmax(hi, cents);
    }
    return hi - lo;
}

static void testDrift() {
    double moving = 0;
    for (uint32_t s = 1; s <= 4; ++s) moving += pitchRange(64, s) / 4;
    const double still = pitchRange(0, 1);
    std::printf("drift: pitch range over 1 s windows: %.2f cents at 64 (4 voices), %.3f at 0\n", moving, still);
    CHECK(moving > 1.5 && moving < 15 && still < 0.1);
}

// The release law is chosen by the level at the note-off: 0.437 and under release slowly and end about 1.03 s
// later (release 145), 0.449 and over fast, ending about 0.56 s later. Held at sustain 220 (0.4305): slow.
static void testReleaseThreshold() {
    for (int sus : {220, 232}) {
        auto core = std::make_unique<Core>(); core->prepare(48000);
        Patch p = probeA(); p.set(AmplitudeEGSustainLevel, sus); core->load(0, p); core->setGain(0, 1, 0);
        std::vector<float> a(128), b(128);
        core->noteOn(0, 0, 0, 84, 1.f);
        for (int i = 0; i < 48000 / 128; ++i) core->render(a.data(), b.data(), 128);
        core->noteOff(0);
        uint32_t frames = 0;
        while (core->voice(0).sounding && frames < 48000 * 3) { core->render(a.data(), b.data(), 128); frames += 128; }
        const double secs = frames / 48000.0;
        std::printf("release from sustain %d (%.4f): ends after %.3f s\n", sus, sus / 511.0, secs);
        CHECK(sus == 220 ? secs > 0.9 : secs < 0.7);
    }
}

// A long envelope segment keeps its law's time constant: the amplitude decay at raw 240 (5.9 s) toward sustain 0 is at
// e^(-1 / 5.9) after 1 s (the control step's coefficient was 2e-4 off, which made it 0.3 per second fast: e^(-1.5)).
static void testLongSegment() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Patch p = probeA(); p.set(AmplitudeEGDecayTime, 240); p.set(AmplitudeEGSustainLevel, 0); p.set(AmplitudeEGAttackTime, 0);
    core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> a(128), b(128);
    core->noteOn(0, 0, 0, 84, 1.f);
    for (int i = 0; i < 8; ++i) core->render(a.data(), b.data(), 128);  // past the attack
    const float l0 = core->voice(0).level;
    for (int i = 0; i < 375; ++i) core->render(a.data(), b.data(), 128);  // 1 s
    const double r = core->voice(0).level / l0, want = std::exp(-1 / 5.895606);
    std::printf("decay 240 over 1 s: %.4f (law %.4f)\n", r, want);
    CHECK(std::fabs(r / want - 1) < 0.005);
}

// The controller boxes (va-round3 R1): a box's output (c + off - 1024) x ratio / 8 within +-8192; a destination's boxes summed
// and clamped to +-8192; the destination moves by the total x span / 8192 raw steps, toward zero, within its range.
static void testBoxLaw() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; float src[kSources] = {}; float knob[8] = {}; int16_t raw[kDests];
    Patch p; char e[200];
    readPatch("vcore-patch/1 Filter1FilterCutoff=500 Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=142 "
              "Box1ControllerSetRatio=144 Box1ControllerSetPolarity=0", p, e, sizeof e);
    core->compile(s, p);
    src[1] = 1023; boxRaw(s, src, knob, raw);
    CHECK(raw[DCutoff] == 755);  // 500 + 2048 x 1023 / 8192 = 755.75: the reference sounds as the static 755 (0.00 dB), not 756 (0.11)
    src[1] = 0; boxRaw(s, src, knob, raw);
    CHECK(raw[DCutoff] == 500);
    // two +64 boxes from the mod wheel on the pitch LFO's depth from 0: the sum stops at 8192, so +247 steps (no vibrato)
    readPatch("vcore-patch/1 Osc1OscillatorPitchLFODepth=0 Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=124 "
              "Box1ControllerSetRatio=192 Box2ControllerSetSwitch=1 Box2ControllerSetSource=1 Box2ControllerSetDestination=124 Box2ControllerSetRatio=192", p, e, sizeof e);
    core->compile(s, p);
    src[1] = 516; boxRaw(s, src, knob, raw);
    CHECK(raw[DPitchDepth1] == 247);
    src[1] = 258; boxRaw(s, src, knob, raw);
    CHECK(raw[DPitchDepth1] == 124);
    // bipolar from a knob at its middle: no move; the filter EG depth stops at +192 steps
    readPatch("vcore-patch/1 Filter1FilterCutoffEGDepth=400 Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetDestination=143 "
              "Box1ControllerSetRatio=192 Box1ControllerSetPolarity=1 PartAssignableKnob1Value=512", p, e, sizeof e);
    core->compile(s, p);
    knob[0] = 512 / 1023.f; boxRaw(s, src, knob, raw);
    CHECK(raw[DFegDepth] == 144);
    knob[0] = 1; boxRaw(s, src, knob, raw);
    CHECK(raw[DFegDepth] == 192);
    // live: the mod wheel moves the cutoff in the render (the core took knob boxes on two destinations only before)
    auto bright = [&](float wheel) {
        Patch q; readPatch("vcore-patch/1 Filter1FilterCutoff=400 Filter1FilterResonance=0 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 VoltageDrift=0 AmplitudeEGSustainLevel=511 "
                           "AmplitudeEGDecayTime=255 Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=142 Box1ControllerSetRatio=160", q, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, q); c->setGain(0, 1, 0);
        c->setController(0, 1, wheel);
        std::vector<float> a(4800), b(4800);
        c->noteOn(0, 0, 0, 48, 1.f); c->render(a.data(), b.data(), 4800); c->render(a.data(), b.data(), 4800);
        double d = 0; for (size_t i = 1; i < a.size(); ++i) d += double(a[i] - a[i - 1]) * (a[i] - a[i - 1]);
        return d;
    };
    const double lo = bright(0), hi = bright(1);
    std::printf("mod wheel on the cutoff: high-passed energy x%.0f at full\n", hi / lo);
    CHECK(hi > 10 * lo);
    // va-round5 C1: the mod wheel through a +32 box on cutoff 400 sounds as the static cutoff of the 7-bit map 8 v (v <= 64), then
    // 512 + (v - 64) 512 / 63: at 64 the static 655 (round(64 x 1023 / 127) = 516 would give 657, 1 % off in F), at 96 the static 785
    auto render = [&](const char* extra, float wheel) {
        std::string t = std::string("vcore-patch/1 Filter1FilterType=3 Filter1FilterResonance=0 Filter2FilterType=0 Osc2OscillatorOutLevel=0 "
                                    "Osc3OscillatorOutLevel=0 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 ") + extra;
        Patch q; readPatch(t.c_str(), q, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(44100); c->load(0, q); c->setGain(0, 1, 0); c->setController(0, 1, wheel);
        std::vector<float> a(8820), b(8820);
        c->noteOn(0, 0, 0, 48, 1.f); c->render(a.data(), b.data(), 8820);
        return a;
    };
    const char* kWheelBox = "Filter1FilterCutoff=400 Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=142 Box1ControllerSetRatio=160 ";
    for (const auto& w : {std::make_pair(64, "Filter1FilterCutoff=655"), std::make_pair(96, "Filter1FilterCutoff=785")}) {
        const auto a = render(kWheelBox, float(w.first) / 127), b = render(w.second, 0);
        double d = 0, n = 0; for (size_t i = 4410; i < a.size(); ++i) { d += double(a[i] - b[i]) * (a[i] - b[i]); n += double(b[i]) * b[i]; }
        std::printf("mod wheel %d through the box against %s: %.1e relative\n", w.first, w.second, std::sqrt(d / n));
        CHECK(std::sqrt(d / n) < 1e-4);
    }
    // C1: three +1 boxes from the wheel at 127 on cutoff 600 sound as the static 647: the boxes are summed, then truncated once (15.98
    // steps each; truncating each first would give 645)
    readPatch("vcore-patch/1 Filter1FilterCutoff=600 Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=142 Box1ControllerSetRatio=129 "
              "Box2ControllerSetSwitch=1 Box2ControllerSetSource=1 Box2ControllerSetDestination=142 Box2ControllerSetRatio=129 "
              "Box3ControllerSetSwitch=1 Box3ControllerSetSource=1 Box3ControllerSetDestination=142 Box3ControllerSetRatio=129", p, e, sizeof e);
    core->compile(s, p);
    src[1] = 1024; boxRaw(s, src, knob, raw);
    CHECK(raw[DCutoff] == 647);
}

// The filter EG depth: a true zero at 256 (the earlier law put 1.4 semitones there), 0.5 semitone of the cutoff a step.
static void testFegDepthZero() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    p.set(Filter1FilterCutoffEGDepth, 256); core->compile(s, p);
    CHECK(s.flt[0].feg == 0.f);
    p.set(Filter1FilterCutoffEGDepth, 352); core->compile(s, p);
    CHECK(std::fabs(s.flt[0].feg - 48.f) < 0.01);  // 0.5 semitone a step (va-round5 F2)
}

// The filter EG's decay and release past 208 (va-round3 R2: 220 / 232 / 244 / 255 -> 2.61 / 4.28 / 7.24 / 12.15 s, one law for
// both) and the amplitude release under 112 (64 / 80 / 96 / 104 -> 4.2 / 6.3 / 12.1 / 17.4 ms).
static void testTimeLaws() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    auto tau = [](float l) { return -1 / (double(l) * 48000 * std::log(2.0)); };
    p.set(FilterCutoffEGDecayTime, 244); p.set(FilterCutoffEGReleaseTime, 232); p.set(AmplitudeEGReleaseTime, 80); core->compile(s, p);
    CHECK(std::fabs(tau(s.feg.ld) / 7.237 - 1) < 0.001 && std::fabs(tau(s.feg.lr) / 4.278 - 1) < 0.001);
    CHECK(std::fabs(tau(s.amp.lr) / 0.00629 - 1) < 0.001);
    // the filter EG's decay under 144 (va-round7 E1, the early part through the same analysis as the engine's): 110 / 120 / 133 read
    // 23.6 / 34.0 / 59.4 ms where the amplitude decay's table, used before, gave 21.7 / 31.2 / 55.9; and 117 and 100 reach coverage
    for (const auto& c : {std::pair<int, double>{110, 0.02399}, {120, 0.03414}, {133, 0.05998}}) {
        p.set(FilterCutoffEGDecayTime, c.first); core->compile(s, p);
        CHECK(std::fabs(tau(s.feg.ld) / c.second - 1) < 0.002);
    }
    Gap g[64]; Patch q = plain(); q.set(FilterCutoffEGDecayTime, 117);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(FilterCutoffEGDecayTime, 99);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == FilterCutoffEGDecayTime);
}

// The pitch LFO's depth between the knots 80 and 96 (va-round3 R1: the mod wheel's box landing on 86 and 92 steps):
// 150.0 and 184.6 cents, where the line between the old knots gave 162.6 and 188.1.
static void testPitchDepthKnots() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    p.set(Osc1OscillatorPitchLFODepth, 247 + 86); p.set(Osc2OscillatorPitchLFODepth, 247 - 92); core->compile(s, p);
    CHECK(std::fabs(s.lfoPitch[0] * 100 - 150.0) < 0.05 && std::fabs(s.lfoPitch[1] * 100 + 184.57) < 0.05);
}

// The waves against the reference (va-round4 W1): one oscillator alone, filters thru, harmonics h1-h12 in dB re the saw's h1 at
// the same key and level; the third oscillator's sine level as the first's; the pulse width's law at 0, 61 and 255; the width the
// oscillator's LFO moves (depth 160: +-32 raw steps, 0.1227 of the period, at 1.282 Hz).
static void harmonics(const char* extra, int key, double* h, int H, size_t from = 22050, size_t n = 22050, std::vector<float>* keep = nullptr) {
    std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter1FilterType=0 Filter2FilterType=0 "
                                "Osc1OscillatorOutLevel=0 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 ") + extra;
    Patch p; char e[200]; if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("wave: %s\n", e); ++failures; return; }
    auto core = std::make_unique<Core>(); core->prepare(44100); core->load(0, p); core->setGain(0, 1, 0); core->noteOn(0, 0, 0, key, 0.8f);
    std::vector<float> L(from + n), R(L.size());
    core->render(L.data(), R.data(), uint32_t(L.size()));
    const double f0 = 440 * std::exp2((key - 69) / 12.0) / 44100, pi = 3.14159265358979;
    for (int k = 0; k < H; ++k) {
        double re = 0, im = 0;
        for (size_t i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * pi * i / n), ph = 2 * pi * (k + 1) * f0 * double(from + i); re += L[from + i] * w * std::cos(ph); im += L[from + i] * w * std::sin(ph); }
        h[k] = 10 * std::log10(re * re + im * im + 1e-30);
    }
    if (keep) *keep = L;
}
static void testWaves() {
    static const float kSaw2[] = {3.46f, -4.86f, -8.10f, -10.90f, -12.75f, -14.46f, -15.77f, -17.00f, -18.02f, -18.99f, -19.83f, -20.63f};
    static const float kTri[] = {3.47f, -99.f, -15.60f, -99.f, -24.49f, -99.f, -30.36f, -99.f, -34.75f, -99.f, -38.27f, -99.f};
    static const float kSine[] = {5.25f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f, -99.f};
    static const float kPw0[] = {-24.08f, -24.05f, -24.05f, -24.05f, -24.06f, -24.08f, -24.10f, -24.13f, -24.16f, -24.19f, -24.23f, -24.27f};
    static const float kPw61[] = {2.78f, -0.02f, -6.04f, -27.82f, -11.95f, -9.61f, -12.82f, -27.86f, -18.02f, -14.18f, -16.28f, -27.93f};
    const struct { const char* extra; int key; const float* ref; } cases[] = {
        {"Osc1OscillatorWave=1 Osc1OscillatorOutLevel=511", 60, kSaw2}, {"Osc1OscillatorWave=3 Osc1OscillatorOutLevel=511", 60, kTri},
        {"Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511", 60, kSine}, {"Osc3OscillatorWave=4 Osc3OscillatorOutLevel=511", 60, kSine},
        {"Osc1OscillatorWave=2 Osc1OscillatorOutLevel=511 Osc1OscillatorPulseWidth=0", 48, kPw0},
        {"Osc1OscillatorWave=2 Osc1OscillatorOutLevel=511 Osc1OscillatorPulseWidth=61", 48, kPw61},
        {"Osc1OscillatorWave=2 Osc1OscillatorOutLevel=511 Osc1OscillatorPulseWidth=255", 48, kPw0}};
    for (const auto& c : cases) {
        double saw[12], h[12], worst = 0;
        harmonics("Osc1OscillatorWave=0 Osc1OscillatorOutLevel=511", c.key, saw, 1);
        harmonics(c.extra, c.key, h, 12);
        for (int k = 0; k < 12; ++k) {
            const double d = c.ref[k] > -90 ? (h[k] - saw[0]) - c.ref[k] : (h[k] - saw[0] < -80 ? 0 : 99);
            worst = std::fmax(worst, std::fabs(d));
        }
        std::printf("wave against the reference, %s at key %d: worst harmonic %.2f dB\n", c.extra, c.key, worst);
        CHECK(worst < 0.5);
    }
    std::vector<float> L; double dummy[1];
    harmonics("Osc1OscillatorWave=2 Osc1OscillatorOutLevel=511 Osc1OscillatorPulseWidthLFODepth=160 Osc1OscillatorLFOWave=3 Osc1OscillatorLFOSpeed=150 "
              "Osc1OscillatorLFOKeyOnReset=1", 48, dummy, 1, 0, 88200, &L);
    const double f0 = 440 * std::exp2(-21 / 12.0) / 44100, pi = 3.14159265358979;
    double lo = 1, hi = 0;
    for (size_t a = 4410; a + 1764 < L.size(); a += 441) {  // 40 ms windows: w from h2 / h1 = cos(pi w)
        double m[2];
        for (int k = 0; k < 2; ++k) { double re = 0, im = 0; for (size_t i = 0; i < 1764; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * pi * i / 1764), ph = 2 * pi * (k + 1) * f0 * double(a + i); re += L[a + i] * w * std::cos(ph); im += L[a + i] * w * std::sin(ph); } m[k] = std::sqrt(re * re + im * im); }
        const double w = std::acos(std::fmin(m[1] / m[0], 1.0)) / pi;
        lo = std::fmin(lo, w); hi = std::fmax(hi, w);
    }
    std::printf("pulse width LFO 160: width %.4f..%.4f (reference 0.5 - 0.1227 at the LFO's peaks)\n", lo, hi);
    CHECK(std::fabs(lo - (0.5 - 0.1227)) < 0.006 && hi > 0.49);
}

// Portamento against the reference (va-round4 P1: a sine gliding 60 -> 72 at time 49 runs 68.16 semitones a second, linearly;
// time mode 1 takes an octave's time whatever the interval; mode 2 starts at most 12 semitones away): a pulse under a 640 low-pass,
// the second note's pitch from the zero crossings, the time it is half way.
static double halfWay(const char* extra, int a, int b) {
    std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGReleaseTime=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 "
        "Osc1OscillatorWave=2 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterCutoff=640 Filter2FilterType=0 PortamentoMaster=1 ") + extra;
    Patch p; char e[200]; if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("porta: %s\n", e); ++failures; return 0; }
    auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> l(96000), r(l.size());
    core->noteOn(0, 0, 0, a, 0.8f);
    core->render(l.data(), r.data(), 24000);
    core->noteOff(0); core->render(&l[24000], &r[24000], 4800);
    core->noteOn(1, 0, 0, b, 0.8f);
    core->render(&l[28800], &r[28800], uint32_t(l.size() - 28800));
    double prev = -1, last = 0;
    for (size_t i = 28800; i + 1 < l.size(); ++i)
        if (l[i] < 0 && l[i + 1] >= 0) {
            const double z = i + l[i] / (l[i] - l[i + 1]);
            if (prev > 0) {
                const double key = 69 + 12 * std::log2(48000 / (z - prev) / 440);
                if ((key - a) / double(b - a) >= 0.5) return ((z + prev) / 2 - 28800) / 48000;
            }
            prev = z; last = z;
        }
    (void)last;
    return -1;
}
static void testPortamento() {
    const double t49 = halfWay("PartPortamentoTime=49", 60, 72), t80 = halfWay("PartPortamentoTime=80", 60, 72);
    const double m1 = halfWay("PartPortamentoTime=49 PartPortamentoTimeMode=1", 60, 84), m2 = halfWay("PartPortamentoTime=49 PartPortamentoTimeMode=2", 60, 84);
    const double off = halfWay("PartPortamentoTime=49 PortamentoMaster=0", 60, 72), fingered = halfWay("PartPortamentoTime=49 PartPortamentoMode=0", 60, 72);
    std::printf("portamento half way: time 49 %.4f s, 80 %.4f s, mode 1 (24 semitones) %.4f s, mode 2 %.4f s, off %.4f, fingered %.4f\n", t49, t80, m1, m2, off, fingered);
    CHECK(std::fabs(t49 - 6 / 68.16) < 0.004 && std::fabs(t80 - 6 / 17.76) < 0.01);  // reference: 88 ms and 338 ms
    CHECK(std::fabs(m1 - 0.5 * 12 / 68.16) < 0.006);                                 // half the octave's time, 24 semitones away
    CHECK(m2 >= 0 && m2 < 0.005);  // starts 12 below 84: half way to 84 from 60 (72) is passed at once
    CHECK(off >= 0 && off < 0.005 && fingered >= 0 && fingered < 0.005);  // no glide: master off; fingered with no key held
}
// A box on the portamento time (52) moves a glide under way (va-round7 A2: a glide 48 -> 60 at time 96 jumps to 60 when the wheel
// takes the time to 0 through a -64 box; at time 64 a +64 box at the wheel's top slows it to time 127's 0.12 semitone a second).
static double glideKeyAfter(int time, int ratio, double wheelAt, double at) {
    const std::string t = "vcore-patch/1 VoltageDrift=0 AmplitudeEGReleaseTime=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Osc1OscillatorWave=4 "
        "Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=0 Filter2FilterType=0 PortamentoMaster=1 PartPortamentoTime=" + std::to_string(time) +
        " Box1ControllerSetSwitch=1 Box1ControllerSetSource=1 Box1ControllerSetDestination=52 Box1ControllerSetRatio=" + std::to_string(ratio);
    Patch p; char e[200]; if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("porta box: %s\n", e); ++failures; return 0; }
    auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> l(size_t((at + 0.05) * 48000) + 4800, 0.f), r(l.size());
    core->noteOn(0, 0, 0, 48, 0.8f); core->render(l.data(), r.data(), 4800); core->noteOff(0); core->render(l.data(), r.data(), 4800);
    core->noteOn(1, 0, 0, 60, 0.8f);
    const size_t w = size_t(wheelAt * 48000);
    core->render(l.data(), r.data(), uint32_t(w)); core->setController(0, 1, 1.f); core->render(&l[w], &r[w], uint32_t(l.size() - w));
    std::vector<double> z;  // the key from the zero crossings over 20 ms from `at`
    for (size_t i = size_t(at * 48000); i < size_t((at + 0.02) * 48000); ++i) if (l[i] < 0 && l[i + 1] >= 0) z.push_back(i + l[i] / (l[i] - l[i + 1]));
    return z.size() < 2 ? 0 : 69 + 12 * std::log2(48000 * double(z.size() - 1) / (z.back() - z.front()) / 440);
}
static void testPortamentoBox() {
    const double jump = glideKeyAfter(96, 64, 0.2, 0.25), slow = glideKeyAfter(64, 192, 0.05, 0.5);
    std::printf("portamento time box during a glide 48 -> 60: to time 0 at 0.2 s -> key %.2f at 0.25 s; to 127 at 0.05 s -> key %.2f at 0.5 s\n", jump, slow);
    CHECK(std::fabs(jump - 60) < 0.05);
    CHECK(std::fabs(slow - (48 + 36.52 * 0.05 + 0.1219 * 0.46)) < 0.15);  // 1.8 semitones at time 64, then 0.12 a second
    // coverage: times 1-7 glide as 8 (va-round7 A3, an octave at 72 -> 84: 5-95 % in 27-44 ms, time 8 35-42 ms); a box on the time in
    // time mode 1 (the glide's rate fixed by its interval at its start) is not built
    Gap g[64]; Patch q; q.set(PortamentoMaster, 1); q.set(PartPortamentoTime, 4);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(Box1ControllerSetSwitch, 1); q.set(Box1ControllerSetSource, 1); q.set(Box1ControllerSetDestination, 52); q.set(PartPortamentoTimeMode, 1);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == PartPortamentoTime);
    q.set(PartPortamentoTimeMode, 0);
    CHECK(notCovered(q, g, 64) == 0);
}
// Mono (va-round4 P1 S_/U_): with portamento a note over a held key is legato (no retrigger) and its release goes back to the
// held key; without, every change retriggers the envelopes. Single key assignment: a key struck again takes its voice over and
// the first note-off lets that go without releasing it.
static void testMonoAndSingle() {
    auto run = [](const char* extra, float* afterLegato, bool* backAndSounding) {
        std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGDecayTime=150 AmplitudeEGSustainLevel=140 AmplitudeEGReleaseTime=0 "
                                    "PartMonoPolyMode=0 PortamentoMaster=1 PartPortamentoTime=49 ") + extra;
        Patch p; char e[200]; readPatch(t.c_str(), p, e, sizeof e);
        auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0);
        std::vector<float> l(48000), r(48000);
        core->noteOn(0, 0, 0, 60, 0.8f); core->render(l.data(), r.data(), 38400);
        const float before = core->voice(0).level;
        core->noteOn(1, 0, 0, 67, 0.8f); core->render(l.data(), r.data(), 480);
        *afterLegato = core->voice(1).level / before;
        core->noteOff(1); core->render(l.data(), r.data(), 480);
        *backAndSounding = core->voiceCount() == 1 && core->voice(0).sounding && !core->voice(0).releasing && !core->voice(1).sounding;
    };
    float legato, retrig; bool back1, back2;
    run("PartPortamentoSwitch=1", &legato, &back1); run("PartPortamentoSwitch=0", &retrig, &back2);
    std::printf("mono: level 10 ms after a legato note x%.2f with portamento, x%.2f without\n", legato, retrig);
    CHECK(std::fabs(legato - 1) < 0.02 && retrig > 2 && back1 && back2);
    Patch p; char e[200];
    readPatch("vcore-patch/1 PartKeyAssignMode=0 AmplitudeEGReleaseTime=0", p, e, sizeof e);
    auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> l(480), r(480);
    core->noteOn(0, 0, 0, 60, 0.8f); core->render(l.data(), r.data(), 480);
    core->noteOn(1, 0, 0, 60, 0.8f); core->render(l.data(), r.data(), 480);
    CHECK(core->voiceCount() == 1);
    core->noteOff(0); core->render(l.data(), r.data(), 480);
    CHECK(core->voiceCount() == 1 && !core->voice(1).releasing && !core->voice(0).sounding);
}

// Cross-build tolerance: every 100 ms window's RMS within 2e-4 of the stored value (relative), or
// 1e-7 absolute (-140 dBFS) where the signal is that quiet.
constexpr double kTolerance = 2e-4, kFloor = 1e-7;
static const double kStats[] = {
#include "probeA_stats.inc"
};

// The pitch LFO against the reference's measured laws (sine wave, speed 208 = 5.005 Hz, depth 300 = 69.7 cents, key-on
// reset at phase 0, delay 32 = 1.02 s): a saw at key 36 under a 45 Hz low-pass, its pitch per period from the zero crossings.
static std::vector<double> periodPitch(const char* extra, double seconds, std::vector<double>* times) {
    std::string text = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGReleaseTime=112 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 "
        "Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterCutoff=300 Filter2FilterType=0 Filter1FilterCutoffEGDepth=256 "
        "PitchLFOWave=3 PitchLFOSpeed=208 PitchLFOKeyOnReset=1 Osc1OscillatorPitchLFODepth=300 ") + extra;
    Patch p; char e[200]; if (!readPatch(text.c_str(), p, e, sizeof e)) { std::printf("lfo: %s\n", e); ++failures; return {}; }
    auto core = std::make_unique<Core>();
    core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0); core->seed(3);
    core->noteOn(0, 0, 0, 36, 0.8f);
    std::vector<float> l(size_t(seconds * 48000)), r(l.size());
    for (size_t at = 0; at < l.size(); at += 128) core->render(&l[at], &r[at], uint32_t(std::min<size_t>(128, l.size() - at)));
    std::vector<double> zc, out;
    for (size_t i = 4800; i + 1 < l.size(); ++i) if (l[i] < 0 && l[i + 1] >= 0) zc.push_back(i + l[i] / (l[i] - l[i + 1]));
    const double f0 = 440 * std::pow(2.0, (36 - 69) / 12.0);
    for (size_t i = 1; i < zc.size(); ++i) { out.push_back(1200 * std::log2(48000 / (zc[i] - zc[i - 1]) / f0)); if (times) times->push_back(zc[i] / 48000); }
    return out;
}
static void testPitchLfo() {
    std::vector<double> t;
    const std::vector<double> c = periodPitch("", 3.0, &t);
    double lo = 1e9, hi = -1e9; int crossings = 0;
    for (size_t i = 0; i < c.size(); ++i) { lo = std::min(lo, c[i]); hi = std::max(hi, c[i]); if (i && c[i - 1] < 0 && c[i] >= 0) ++crossings; }
    std::printf("pitch LFO: %.1f..%.1f cents, %d upward crossings in %.2f s\n", lo, hi, crossings, t.back() - t.front());
    CHECK(hi > 60 && hi < 75 && lo < -60 && lo > -75);           // 69.7 cents, sampled once a period
    CHECK(std::abs(crossings - 5.005 * (t.back() - t.front())) < 1.5);
    std::vector<double> td;
    const std::vector<double> d = periodPitch("PitchLFODelayTime=32", 2.0, &td);
    double before = 0, after = 0;
    for (size_t i = 0; i < d.size(); ++i) { if (td[i] < 0.95) before = std::max(before, std::abs(d[i])); if (td[i] > 1.15) after = std::max(after, std::abs(d[i])); }
    std::printf("pitch LFO delay 32: %.2f cents before 0.95 s, %.1f after 1.15 s\n", before, after);
    CHECK(before < 5 && after > 60);  // the zero-crossing estimate scatters about 2 cents
}

// Controller box destinations against the reference (va-round6 A1: knob 1 at 1023 through a unipolar linear box, a saw or square at key
// 36): the oscillators' frequency (123) +8 -> +450.00 cents and -32 -> -1800.00 cents (36 semitones at a full box); the pulse width
// (125) +16 from 128 -> h2/h1 0.6965 (63 raw steps: span 255); the pitch LFO's speed (401) +16 from 150 -> 17.50 Hz (span 415).
static double meanHz(const std::vector<float>& L, size_t from) {
    std::vector<double> z;
    for (size_t i = from; i + 1 < L.size(); ++i) if (L[i] < 0 && L[i + 1] >= 0) z.push_back(double(i) + L[i] / (L[i] - L[i + 1]));
    return z.size() < 2 ? 0 : 44100 * double(z.size() - 1) / (z.back() - z.front());
}
static void testBoxDestinations() {
    const std::string box = "PartAssignableKnob1Value=1023 Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetPolarity=0 ";
    double h[2]; std::vector<float> L;
    const double base = 440 * std::exp2((36 - 69) / 12.0);
    for (const auto& c : {std::pair<int, double>{136, 450.0}, {96, -1800.0}}) {
        harmonics((box + "Box1ControllerSetDestination=123 Box1ControllerSetRatio=" + std::to_string(c.first) +
                   " Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511").c_str(), 36, h, 1, 4410, 44100, &L);
        const double cents = 1200 * std::log2(meanHz(L, 4410) / base);
        std::printf("box on the frequency, ratio %+d: %+.2f cents (reference %+.2f)\n", c.first - 128, cents, c.second);
        CHECK(std::fabs(cents - c.second) < 1.0);
    }
    harmonics((box + "Box1ControllerSetDestination=125 Box1ControllerSetRatio=144 Osc1OscillatorWave=2 Osc1OscillatorOutLevel=511").c_str(), 36, h, 2);
    const double r = std::pow(10, (h[1] - h[0]) / 20);
    std::printf("box on the pulse width, ratio +16: h2/h1 %.4f (reference 0.6965)\n", r);
    CHECK(std::fabs(r - 0.6965) < 0.01);
    harmonics((box + "Box1ControllerSetDestination=401 Box1ControllerSetRatio=144 Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 "
               "Osc1OscillatorPitchLFODepth=300 PitchLFOWave=3 PitchLFOSpeed=150 PitchLFOKeyOnReset=1").c_str(), 84, h, 1, 4410, 88200, &L);
    std::vector<double> z;  // the LFO's rate from the period sequence's upward crossings of its mean
    for (size_t i = 4410; i + 1 < L.size(); ++i) if (L[i] < 0 && L[i + 1] >= 0) z.push_back(double(i) + L[i] / (L[i] - L[i + 1]));
    std::vector<double> per; for (size_t i = 1; i < z.size(); ++i) per.push_back(z[i] - z[i - 1]);
    double m = 0; for (double x : per) m += x; m /= double(per.size());
    int up = 0; double first = 0, last = 0;
    for (size_t i = 1; i < per.size(); ++i) if (per[i - 1] >= m && per[i] < m) { if (!up) first = z[i]; last = z[i]; ++up; }
    const double hz = up > 1 ? 44100 * (up - 1) / (last - first) : 0;
    std::printf("box on the pitch LFO's speed, ratio +16: %.2f Hz (reference 17.50)\n", hz);
    CHECK(std::fabs(hz / 17.50 - 1) < 0.02);
    // the filter LFO's depth (144): +16 from 256 sounds as the static depth 304 (span 192: the reference's swings at +16 / +32 fall
    // between its static depths 256-320 / 320-384 where 304 / 352 predict them)
    const std::string flt = "Osc1OscillatorWave=0 Osc1OscillatorOutLevel=511 Filter1FilterType=1 Filter1FilterCutoff=500 FilterCutoffLFOWave=3 "
                            "FilterCutoffLFOSpeed=120 FilterCutoffLFOKeyOnReset=1 ";
    std::vector<float> a1, b1;
    harmonics((box + flt + "Box1ControllerSetDestination=144 Box1ControllerSetRatio=144 Filter1FilterCutoffLFODepth=256").c_str(), 36, h, 1, 4410, 44100, &a1);
    harmonics((flt + "Filter1FilterCutoffLFODepth=304").c_str(), 36, h, 1, 4410, 44100, &b1);
    double d = 0, e = 0; for (size_t i = 0; i < a1.size(); ++i) { d += double(a1[i] - b1[i]) * (a1[i] - b1[i]); e += double(b1[i]) * b1[i]; }
    std::printf("box on the filter LFO depth, ratio +16, against the static depth 304: %.2e relative\n", std::sqrt(d / (e + 1e-30)));
    CHECK(e > 0 && d < 1e-10 * e);
}

// Ageing (va-round7 X1, G2): at drift 0 it moves nothing over 0.12 dB or 0.01 cent from 0 to 200; at drift 64 the drift's statistics at
// 154 and 200 are those at 100 (12 voices each). Covered there; under 100 with drift, and with drift over 64, reported.
static void testAgeing() {
    Gap g[64]; Patch q; q.set(Ageing, 154);
    CHECK(notCovered(q, g, 64) == 0);  // drift 64, the model's default
    q.set(Ageing, 30);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Ageing);
    q.set(VoltageDrift, 0);
    CHECK(notCovered(q, g, 64) == 0);
}

// Aftertouch as a box source (va-round7 A3P: channel pressure through a +32 cutoff box sounds as the mod wheel at 0, 16, 32, 48, 64, 96
// and 127 within 0.000 dB on harmonics 1-8: the 7-bit map): covered.
static void testPressureSource() {
    Gap g[64]; Patch q;
    q.set(Box1ControllerSetSwitch, 1); q.set(Box1ControllerSetSource, 2); q.set(Box1ControllerSetDestination, 142);
    CHECK(notCovered(q, g, 64) == 0);
}

// The oscillators' wave (122; va-round7 A2: the mod wheel through a +-64 box at the 7-bit map's exact values): the wave moves by
// the sum x 4 / 8192 rounded half away from zero (sum 960 -> none, 1024 -> one step up, -1024 -> one down, -3072 -> two down).
// A box landing on a wave sounds as that wave set in the patch, bit for bit; coverage takes the box where no pulse is involved.
static void testWaveBox() {
    const std::string box = "Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetDestination=122 Osc1OscillatorOutLevel=511 ";
    const struct { int base, ratio, knob, lands; } cases[] = {{0, 192, 120, 0}, {0, 192, 128, 1}, {0, 192, 384, 2}, {2, 64, 128, 1}, {2, 192, 128, 3},
                                                              {4, 64, 120, 4}, {4, 64, 128, 3}, {4, 64, 384, 2}};
    for (const auto& c : cases) {
        double h[1]; std::vector<float> a, b;
        harmonics((box + "Osc1OscillatorWave=" + std::to_string(c.base) + " Box1ControllerSetRatio=" + std::to_string(c.ratio) +
                   " PartAssignableKnob1Value=" + std::to_string(c.knob)).c_str(), 36, h, 1, 4410, 8820, &a);
        harmonics(("Osc1OscillatorOutLevel=511 Osc1OscillatorWave=" + std::to_string(c.lands)).c_str(), 36, h, 1, 4410, 8820, &b);
        double d = 0, e = 0; for (size_t i = 0; i < a.size(); ++i) { d += double(a[i] - b[i]) * (a[i] - b[i]); e += double(b[i]) * b[i]; }
        std::printf("box on the wave: base %d, ratio %+d, knob %d -> as wave %d: %.1e relative\n", c.base, c.ratio - 128, c.knob, c.lands, std::sqrt(d / (e + 1e-30)));
        CHECK(e > 0 && d < 1e-12 * e);
    }
    Gap g[64]; Patch p; char e[200];
    readPatch(("vcore-patch/1 " + box + "Box1ControllerSetSource=6 Box1ControllerSetPolarity=1 Box1ControllerSetRatio=255").c_str(), p, e, sizeof e);
    CHECK(notCovered(p, g, 64) == 0);  // the ribbon over its range, every wave a table read
    p.set(Osc1OscillatorPulseWidth, 100);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Osc1OscillatorWave);
}

// The saturators against the reference (va-round7 X1: a 65.4 Hz sine through LPF24 at 1023): the fundamental's gain over drive 0
// and the 2nd harmonic against the fundamental, filter 1 at drives 7 / 15 / 23 (+1.07 / +1.07 / +0.73 dB, -56.5 / -50.8 / -44.7 dB),
// filter 2 at 15 (as filter 1's), the amplitude saturator at 8 (+0.61 dB, -59.7 dB). A drive not measured is reported.
static void testSaturators() {
    const std::string sine = "Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 Filter1FilterType=1 Filter1FilterCutoff=1023 ";
    double h0[2], h[2];
    harmonics(sine.c_str(), 36, h0, 2);
    const struct { const char* extra; double gain, h2; } cases[] = {
        {"Filter1FilterSaturatorDrive=7", 1.070, -56.5}, {"Filter1FilterSaturatorDrive=15", 1.072, -50.8}, {"Filter1FilterSaturatorDrive=23", 0.727, -44.7},
        {"Filter2FilterType=5 Filter2FilterSaturatorDrive=15", 1.072 - 0.65, -50.8}, {"AmplitudeSaturatorDrive=8", 0.609, -59.7}};
    for (const auto& c : cases) {
        harmonics((sine + c.extra).c_str(), 36, h, 2);
        std::printf("saturator %s: h1 %+.3f dB, h2 %.1f dB re h1 (reference %+.3f, %.1f)\n", c.extra, h[0] - h0[0], h[1] - h[0], c.gain, c.h2);
        CHECK(std::fabs(h[0] - h0[0] - c.gain) < 0.1 && std::fabs(h[1] - h[0] - c.h2) < 1.0);
    }
    // the amplitude saturator acts after the amplitude EG (va-round7 S2: drive 23, the EG decaying, h2 against h1 falls 18.4 dB as the
    // level falls 18.0 dB; the filter's stays within 0.1 dB)
    auto decaying = [&](const char* extra, double* fall, double* h2fall) {
        std::vector<float> L; double a[2], b[2];
        const std::string t = sine + "AmplitudeEGDecayTime=200 AmplitudeEGSustainLevel=0 " + extra;
        harmonics(t.c_str(), 36, a, 2, 4410, 8820, &L); harmonics(t.c_str(), 36, b, 2, 4410 + 2 * 44100, 8820);
        *fall = a[0] - b[0]; *h2fall = (a[1] - a[0]) - (b[1] - b[0]);
    };
    double fall, h2fall;
    decaying("AmplitudeSaturatorDrive=23", &fall, &h2fall);
    std::printf("amplitude saturator 23 over the EG's decay: level falls %.1f dB, h2 against h1 %.1f dB\n", fall, h2fall);
    CHECK(fall > 10 && std::fabs(h2fall - fall) < 1.0);  // h2 goes as the level squared over a window the level moves in
    decaying("Filter1FilterSaturatorDrive=23", &fall, &h2fall);
    std::printf("filter saturator 23 over the EG's decay: level falls %.1f dB, h2 against h1 %.1f dB\n", fall, h2fall);
    CHECK(fall > 10 && std::fabs(h2fall) < 0.2);
    Gap g[64]; Patch q = plain();
    q.set(Filter1FilterSaturatorDrive, 23); q.set(Filter2FilterSaturatorDrive, 15); q.set(AmplitudeSaturatorDrive, 8);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Filter1FilterSaturatorDrive);  // filter 2 a filter: filter 1's runs after it
    q = Patch();  // the model's default: filter 2 a high-pass at 0 (under 5.2 Hz, its gain only)
    q.set(Filter1FilterSaturatorDrive, 23); q.set(Filter2FilterSaturatorDrive, 15); q.set(AmplitudeSaturatorDrive, 8);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(Filter1FilterSaturatorDrive, 24);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Filter1FilterSaturatorDrive);
}

static void testStatistics() {
    const auto x = renderProbeA(2, 1, 128);
    const size_t half = x.size() / 2, win = 4800;
    std::vector<double> rms;
    for (size_t w = 0; w + win <= half; w += win) {
        double s = 0; for (size_t i = w; i < w + win; ++i) s += double(x[i]) * x[i] + double(x[half + i]) * x[half + i];
        rms.push_back(std::sqrt(s / double(2 * win)));
    }
    if (std::getenv("VC_WRITE_STATS")) {
        std::printf("// SPDX-License-Identifier: AGPL-3.0-or-later\n// Generated by test_core (VC_WRITE_STATS=1): probe A's stored render statistics.\n");
        for (double v : rms) std::printf("%.9e,\n", v);
        return;
    }
    CHECK(rms.size() == sizeof kStats / sizeof kStats[0]);
    double worst = 0;
    for (size_t i = 0; i < rms.size() && i < sizeof kStats / sizeof kStats[0]; ++i) {
        const double d = std::fabs(rms[i] - kStats[i]);
        worst = std::fmax(worst, d / std::fmax(kStats[i], kFloor));
        CHECK(d <= kTolerance * kStats[i] || d <= kFloor);
    }
    std::printf("statistics: %zu windows, worst relative difference %.2e (tolerance %.0e)\n", rms.size(), worst, kTolerance);
}

int main() {
    testText(); testCoverage(); testCutoffAgainstReference(); testFilterTypes(); testCalibration(); testOscLevel(); testAlias(); testReleaseThreshold(); testLongSegment(); testBoxLaw(); testPitchDepthKnots(); testFegDepthZero(); testTimeLaws(); testDrift(); testInterleaveIdentical(); testVoiceLifetime(); testPitchLfo(); testBoxDestinations(); testAgeing(); testPressureSource(); testWaveBox(); testWaves(); testSaturators(); testPortamento(); testPortamentoBox(); testMonoAndSingle(); testStatistics();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
