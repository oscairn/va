// SPDX-License-Identifier: AGPL-3.0-or-later
// The DSP core's own tests (core/, linked directly; no plugin): patch text, coverage, plans, voices,
// and the cross-build statistics of a fixed sequence (every target must stay within kTolerance of
// the stored values). VC_WRITE_STATS=1 prints the statistics in the form of probeA_stats.inc instead.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vcore.h"

using namespace vcore;
namespace vcore { const float* bandTables(); }  // laws.cpp

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
    p = plain(); p.set(Osc3OscillatorOutLevel, 50); p.set(Unison, 2);  // oscillator 3 under 90: no measured level law (unison: covered)
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Osc3OscillatorOutLevel);
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
    CHECK(notCovered(p, g, 64) == 0 && describe(p).lfo[1]);  // the filter LFO: depth verified to 128 steps (va-round8 F)
    p.set(Filter1FilterCutoffLFODepth, 384);
    CHECK(notCovered(p, g, 64) == 0);
    p.set(Filter1FilterCutoffLFODepth, 400);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Filter1FilterCutoffLFODepth);
    {  // the depth law: 25.17 cents a step at the LFO's peak (24 statics from -128 to +128 steps within 0.83 %; 26.6 read the swings 4-5 % large)
        Sound s; auto c = std::make_unique<Core>(); c->prepare(48000); c->compile(s, p);
        CHECK(std::fabs(s.lfoCut * 100 / 144 - 25.17) < 0.01);
    }
    p = plain(); p.set(VoltageDrift, 127);
    CHECK(notCovered(p, g, 64) == 0);  // drift: measured to 127 (va-round8 D)
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
    p.set(Box3ControllerSetCurveType, 0); p.set(Box3ControllerSetDestination, 146);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetDestination);
    // classified: an effect's parameter or a send is the host's; a box on a unit out of scope says so
    for (int d : {1, 34, 50, 51, 356, 369}) { p.set(Box3ControllerSetDestination, d); CHECK(notCovered(p, g, 64) == 1 && std::strstr(g[0].why, "the host's")); }
    for (int d : {128, 131, 134, 135, 138, 406}) { p.set(Box3ControllerSetDestination, d); CHECK(notCovered(p, g, 64) == 1 && std::strstr(g[0].why, "out of scope")); }
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
    // changes() stays while every voice sounds on, and moves on at a note-off and when the voice ends
    const uint32_t held = core->changes();
    core->render(&l[0], &r[0], 128);
    CHECK(core->changes() == held);
    core->noteOff(3);
    CHECK(core->voice(3).releasing && core->changes() != held);
    uint32_t frames = 0, released = core->changes(), moved = 0;
    while (core->voice(3).sounding && frames < 48000 * 3) {
        std::vector<float> a(128), b(128); core->render(a.data(), b.data(), 128); frames += 128;
        moved += core->changes() != released;
    }
    CHECK(moved == 1);  // only the render in which it ended
    // the release (raw 145 from sustain 140, the low-release law) ends at exactly zero about 1.03 s after the note-off
    CHECK(!core->voice(3).sounding && frames > 48000 * 9 / 10 && frames < 48000 * 12 / 10);
    core->noteOn(5, 0, 0, 90, 1.f); core->noteOn(6, 0, 0, 91, 1.f);
    core->choke(5, 144);
    { std::vector<float> a(256), b(256); core->render(a.data(), b.data(), 140); CHECK(core->voice(5).sounding); core->render(a.data(), b.data(), 8); }
    CHECK(!core->voice(5).sounding && core->voice(6).sounding);
    core->kill(6);
    CHECK(!core->voice(6).sounding && core->voiceCount() == 0);
}

// The quiet end (docs/design.md): a released note ends once a bound of its output gain is below -96 dBFS. One saw (filters thru) at
// level 100 with a box from the mod wheel able to take it to the top, a long release (raw 200: about 6 s to the envelope's own end)
// and the caller's gain at 0.001: the bound is the top level's saw at full scale times the sound's level, the gain and the louder pan
// side, so the note ends where the release law brings the envelope to -96 dBFS over it, long before the envelope's own end; with the
// wheel moved to the top just before, at the same time and still below -96 dBFS until then. A note held at a sustain under that
// level plays on.
static void testQuietEnd() {
    namespace vc = vcore;
    const char* text = "vcore-patch/1 Osc1OscillatorOutLevel=100 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterType=0 "
                       "Filter2FilterType=0 AmplitudeEGAttackTime=0 AmplitudeEGDecayTime=0 AmplitudeEGReleaseTime=200 Box1ControllerSetSwitch=1 "
                       "Box1ControllerSetSource=1 Box1ControllerSetDestination=136 Box1ControllerSetRatio=255";
    Patch p; char e[200];
    CHECK(readPatch(text, p, e, sizeof e));
    for (bool wheel : {false, true}) {
        auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 0.001f, 0);
        Sound s; core->compile(s, p);
        std::vector<float> l(128), r(128);
        core->noteOn(0, 0, 0, 60, 1.f);
        for (int i = 0; i < 100; ++i) core->render(l.data(), r.data(), 128);
        const double l0 = core->voice(0).level;  // the envelope at the note-off (full sustain: the fast release law)
        CHECK(l0 >= s.amp.threshold);
        core->noteOff(0);
        const double gain = vc::lawTables().oscLevel[511] * vc::kTabPeak * s.level * 0.001 * std::sqrt(0.5), quiet = std::pow(10.0, -96.0 / 20) / gain;
        const double target = -l0 * s.amp.endHigh;  // the release falls toward it, a straight line in log2 of the distance at lr a frame
        const double end = std::log2((quiet - target) / (l0 - target)) / s.amp.lr, full = std::log2(-target / (l0 - target)) / s.amp.lr;
        long last = -1, t = 0; double loud = 0;
        while (core->voice(0).sounding && t < long(full) + 48000) {
            if (wheel && t == long(end) / 128 * 128 - 4864) core->setController(0, 1, 1.f);  // the level to its top, 0.1 s before
            std::fill(l.begin(), l.end(), 0.f); std::fill(r.begin(), r.end(), 0.f);
            core->render(l.data(), r.data(), 128);
            for (int i = 0; i < 128; ++i) {
                const double x = std::fmax(std::fabs(l[size_t(i)]), std::fabs(r[size_t(i)]));
                if (x > 0) last = t + i;
                if (t + i > long(end) - 2400) loud = std::fmax(loud, x);
            }
            t += 128;
        }
        std::printf("quiet end%s: predicted %.0f frames after the note-off, last sample at %ld; the envelope's own end at %.0f\n",
                    wheel ? " (wheel at the top)" : "", end, last, full);
        CHECK(!core->voice(0).sounding && std::fabs(double(last) - end) <= 48 && end < 0.5 * full);
        CHECK(loud < std::pow(10.0, -96.0 / 20));  // the level at its top could not make it audible before its end
    }
    {  // a unison note (two voices) ends as a whole, by the same rule, long before the envelope's own end
        Patch q = p; q.set(Unison, 1);
        auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, q); core->setGain(0, 0.001f, 0);
        std::vector<float> l(128), r(128);
        core->noteOn(0, 0, 0, 60, 1.f);
        for (int i = 0; i < 100; ++i) core->render(l.data(), r.data(), 128);
        core->noteOff(0);
        long t = 0; bool whole = true;
        while (core->voiceCount() && t < 48000 * 8) { core->render(l.data(), r.data(), 128); t += 128; whole &= core->voiceCount() != 1; }
        CHECK(whole && core->voiceCount() == 0 && t < 48000 * 4);
    }
    {  // held at a sustain far below that: plays on
        Patch q = p; q.set(AmplitudeEGSustainLevel, 1); q.set(AmplitudeEGDecayTime, 0);
        auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, q); core->setGain(0, 0.001f, 0);
        std::vector<float> l(128), r(128);
        core->noteOn(0, 0, 0, 60, 1.f);
        for (int i = 0; i < 48000 * 8 / 128; ++i) core->render(l.data(), r.data(), 128);
        CHECK(core->voice(0).sounding && !core->voice(0).releasing);
    }
    // the tables' largest sample is under the bound's full scale
    float m = 0; const float* tab = vc::bandTables();
    for (int i = 0; i < (4 * kLevels + 1) * (kTabN + 1); ++i) m = std::fmax(m, std::fabs(tab[i]));
    CHECK(m <= kTabPeak);
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
    double moving = 0, wide = 0;
    for (uint32_t s = 1; s <= 4; ++s) { moving += pitchRange(64, s) / 4; wide += pitchRange(127, s) / 4; }
    const double still = pitchRange(0, 1);
    std::printf("drift: pitch range over 1 s windows: %.2f cents at 64 (4 voices), %.2f at 127, %.3f at 0\n", moving, wide, still);
    CHECK(moving > 1.5 && moving < 15 && still < 0.1);
    // over 64 (va-round8 D, 12 voices held 30 s, 16 note starts): at 127 the voices sit +17 cents on average, move with 7.4 times 64's
    // amplitude (within-voice sd 11.5 cents against 1.7) and each voice's cutoff is off by sd 4.0 semitones (64: 0.30)
    CHECK(wide > 4 * moving);
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p; p.set(VoltageDrift, 127); core->compile(s, p);
    CHECK(std::fabs(s.driftMean * 100 - 17.0) < 0.01 && std::fabs(s.driftCutSpread / std::sqrt(3.f) - 3.985f) < 0.01);
    p.set(VoltageDrift, 64); core->compile(s, p);
    CHECK(std::fabs(s.driftCutSpread / std::sqrt(3.f) - 0.296f) < 0.01);
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
    // short releases (va-round8 R2, 12 voices each, the engine's renders through the same analysis): the amplitude EG's at 16 / 32 / 55 / 64
    // read 0.97 / 1.53 / 2.82 / 3.77 ms in the reference (each voice +-0.4-0.65 ms), the filter EG's at 55 / 60 / 70 2.77 / 3.27 / 4.85 ms
    // (the amplitude's law); before, the amplitude's held 64's 4.16 ms under 64 and the filter's 128's 48 ms under 128
    for (const auto& c : {std::pair<int, double>{16, 0.00097}, {32, 0.00153}, {55, 0.00282}, {64, 0.00377}}) {
        p.set(AmplitudeEGReleaseTime, c.first); core->compile(s, p);
        CHECK(std::fabs(tau(s.amp.lr) / c.second - 1) < 0.1);
    }
    for (const auto& c : {std::pair<int, double>{55, 0.00277}, {60, 0.00327}, {70, 0.00485}}) {
        p.set(FilterCutoffEGReleaseTime, c.first); core->compile(s, p);
        CHECK(std::fabs(tau(s.feg.lr) / c.second - 1) < 0.1);
    }
    Gap g[64]; Patch q = plain(); q.set(FilterCutoffEGDecayTime, 117);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(FilterCutoffEGDecayTime, 99);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == FilterCutoffEGDecayTime);
    q = plain(); q.set(AmplitudeEGReleaseTime, 0); q.set(FilterCutoffEGReleaseTime, 55);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(FilterCutoffEGReleaseTime, 54);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == FilterCutoffEGReleaseTime);
}

// The pitch LFO's depth between the knots 80 and 96 (va-round3 R1: the mod wheel's box landing on 86 and 92 steps):
// 150.0 and 184.6 cents, where the line between the old knots gave 162.6 and 188.1.
static void testPitchDepthKnots() {
    auto core = std::make_unique<Core>(); core->prepare(48000);
    Sound s; Patch p;
    p.set(Osc1OscillatorPitchLFODepth, 247 + 86); p.set(Osc2OscillatorPitchLFODepth, 247 - 92); core->compile(s, p);
    CHECK(std::fabs(s.lfoPitch[0] * 100 - 150.0) < 0.05 && std::fabs(s.lfoPitch[1] * 100 + 184.57) < 0.05);
}

// The box curves (core/curves.inc, measured at the mod wheel's 128 values): through the 7-bit map each lands on its values at
// the wheel's values, straight between; against the published numbers (each curve's sum over the 128 values, and points the
// three engines' tests read alike); the linear curve as before; a curve not measured still reported, and a measured one reported
// where its source falls between the wheel's values (the bend, a knob).
static void testCurves() {
    const struct { int type, p1, p2; long sum; } published[] = {
        {0, 0, 0, 121352}, {0, 1, 0, 109136}, {0, 2, 0, 97497}, {0, 3, 0, 88232}, {0, 4, 0, 77913}, {0, 6, 0, 52733}, {0, 7, 0, 42455},
        {0, 8, 0, 33265}, {0, 9, 0, 21722}, {0, 10, 0, 9633}, {1, 2, 0, 76181}, {2, 0, 2, 113591}, {2, 0, 3, 105362}, {2, 0, 4, 97134},
        {2, 6, 1, 52665}, {2, 6, 2, 52593}, {2, 7, 3, 44619}, {2, 8, 1, 27065}, {2, 8, 2, 30082}, {2, 8, 3, 38238}, {2, 8, 4, 46392},
        {2, 9, 3, 31864}, {2, 10, 1, 9180}, {2, 10, 2, 17335}, {2, 10, 4, 33646}, {3, 1, 0, -119685}, {3, 2, 0, -114253},
        {3, 5, 0, -94336}, {3, 6, 0, -95326}, {3, 7, 0, -96053}, {9, 0, 0, 66393}, {10, 0, 0, 65116}, {17, 0, 0, 0}, {18, 4, 0, 83552},
        {19, 2, 0, 65152}};
    CHECK(sizeof published / sizeof published[0] == kCurveCount - 1);
    for (const auto& c : published) {
        const int id = findCurve(0, c.type, c.p1, c.p2);
        long sum = 0; bool onValues = id > 0;
        for (int v = 0; v < 128 && id > 0; ++v) { const float y = curveAt(id, curveKnot(v)); sum += long(y); onValues &= y == float(kCurves[id].y[v]); }
        if (!(id > 0 && onValues && sum == c.sum)) { std::printf("curve (%d, %d, %d): id %d, sum %ld\n", c.type, c.p1, c.p2, id, sum); ++failures; }
    }
    auto at = [](int type, int p1, int p2, int v) { return curveAt(findCurve(0, type, p1, p2), curveKnot(v)); };
    CHECK(at(0, 8, 0, 0) == 0 && at(0, 8, 0, 32) == 64 && at(0, 8, 0, 64) == 171 && at(0, 8, 0, 96) == 390);
    CHECK(at(0, 1, 0, 8) == 433 && at(0, 1, 0, 64) == 939 && at(0, 1, 0, 120) == 1019);
    CHECK(at(2, 6, 1, 56) == 0 && at(2, 6, 1, 64) == 128 && at(2, 6, 1, 72) == 384 && at(2, 6, 1, 127) == 1024);
    CHECK(at(3, 1, 0, 8) == -367 && at(3, 1, 0, 32) == -1024 && at(3, 1, 0, 127) == -1024);
    CHECK(at(10, 0, 0, 0) == 0 && at(10, 0, 0, 64) == 1024 && at(10, 0, 0, 120) == 103);
    CHECK(at(18, 4, 0, 16) == 341 && at(18, 4, 0, 31) == 341 && at(18, 4, 0, 32) == 541 && at(17, 0, 0, 100) == 0);
    CHECK(findCurve(0, 3, 1, 7) == findCurve(0, 3, 1, 0));  // parameter 2 counts for type 2 only
    const int i = findCurve(0, 0, 8, 0);  // halfway between the wheel's 64 and 65
    CHECK(std::fabs(curveAt(i, (curveKnot(64) + curveKnot(65)) / 2) - (curveAt(i, curveKnot(64)) + curveAt(i, curveKnot(65))) / 2) < 1e-3f);
    CHECK(curveAt(0, 0) == 0 && curveAt(0, 516.5f) == 516.5f && curveAt(0, 1022) == 1022 && curveAt(0, 1023) == 1024);  // linear
    CHECK(findCurve(0, 16, 9, 0) < 0 && findCurve(0, 2, 3, 1) < 0 && findCurve(1, 0, 8, 0) < 0);  // not measured
    Gap g[64];
    Patch p = plain(); p.set(Box3ControllerSetSwitch, 1); p.set(Box3ControllerSetSource, 1); p.set(Box3ControllerSetPolarity, 0); p.set(Box3ControllerSetRatio, 136);
    p.set(Filter2FilterControllerBox3Switch, 0); p.set(Filter1FilterResonance, 0);
    p.set(Box3ControllerSetCurveType, 3); p.set(Box3ControllerSetCurveParameter1, 1);
    CHECK(notCovered(p, g, 64) == 0);  // the mod wheel through a falling curve
    p.set(Box3ControllerSetSource, 0);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetCurveType);  // the bend
    p.set(Box3ControllerSetSource, 8); p.set(PartAssignableKnob1Value, 512);
    CHECK(notCovered(p, g, 64) == 0);
    p.set(PartAssignableKnob1Value, 516);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetCurveType);
    p.set(Box3ControllerSetSource, 1); p.set(Box3ControllerSetCurveType, 16); p.set(Box3ControllerSetCurveParameter1, 9);
    CHECK(notCovered(p, g, 64) == 1 && g[0].param == Box3ControllerSetCurveType);
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
    // a box on mono/poly (53; va-coverage-2026-11-22 F: key 60 held, then 67): from poly, -64 at the wheel's 65 (sum -4160) turns mono and
    // 64 (-4096) does not, nor -16 at 127; from mono, +64 at 127 turns poly and at 64 does not; read at a note-on: the wheel moved while
    // both sound changes nothing, moved before the second note it takes the first note over
    auto two = [](int mode, int ratio, int wheel, int movedTo, bool before) {
        Patch q; q.set(PartMonoPolyMode, mode); q.set(Box1ControllerSetSwitch, 1); q.set(Box1ControllerSetSource, 1);
        q.set(Box1ControllerSetDestination, 53); q.set(Box1ControllerSetRatio, 128 + ratio); q.set(Box1ControllerSetPolarity, 0);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, q); c->setGain(0, 1, 0);
        std::vector<float> l(480), r(480);
        c->setController(0, 1, float(wheel) / 127);
        c->noteOn(0, 0, 0, 60, 0.8f); c->render(l.data(), r.data(), 480);
        if (before) c->setController(0, 1, float(movedTo) / 127);
        c->noteOn(1, 0, 0, 67, 0.8f); c->render(l.data(), r.data(), 480);
        if (!before) { c->setController(0, 1, float(movedTo) / 127); c->render(l.data(), r.data(), 480); }
        return c->voice(0).sounding && c->voice(1).sounding && c->voiceCount() == 2;  // true: both sound (poly)
    };
    CHECK(two(1, -64, 63, 63, true) && two(1, -64, 64, 64, true) && !two(1, -64, 65, 65, true) && !two(1, -64, 127, 127, true));
    CHECK(two(1, -16, 127, 127, true) && two(1, 64, 127, 127, true) && !two(0, 64, 64, 64, true) && two(0, 64, 127, 127, true));
    CHECK(!two(1, -64, 0, 127, true) && two(1, -64, 0, 127, false));
}

// Unison against the reference (va-round8 U, a sine at key 84): each copy's partial at its detune, its level in the near and the far
// channel against one voice's (mode 1: -3.01 dB, mode 2: -6.02 dB; the far side by the spread), from the left and right channels.
static double dftDb(const std::vector<float>& x, size_t from, size_t n, double f) {
    double re = 0, im = 0; const double pi = 3.14159265358979;
    for (size_t i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * pi * i / n), ph = 2 * pi * f * double(from + i); re += x[from + i] * w * std::cos(ph); im += x[from + i] * w * std::sin(ph); }
    return 10 * std::log10(re * re + im * im + 1e-30);
}
static void renderSine(const char* extra, int key, std::vector<float>& L, std::vector<float>& R, size_t frames) {
    const std::string t = std::string("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter1FilterType=0 Filter2FilterType=0 "
                                      "Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 ") + extra;
    Patch p; char e[200]; if (!readPatch(t.c_str(), p, e, sizeof e)) { std::printf("unison: %s\n", e); ++failures; return; }
    auto core = std::make_unique<Core>(); core->prepare(44100); core->load(0, p); core->setGain(0, 1, 0); core->noteOn(0, 0, 0, key, 0.8f);
    L.assign(frames, 0.f); R.assign(frames, 0.f); core->render(L.data(), R.data(), uint32_t(frames));
}
static void testUnison() {
    const size_t from = 13230, n = 88200;
    const double f = 440 * std::exp2(15 / 12.0) / 44100;
    std::vector<float> L, R;
    renderSine("", 84, L, R, from + n);
    const double one = dftDb(L, from, n, f);
    // (mode, detune, spread): the reference's copies, flat to sharp: cents, left and right dB re one voice (one voice: -6.07 dB)
    const struct { int u, d, s, copies; double c[4], l[4], r[4]; } cases[] = {
        {1, 6, 0, 2, {-5.93, 5.94}, {-3.01, -3.01}, {-3.01, -3.01}},
        {1, 2, 0, 2, {-1.97, 1.99}, {-3.02, -3.01}, {-3.02, -3.01}},
        {1, 6, 14, 2, {-5.93, 5.94}, {-3.01, -15.43}, {-15.43, -3.01}},
        {1, 6, 7, 2, {-5.93, 5.94}, {-3.01, -5.65}, {-5.65, -3.01}},
        {1, 15, 15, 2, {-14.93, 14.95}, {-3.02, -17.70}, {-17.70, -3.02}},
        {2, 5, 3, 4, {-9.95, -3.29, 3.30, 9.97}, {-6.02, -6.02, -6.40, -7.90}, {-7.90, -6.40, -6.02, -6.02}},
        {2, 6, 15, 4, {-11.93, -3.95, 3.96, 11.95}, {-6.02, -6.02, -11.67, -99}, {-99, -11.67, -6.02, -6.02}}};
    for (const auto& c : cases) {
        renderSine(("Unison=" + std::to_string(c.u) + " UnisonDetune=" + std::to_string(c.d) + " UnisonSpread=" + std::to_string(c.s)).c_str(), 84, L, R, from + n);
        double worstC = 0, worstDb = 0;
        for (int k = 0; k < c.copies; ++k) {
            double best = -1e9, at = 0;  // the copy's partial: the peak within half a cent of the reference's
            for (double x = c.c[k] - 0.5; x <= c.c[k] + 0.5; x += 0.01) { const double v = std::pow(10, dftDb(L, from, n, f * std::exp2(x / 1200)) / 10) + std::pow(10, dftDb(R, from, n, f * std::exp2(x / 1200)) / 10); if (v > best) { best = v; at = x; } }
            const double fk = f * std::exp2(at / 1200), l = dftDb(L, from, n, fk) - one, r = dftDb(R, from, n, fk) - one;
            worstC = std::fmax(worstC, std::fabs(at - c.c[k]));
            if (c.l[k] > -90) worstDb = std::fmax(worstDb, std::fabs(l - c.l[k])); else CHECK(l < -60);  // -99: silent (under the reference's floor)
            if (c.r[k] > -90) worstDb = std::fmax(worstDb, std::fabs(r - c.r[k])); else CHECK(r < -60);
            if (std::fabs(at - c.c[k]) > 0.1) std::printf("  copy %d: %.2f cents against %.2f\n", k, at, c.c[k]);
        }
        std::printf("unison %d detune %d spread %d against the reference: copies within %.3f cent and %.3f dB\n", c.u, c.d, c.s, worstC, worstDb);
        CHECK(worstC < 0.1 && worstDb < 0.1);
    }
    Gap g[8]; Patch q; q.set(Unison, 2); q.set(UnisonDetune, 15); q.set(UnisonSpread, 15);
    CHECK(notCovered(q, g, 8) == 0 && unisonVoices(q) == 4);
}

// A unison note is its voices together: started, released, faded, stolen and freed as one note (never a copy left behind); a full pool
// cuts a note already fading; mono and legato with portamento move every copy (va-round8 U UG: the copies glide together, keep their
// detune and spread).
static void testUnisonNotes() {
    Patch p; char e[200];
    readPatch("vcore-patch/1 Unison=2 UnisonDetune=6 AmplitudeEGReleaseTime=64", p, e, sizeof e);
    auto core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->setGain(0, 1, 0);
    std::vector<float> a(128), b(128);
    core->noteOn(3, 0, 0, 60, 1.f);
    CHECK(core->voiceCount() == 4 && core->voice(3).sounding);
    core->noteOn(3, 0, 0, 62, 1.f);  // the slot again: the old note's copies go with it
    CHECK(core->voiceCount() == 4);
    core->noteOn(4, 0, 0, 64, 1.f); core->render(a.data(), b.data(), 128);
    core->noteOff(3);
    CHECK(core->voice(3).releasing && core->voiceCount() == 8);
    uint32_t frames = 0;
    while (core->voice(3).sounding && frames < 48000) { CHECK(core->voiceCount() == 8); core->render(a.data(), b.data(), 128); frames += 128; }
    CHECK(!core->voice(3).sounding && core->voiceCount() == 4);  // every copy ended on the same step
    core->choke(4, 100);
    core->render(a.data(), b.data(), 128);
    CHECK(!core->voice(4).sounding && core->voiceCount() == 0);
    for (uint32_t v = 0; v < 16; ++v) core->noteOn(v, 0, 0, 40 + int(v), 1.f);  // 64 voices: the pool is full
    CHECK(core->voiceCount() == 64);
    core->noteOn(20, 0, 0, 70, 1.f);
    CHECK(!core->voice(20).sounding && core->voiceCount() == 64);  // no room, nothing fading: the note does not sound
    core->choke(5, 4800);
    core->noteOn(20, 0, 0, 70, 1.f);
    CHECK(core->voice(20).sounding && !core->voice(5).sounding && core->voiceCount() == 64);  // the fading note is cut for it
    core->kill(20);
    CHECK(core->voiceCount() == 60);
    // mono with portamento: a legato note takes every copy over and glides them; its release goes back to the held key with all of them
    std::vector<float> L(48000), R(48000);
    readPatch("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter1FilterType=0 Filter2FilterType=0 Osc1OscillatorWave=4 "
              "Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Unison=1 UnisonDetune=15 PartMonoPolyMode=0 PortamentoMaster=1 PartPortamentoTime=8", p, e, sizeof e);
    core = std::make_unique<Core>(); core->prepare(44100); core->load(0, p); core->setGain(0, 1, 0);
    core->noteOn(0, 0, 0, 72, 0.8f); core->render(L.data(), R.data(), 4410);
    core->noteOn(1, 0, 0, 84, 0.8f);
    CHECK(core->voiceCount() == 2);
    L.assign(13230 + 44100, 0.f); R.assign(L.size(), 0.f); core->render(L.data(), R.data(), uint32_t(L.size()));
    const double f84 = 440 * std::exp2(15 / 12.0) / 44100, one = dftDb(L, 13230, 44100, f84 * std::exp2(-15 / 1200.0)), other = dftDb(L, 13230, 44100, f84 * std::exp2(15 / 1200.0));
    const double mid = dftDb(L, 13230, 44100, f84);
    std::printf("unison under a legato glide 72 -> 84: copies at -15 / +15 cents %.1f / %.1f dB, between them %.1f dB\n", one, other, mid);
    CHECK(std::fabs(one - other) < 0.5 && mid < one - 6);
    core->noteOff(1); core->render(L.data(), R.data(), 4410);
    CHECK(core->voiceCount() == 2 && core->voice(0).sounding && !core->voice(0).releasing && !core->voice(1).sounding);
    // random notes, releases, chokes, kills and steals on unison sounds of 2 and 4: the voices are always whole notes
    readPatch("vcore-patch/1 Unison=1 AmplitudeEGReleaseTime=40", p, e, sizeof e);
    Patch p4; readPatch("vcore-patch/1 Unison=2 AmplitudeEGReleaseTime=90", p4, e, sizeof e);
    core = std::make_unique<Core>(); core->prepare(48000); core->load(0, p); core->load(1, p4); core->setGain(0, 1, 0); core->setGain(1, 1, 0);
    uint32_t r = 12345; auto rnd = [&](uint32_t m) { r = r * 1664525u + 1013904223u; return (r >> 8) % m; };
    int sound[kMaxVoices]; for (int& s : sound) s = -1;
    bool whole = true, finite = true;
    for (int step = 0; step < 4000; ++step) {
        const uint32_t v = rnd(kMaxVoices);
        switch (rnd(6)) {
        case 0: case 1: sound[v] = int(rnd(2)); core->noteOn(v, uint32_t(sound[v]), 0, 30 + int(rnd(60)), 1.f); break;
        case 2: core->noteOff(v); break;
        case 3: core->choke(v, 1 + rnd(400)); break;
        case 4: core->kill(v); break;
        default: std::fill(a.begin(), a.end(), 0.f); core->render(a.data(), b.data(), 1 + rnd(128)); for (float x : a) finite &= std::isfinite(x) && std::fabs(x) < 16; break;
        }
        uint32_t want = 0;
        for (uint32_t i = 0; i < kMaxVoices; ++i) if (core->voice(i).sounding) want += sound[i] == 0 ? 2 : 4;
        if (whole && core->voiceCount() != want) std::printf("unison fuzz: step %d: %u voices, %u expected\n", step, core->voiceCount(), want);
        whole &= core->voiceCount() == want;
    }
    CHECK(whole && finite);
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
    // the part's pitch (54; va-coverage-2026-11-22 A: a sine against the same key without the box, by zero-crossing fit): a cent an
    // output unit, on every oscillator, to the box sum's +-8192
    for (const auto& c : {std::pair<int, double>{129, 127.95}, {132, 511.96}, {136, 1024.00}, {120, -1024.00}, {144, 2048.00}, {192, 8191.99}}) {
        const int key = c.first == 192 ? 24 : 36;
        harmonics((box + "Box1ControllerSetDestination=54 Box1ControllerSetRatio=" + std::to_string(c.first) + " Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511").c_str(),
                  key, h, 1, 4410, 44100, &L);
        const double cents = 1200 * std::log2(meanHz(L, 4410) / (440 * std::exp2((key - 69) / 12.0)));
        std::printf("box on the pitch, ratio %+d: %+.2f cents (reference %+.2f)\n", c.first - 128, cents, c.second);
        CHECK(std::fabs(cents - c.second) < 1.0);
    }
    harmonics((box + "Box1ControllerSetDestination=54 Box1ControllerSetRatio=136 Osc1OscillatorOutLevel=0 Osc2OscillatorWave=4 Osc2OscillatorOutLevel=511").c_str(),
              36, h, 1, 4410, 44100, &L);
    CHECK(std::fabs(1200 * std::log2(meanHz(L, 4410) / base) - 1024) < 1.0);  // oscillator 2 alike
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
    // every LFO's speed (119; va-coverage-2026-11-22 B, each LFO at speed 150 with key-on reset): +8 at a full box (51 steps) sounds as the
    // static 201 on the pitch, filter, amplitude and oscillator LFOs (the reference's rates 4.3755 / 4.3717 / 4.3721 Hz against 4.3753
    // for the static); with 401 at +8 as well, each destination's steps add: 252 (16.990 Hz against the static 252's 16.992, 253's 17.495)
    {
        const std::string lfo = "Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 Osc1OscillatorPitchLFODepth=300 PitchLFOWave=3 PitchLFOKeyOnReset=1 "
                                "Filter1FilterType=1 Filter1FilterCutoff=500 Filter1FilterCutoffLFODepth=352 FilterCutoffLFOWave=3 FilterCutoffLFOKeyOnReset=1 "
                                "FilterCutoffLFOSpeed=150 AmplitudeLevelLFODepth=192 AmplitudeLFOWave=3 AmplitudeLFOKeyOnReset=1 AmplitudeLFOSpeed=150 ";
        auto same = [&](const std::string& a, const std::string& b, const char* what) {
            std::vector<float> x, y;
            harmonics((lfo + a).c_str(), 48, h, 1, 4410, 88200, &x); harmonics((lfo + b).c_str(), 48, h, 1, 4410, 88200, &y);
            double d = 0, e = 0; for (size_t i = 0; i < x.size(); ++i) { d += double(x[i] - y[i]) * (x[i] - y[i]); e += double(y[i]) * y[i]; }
            std::printf("box: %s: %.1e relative\n", what, std::sqrt(d / (e + 1e-30)));
            CHECK(e > 0 && d < 1e-6 * e);
        };
        same(box + "Box1ControllerSetDestination=119 Box1ControllerSetRatio=136 PitchLFOSpeed=150 FilterCutoffLFOSpeed=150 AmplitudeLFOSpeed=150",
             "PitchLFOSpeed=201 FilterCutoffLFOSpeed=201 AmplitudeLFOSpeed=201", "every LFO's speed (119) +8 against the static 201");
        same(box + "Box2ControllerSetSwitch=1 Box2ControllerSetSource=8 Box2ControllerSetPolarity=0 Box1ControllerSetDestination=119 Box1ControllerSetRatio=136 "
                   "Box2ControllerSetDestination=401 Box2ControllerSetRatio=136 PitchLFOSpeed=150 FilterCutoffLFOSpeed=150 AmplitudeLFOSpeed=150",
             "PitchLFOSpeed=252 FilterCutoffLFOSpeed=201 AmplitudeLFOSpeed=201", "119 with 401, against the static 252");
        same(box + "Box1ControllerSetDestination=119 Box1ControllerSetRatio=136 Osc1OscillatorWave=2 Osc1OscillatorPulseWidthLFODepth=192 "
                   "Osc1OscillatorLFOWave=3 Osc1OscillatorLFOSpeed=150 Osc1OscillatorLFOKeyOnReset=1 PitchLFOSpeed=150",
             "Osc1OscillatorWave=2 Osc1OscillatorPulseWidthLFODepth=192 Osc1OscillatorLFOWave=3 Osc1OscillatorLFOSpeed=201 Osc1OscillatorLFOKeyOnReset=1 "
             "PitchLFOSpeed=201 FilterCutoffLFOSpeed=201 AmplitudeLFOSpeed=201", "119 on the oscillator's LFO");
        // the amplitude LFO's depth (150; C: the level's swing at speed 200): span 127, +8 / +16 / -16 / +64 from 128 and +16 from 160
        // swing as the static 143 / 159 / 97 / 255 / 191 (1.871 / 4.484 / 4.486 / 95.655 / 11.582 dB against 1.870 / 4.485 / - / 95.652 /
        // 11.584)
        for (const auto& c : {std::array<int, 3>{128, 136, 143}, {128, 144, 159}, {128, 112, 97}, {128, 192, 255}, {160, 144, 191}})
            same(box + "Box1ControllerSetDestination=150 Box1ControllerSetRatio=" + std::to_string(c[1]) + " AmplitudeLevelLFODepth=" + std::to_string(c[0]),
                 "AmplitudeLevelLFODepth=" + std::to_string(c[2]), "the amplitude LFO's depth (150) against its static landing");
        // the unison detune and spread (375, 376; D: the copies' beat rate and each copy's level left and right): span 16 within 0-15,
        // +8 / +16 / +32 / +64 from 0 and -16 from 15 beat as the static 2 / 4 / 8 / 15 / 11 (within 0.002 Hz); the spread's +16 / +32 /
        // +64 as the static 4 / 8 / 15 (within 0.01 dB)
        for (const auto& c : {std::array<int, 4>{375, 0, 136, 2}, {375, 0, 144, 4}, {375, 0, 160, 8}, {375, 0, 192, 15}, {375, 15, 112, 11},
                              {376, 0, 144, 4}, {376, 0, 160, 8}, {376, 0, 192, 15}}) {
            const std::string what = c[0] == 375 ? "UnisonDetune=" : "UnisonSpread=", other = c[0] == 375 ? " UnisonSpread=5" : " UnisonDetune=6";
            same(box + "Unison=1" + other + " Box1ControllerSetDestination=" + std::to_string(c[0]) + " Box1ControllerSetRatio=" + std::to_string(c[2]) + " " +
                 what + std::to_string(c[1]), "Unison=1" + other + " " + what + std::to_string(c[3]), "the unison detune or spread against its static landing");
        }
        Gap g[64]; Patch q; char e[400];
        readPatch(("vcore-patch/1 " + lfo + box + "Box1ControllerSetDestination=119 Box1ControllerSetRatio=136 PitchLFOSpeed=150").c_str(), q, e, sizeof e);
        q.set(Filter1FilterResonance, 0);
        CHECK(notCovered(q, g, 64) == 0);
        q.set(AmplitudeLFOSpeed, 300);  // +51: past 320, audio rate
        CHECK(notCovered(q, g, 64) == 1 && g[0].param == AmplitudeLFOSpeed);
    }
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
    q.set(VoltageDrift, 100); q.set(Ageing, 200);  // va-round8 D: at drift 100, ageing 200 against 100 within 12 voices' scatter
    CHECK(notCovered(q, g, 64) == 0);
    q.set(AmplitudeEGReleaseTime, 81);  // va-round8 R2: ageing moves the voices' spread of short releases (0 / 100 / 200 at 55: sd 0.87 / 0.57 / 0.25 ms)
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Ageing);
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
// Amplitude level velocity sensitivity (va-coverage-2026-11-22 G: a sine at key 60, the level at each velocity against the same sound at
// sensitivity 0): the reference's readings at four sensitivities, and the coverage limit (128 steps either way, and 192).
static void testVelocitySensitivity() {
    auto level = [](int sens, int vel) {
        Patch p; char e[300];
        readPatch(("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 AmplitudeEGAttackTime=0 Filter1FilterType=0 Filter2FilterType=0 "
                   "Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 AmplitudeLevel=400 AmplitudeLevelVelocitySensitivity=" +
                   std::to_string(sens)).c_str(), p, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, p); c->setGain(0, 1, 0); c->noteOn(0, 0, 0, 60, float(vel) / 127);
        std::vector<float> l(24000), r(24000); c->render(l.data(), r.data(), 24000);
        double e2 = 0; for (size_t i = 12000; i < l.size(); ++i) e2 += double(l[i]) * l[i];
        return 10 * std::log10(e2 + 1e-30);
    };
    const double ref = level(256, 100);
    const struct { int sens, vel; double db; } pts[] = {{320, 1, -11.90}, {320, 64, -4.99}, {320, 127, 0.0}, {264, 1, -1.10}, {128, 64, -12.05},
                                                        {128, 1, -0.14}, {448, 100, -9.71}, {448, 120, -2.04}, {64, 16, -5.00}, {352, 32, -14.35}};
    for (const auto& t : pts) {
        const double d = level(t.sens, t.vel) - ref;
        std::printf("velocity sensitivity %+d, velocity %d: %+.2f dB (reference %+.2f)\n", t.sens - 256, t.vel, d, t.db);
        CHECK(std::fabs(d - t.db) < 0.15);
    }
    CHECK(level(256, 1) - ref == 0);
    Gap g[64]; Patch q;
    for (int sv : {256 + 128, 256 - 128, 256 + 192, 256 - 192, 300}) { q.set(AmplitudeLevelVelocitySensitivity, sv); CHECK(notCovered(q, g, 64) == 0); }
    for (int sv : {256 + 129, 256 - 160, 511, 1}) { q.set(AmplitudeLevelVelocitySensitivity, sv); CHECK(notCovered(q, g, 64) == 1 && g[0].param == AmplitudeLevelVelocitySensitivity); }
}

// The pan box (118; va-coverage-2026-11-22 E, P): a balance, the far side down by its step's attenuation: +8 / +16 / +24 / -16 at a full
// box (16 / 32 / 48 / -32 steps) -> left -1.505 / -6.397 / -17.685 dB, right -6.397; +32 (64 steps) silent on the left; the near side unmoved.
static void testPanBox() {
    auto lr = [](int ratio, double* l, double* r) {
        Patch p; char e[300];
        readPatch(("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter1FilterType=0 Filter2FilterType=0 Osc1OscillatorWave=4 "
                   "Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 PartAssignableKnob1Value=1023 Box1ControllerSetSwitch=1 "
                   "Box1ControllerSetSource=8 Box1ControllerSetPolarity=0 Box1ControllerSetDestination=118 Box1ControllerSetRatio=" + std::to_string(128 + ratio)).c_str(), p, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, p); c->setGain(0, 1, 0); c->noteOn(0, 0, 0, 60, 0.8f);
        std::vector<float> L(24000), R(24000); c->render(L.data(), R.data(), 24000);
        double a = 0, b = 0; for (size_t i = 12000; i < L.size(); ++i) { a += double(L[i]) * L[i]; b += double(R[i]) * R[i]; }
        *l = 10 * std::log10(a + 1e-30); *r = 10 * std::log10(b + 1e-30);
    };
    double l0, r0; lr(0, &l0, &r0);
    const struct { int ratio; double l, r; } pts[] = {{8, -1.505, 0}, {16, -6.397, 0}, {24, -17.685, 0}, {-16, 0, -6.397}};
    for (const auto& t : pts) {
        double l, r; lr(t.ratio, &l, &r);
        std::printf("pan box %+d: left %+.3f right %+.3f dB (reference %+.3f / %+.3f)\n", t.ratio, l - l0, r - r0, t.l, t.r);
        CHECK(std::fabs(l - l0 - t.l) < 0.01 && std::fabs(r - r0 - t.r) < 0.01);
    }
    double l, r; lr(32, &l, &r);
    CHECK(l < -200 && std::fabs(r - r0) < 0.01);
    Gap g[64]; Patch q; q.set(Box1ControllerSetSwitch, 1); q.set(Box1ControllerSetDestination, 118);
    CHECK(notCovered(q, g, 64) == 0);
}

// Amplitude level key follow (va-coverage-2026-11-22 H: a sine at amplitude level 400, against key follow 0 at the same key): linear in
// the key from key 60, the slope by the key follow; a raise stops at the level's top (+4.256 dB over 400).
static void testKeyFollow() {
    auto level = [](int follow, int key) {
        Patch p; char e[300];
        readPatch(("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter1FilterType=0 Filter2FilterType=0 Osc1OscillatorWave=4 "
                   "Osc1OscillatorOutLevel=511 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 AmplitudeLevel=400 AmplitudeLevelKeyFollow=" + std::to_string(follow)).c_str(), p, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, p); c->setGain(0, 1, 0); c->noteOn(0, 0, 0, key, 0.8f);
        std::vector<float> l(24000), r(24000); c->render(l.data(), r.data(), 24000);
        double e2 = 0; for (size_t i = 12000; i < l.size(); ++i) e2 += double(l[i]) * l[i];
        return 10 * std::log10(e2 + 1e-30);
    };
    const struct { int follow, key; double db; } pts[] = {{8, 24, 1.128}, {8, 108, -1.500}, {16, 96, -2.256}, {64, 84, -6.019}, {127, 96, -18.075},
                                                          {127, 72, -6.025}, {32, 24, 4.255}, {127, 36, 4.254}, {96, 60, 0.0}};
    for (const auto& t : pts) {
        const double d = level(t.follow, t.key) - level(0, t.key);
        std::printf("key follow %d at key %d: %+.3f dB (reference %+.3f)\n", t.follow, t.key, d, t.db);
        CHECK(std::fabs(d - t.db) < 0.05);
    }
    Gap g[64]; Patch q; q.set(AmplitudeLevelKeyFollow, 127);
    CHECK(notCovered(q, g, 64) == 0);
}

// Filter 1's out level (va-coverage-2026-11-22 I: a sine through LPF24 at 1023, LPF12 and thru, against out level 64): 32 / 48 / 80 / 96
// -> -12.041 / -6.021 / +6.020 / +12.041 dB (thru 96: +12.045); covered at any value on filter 1.
static void testOutLevel() {
    auto level = [](int type, int out) {
        Patch p; char e[300];
        readPatch(("vcore-patch/1 VoltageDrift=0 AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 Filter2FilterType=0 Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 "
                   "Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 AmplitudeLevel=200 Filter1FilterCutoff=1023 Filter1FilterResonance=0 Filter1FilterType=" + std::to_string(type) +
                   " Filter1FilterOutLevel=" + std::to_string(out)).c_str(), p, e, sizeof e);
        auto c = std::make_unique<Core>(); c->prepare(48000); c->load(0, p); c->setGain(0, 1, 0); c->noteOn(0, 0, 0, 60, 0.8f);
        std::vector<float> l(24000), r(24000); c->render(l.data(), r.data(), 24000);
        double e2 = 0; for (size_t i = 12000; i < l.size(); ++i) e2 += double(l[i]) * l[i];
        return 10 * std::log10(e2 + 1e-30);
    };
    const struct { int type, out; double db; } pts[] = {{1, 32, -12.041}, {1, 48, -6.021}, {1, 80, 6.020}, {1, 96, 12.041}, {3, 32, -12.041}, {0, 96, 12.045}};
    for (const auto& t : pts) {
        const double d = level(t.type, t.out) - level(t.type, 64);
        std::printf("filter 1 type %d out level %d: %+.3f dB (reference %+.3f)\n", t.type, t.out, d, t.db);
        CHECK(std::fabs(d - t.db) < 0.01);
    }
    Gap g[64]; Patch q; q.set(Filter1FilterOutLevel, 80);
    CHECK(notCovered(q, g, 64) == 0);
}

static void testSaturators() {
    const std::string sine = "Osc1OscillatorWave=4 Osc1OscillatorOutLevel=511 Filter1FilterType=1 Filter1FilterCutoff=1023 ";
    double h0[2], h[2];
    harmonics(sine.c_str(), 36, h0, 2);
    const struct { const char* extra; double gain, h2; } cases[] = {
        {"Filter1FilterSaturatorDrive=7", 1.070, -56.5}, {"Filter1FilterSaturatorDrive=15", 1.072, -50.8}, {"Filter1FilterSaturatorDrive=23", 0.727, -44.7},
        {"Filter2FilterType=5 Filter2FilterSaturatorDrive=15", 1.072 - 0.65, -50.8}, {"AmplitudeSaturatorDrive=8", 0.609, -59.7},
        // va-coverage-2026-11-22 S (every drive 1-80, the same probe): filter 11 / 30, amplitude 20 / 32
        {"Filter1FilterSaturatorDrive=11", 1.316, -53.3}, {"Filter1FilterSaturatorDrive=30", 1.581, -39.1}, {"AmplitudeSaturatorDrive=20", 1.291, -50.5},
        {"AmplitudeSaturatorDrive=32", 1.243, -41.6}};
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
    // no DC (va-round7 X1: the reference's mean over whole periods under 1e-5 of the fundamental at every drive; c x^2 alone gives 6e-3 at 23)
    for (const char* extra : {"Filter1FilterSaturatorDrive=23", "AmplitudeSaturatorDrive=23"}) {
        std::vector<float> L; double h[1];
        harmonics((sine + extra).c_str(), 36, h, 1, 0, 2 * 44100, &L);
        const size_t from = 22050, n = size_t(44100 / 65.406 * 40);  // 40 whole periods
        double m = 0; for (size_t i = from; i < from + n; ++i) m += L[i];
        m /= double(n);
        const double a = std::sqrt(std::pow(10, h[0] / 10)) * 4 / double(2 * 44100);  // the fundamental's amplitude (Hann window: sum w = n / 2)
        std::printf("saturator %s: mean %.2e of the fundamental\n", extra, std::fabs(m) / a);
        CHECK(std::fabs(m) < 1e-4 * a);
    }
    Gap g[64]; Patch q = plain();
    q.set(Filter1FilterSaturatorDrive, 23); q.set(Filter2FilterSaturatorDrive, 15); q.set(AmplitudeSaturatorDrive, 8);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Filter1FilterSaturatorDrive);  // filter 2 a filter: filter 1's runs after it
    q = Patch();  // the model's default: filter 2 a high-pass at 0 (under 5.2 Hz, its gain only)
    q.set(Filter1FilterSaturatorDrive, 23); q.set(Filter2FilterSaturatorDrive, 15); q.set(AmplitudeSaturatorDrive, 8);
    CHECK(notCovered(q, g, 64) == 0);
    q.set(Filter1FilterSaturatorDrive, 32); q.set(AmplitudeSaturatorDrive, 32);
    CHECK(notCovered(q, g, 64) == 0);  // every drive to 32 (va-coverage-2026-11-22 S)
    q.set(Filter1FilterSaturatorDrive, 33);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == Filter1FilterSaturatorDrive);
    q.set(Filter1FilterSaturatorDrive, 32); q.set(AmplitudeSaturatorDrive, 33);
    CHECK(notCovered(q, g, 64) == 1 && g[0].param == AmplitudeSaturatorDrive);
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
    testText(); testCoverage(); testCutoffAgainstReference(); testFilterTypes(); testCalibration(); testOscLevel(); testAlias(); testReleaseThreshold(); testQuietEnd(); testLongSegment(); testBoxLaw(); testPitchDepthKnots(); testCurves();testFegDepthZero(); testTimeLaws(); testDrift(); testInterleaveIdentical(); testVoiceLifetime(); testPitchLfo(); testBoxDestinations(); testAgeing(); testPressureSource(); testWaveBox(); testWaves(); testSaturators(); testVelocitySensitivity(); testPanBox(); testKeyFollow(); testOutLevel(); testPortamento(); testPortamentoBox(); testMonoAndSingle(); testUnison(); testUnisonNotes(); testStatistics();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
