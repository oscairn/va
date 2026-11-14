// SPDX-License-Identifier: AGPL-3.0-or-later
// Bench, cost and render driver for the DSP core (core/), 128-frame blocks.
//   vcbench render PATCH RATE SECONDS OUT.f32 EVENT...   stereo interleaved float32; events on@t:key:vel(0-127) off@t:key,
//                                                         applied on their exact frame; also cc@t:number:value (the box
//                                                         sources' MIDI controllers, as the plugin maps them) and pb@t:value
//                                                         (14-bit, 8192 centre)
//   vcbench cg CASE WARM MEAS [whole] [lanes1] TSV       one callgrind dump per measured block (or one for the window)
//   vcbench time [CPU]                                   thread-CPU and wall time per block for every case (also 8 held
//                                                         voices of "probeA-boxes": the LFOs, 16 boxes, moving controllers;
//                                                         and of "probeA-osc3" and "probeA-pwm")
//   vcbench check                                        NaN/Inf/subnormal check over random notes (exit 1 on failure)
//   vcbench coverage PATCH                               what the patch uses that the core does not cover (exit 1 if any)
// CASE: rest | held N | pattern | shed N (N held voices choked in one block, 3 ms fade) ; at 48 kHz, probe A.
// PATCH: a file with a vcore-patch/1 text, "probeA" for the built-in probe A patch, "probeA-boxes" (tools/cost/plans/9-*),
// "probeA-osc3" (10-*) or "probeA-pwm" (11-*).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_ZERO_STATS
#define CALLGRIND_DUMP_STATS
#endif
#include "../core/vcore.h"
#include "../core/vsimd.h"
#ifndef VC_FLAGS
#define VC_FLAGS "?"
#endif

using namespace vcore;
constexpr int B = 128;

// The "probe A" patch: a two-oscillator arpeggiated probe part, alone, effects off (raw values that differ from the defaults).
const char* kProbeA =
#include "probeA.inc"
;

// The time mode's box plan class: probe A with the three LFOs and 16 controller boxes (tools/cost/plans/9-*.patch).
const char* kProbeABoxes = "vcore-patch/1 UnisonDetune=7 UnisonSpread=2 FilterCutoffEGAttackTime=30 FilterCutoffEGDecayTime=117 FilterCutoffEGSustainLevel=41 FilterCutoffEGReleaseTime=145 AmplitudeEGDecayTime=150 AmplitudeEGSustainLevel=140 AmplitudeEGReleaseTime=145 Osc2OscillatorWave=2 Osc2OscillatorPitch=554 Osc2OscillatorOutLevel=330 Osc3OscillatorWave=4 Osc3OscillatorOutLevel=0 Filter1FilterCutoff=636 Filter1FilterCutoffEGDepth=257 Filter1FilterResonance=50 Filter2FilterControllerBox2Switch=0 Filter2FilterControllerBox4Switch=0 PartPortamentoSwitch=0 PartAssignableKnob2Value=1023 PartAssignableKnob3Value=473 PartAssignableKnob4Value=0 PartFEGDepth=65 PartFilterCutoffFrequency=63 ZoneZoneOctaveShift=61 ZoneZoneNoteLimitLow=84 ZoneZoneNoteLimitHigh=96 Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetDestination=143 Box1ControllerSetPolarity=1 Box2ControllerSetSwitch=1 Box2ControllerSetSource=9 Box2ControllerSetDestination=140 Box2ControllerSetPolarity=1 PitchLFOWave=3 PitchLFOSpeed=225 Osc1OscillatorPitchLFODepth=300 Osc2OscillatorPitchLFODepth=300 Filter1FilterCutoffLFODepth=280 AmplitudeLevelLFODepth=160 AmplitudeLFOWave=3 Box3ControllerSetSwitch=1 Box3ControllerSetSource=1 Box3ControllerSetDestination=124 Box3ControllerSetRatio=136 Box3ControllerSetPolarity=0 Box3ControllerSetCurveParameter1=6 Box4ControllerSetSwitch=1 Box4ControllerSetSource=0 Box4ControllerSetDestination=142 Box4ControllerSetRatio=137 Box4ControllerSetPolarity=1 Box5ControllerSetSwitch=1 Box5ControllerSetSource=7 Box5ControllerSetDestination=145 Box5ControllerSetRatio=138 Box5ControllerSetPolarity=0 Box6ControllerSetSwitch=1 Box6ControllerSetSource=3 Box6ControllerSetDestination=136 Box6ControllerSetRatio=139 Box6ControllerSetPolarity=1 Box6ControllerSetCurveParameter1=6 Box7ControllerSetSwitch=1 Box7ControllerSetSource=6 Box7ControllerSetDestination=139 Box7ControllerSetRatio=140 Box7ControllerSetPolarity=0 Box8ControllerSetSwitch=1 Box8ControllerSetSource=16 Box8ControllerSetDestination=140 Box8ControllerSetRatio=136 Box8ControllerSetPolarity=1 Box9ControllerSetSwitch=1 Box9ControllerSetSource=17 Box9ControllerSetDestination=141 Box9ControllerSetRatio=137 Box9ControllerSetPolarity=0 Box9ControllerSetCurveParameter1=6 Box10ControllerSetSwitch=1 Box10ControllerSetSource=10 Box10ControllerSetDestination=147 Box10ControllerSetRatio=138 Box10ControllerSetPolarity=1 Box11ControllerSetSwitch=1 Box11ControllerSetSource=11 Box11ControllerSetDestination=148 Box11ControllerSetRatio=139 Box11ControllerSetPolarity=0 Box12ControllerSetSwitch=1 Box12ControllerSetSource=12 Box12ControllerSetDestination=149 Box12ControllerSetRatio=140 Box12ControllerSetPolarity=1 Box12ControllerSetCurveParameter1=6 Box13ControllerSetSwitch=1 Box13ControllerSetSource=1 Box13ControllerSetDestination=124 Box13ControllerSetRatio=136 Box13ControllerSetPolarity=0 Box14ControllerSetSwitch=1 Box14ControllerSetSource=1 Box14ControllerSetDestination=142 Box14ControllerSetRatio=137 Box14ControllerSetPolarity=1 Box15ControllerSetSwitch=1 Box15ControllerSetSource=0 Box15ControllerSetDestination=143 Box15ControllerSetRatio=138 Box15ControllerSetPolarity=0 Box15ControllerSetCurveParameter1=6 Box16ControllerSetSwitch=1 Box16ControllerSetSource=7 Box16ControllerSetDestination=136 Box16ControllerSetRatio=139 Box16ControllerSetPolarity=1";

struct Ev { long frame; bool on; int key; float vel; int cc = -1; };  // cc >= 0: a controller (key: value); 1000: pitch bend

// The time mode's other plan classes (tools/cost/plans/10-*, 11-*): probe A with a third oscillator, and with a pulse width LFO.
const char* kOsc3 = " Osc3OscillatorWave=4 Osc3OscillatorOutLevel=300";
const char* kPwm = " Osc2OscillatorPulseWidth=61 Osc2OscillatorPulseWidthLFODepth=160 Osc2OscillatorLFOWave=3 Osc2OscillatorLFOSpeed=150";

static Patch loadPatch(const char* arg) {
    std::string text = strcmp(arg, "probeA-boxes") ? kProbeA : kProbeABoxes;
    if (!strcmp(arg, "probeA-osc3")) text += kOsc3;
    if (!strcmp(arg, "probeA-pwm")) text += kPwm;
    if (strcmp(arg, "probeA") && strcmp(arg, "probeA-boxes") && strcmp(arg, "probeA-osc3") && strcmp(arg, "probeA-pwm")) {
        FILE* f = fopen(arg, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", arg); exit(2); }
        text.clear(); char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    }
    Patch p; char err[200];
    if (!readPatch(text.c_str(), p, err, sizeof err)) { fprintf(stderr, "patch: %s\n", err); exit(2); }
    return p;
}

// The engine with voice slots assigned in order (a stand-in for the shell's allocation).
struct Host {
    Core* core = new Core;
    int keyOf[kMaxVoices];
    Host(const Patch& p, double sr) { core->prepare(sr); core->load(0, p); core->setGain(0, 1, 0); std::fill(keyOf, keyOf + kMaxVoices, -1); }
    ~Host() { delete core; }
    void apply(const Ev& e) {
        if (e.cc == 1000) { core->setPitchBend(0, float(e.key - 8192) / (e.key > 8192 ? 8191.f : 8192.f)); return; }
        if (e.cc >= 0) {
            const int src = e.cc == 1 ? 1 : e.cc == 2 ? 7 : e.cc == 11 ? 3 : e.cc == 16 ? 6 : e.cc == 86 ? 16 : e.cc == 87 ? 17 : -1;
            // the ribbon and the switches are the sound's own controls (the plugin's parameters); here the reference's CCs drive them
            if (src == 6 || src >= 16) core->setPanel(0, src, float(e.key) / 127.f);
            else if (src > 0) core->setController(0, src, float(e.key) / 127.f);
            return;
        }
        if (e.on) {
            for (uint32_t v = 0; v < kMaxVoices; ++v)
                if (!core->voice(v).sounding) { core->noteOn(v, 0, 0, e.key, e.vel); keyOf[v] = e.key; return; }
        } else {
            for (uint32_t v = 0; v < kMaxVoices; ++v)
                if (keyOf[v] == e.key && core->voice(v).sounding && !core->voice(v).releasing) { core->noteOff(v); keyOf[v] = -1; return; }
        }
    }
    // one block with sample-accurate events (frames relative to the block)
    void block(float* L, float* R, const Ev* ev, size_t n) {
        std::memset(L, 0, B * 4); std::memset(R, 0, B * 4);
        long at = 0;
        for (size_t i = 0; i < n; ++i) {
            if (ev[i].frame > at) { core->render(L + at, R + at, uint32_t(ev[i].frame - at)); at = ev[i].frame; }
            apply(ev[i]);
        }
        if (at < B) core->render(L + at, R + at, uint32_t(B - at));
    }
};

// the pattern: keys 84 88 91 88 (sounding 48 52 55 52), sixteenths at 84 BPM, gate 0.9, velocity 0.8
static std::vector<Ev> pattern(const std::string& c, int n, long blocks, double sr) {
    std::vector<Ev> evs;
    if (c == "pattern") {
        const double step = 60.0 / 84 / 4 * sr, gate = 0.9 * step;
        const int keys[] = {84, 88, 91, 88};
        for (long k = 0; k * step < double((blocks + 4) * B); ++k) {
            evs.push_back({long(std::floor(k * step)), true, keys[k % 4], 0.8f});
            evs.push_back({long(std::floor(k * step + gate)), false, keys[k % 4], 0.f});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    } else if (c == "held" || c == "shed") {
        for (int k = 0; k < n; ++k) evs.push_back({0, true, 84 + (k % 13), 0.8f});
    }
    return evs;
}

static void blockEvents(const std::vector<Ev>& evs, size_t& ei, long b, std::vector<Ev>& out) {
    out.clear();
    while (ei < evs.size() && evs[ei].frame < (b + 1) * B) { Ev x = evs[ei++]; x.frame -= b * B; out.push_back(x); }
}

static int render(int argc, char** argv) {
    if (argc < 6) return 2;
    const Patch p = loadPatch(argv[2]);
    const double sr = atof(argv[3]), secs = atof(argv[4]);
    std::vector<Ev> evs;
    for (int i = 6; i < argc; ++i) {
        double t; int k, v = 0;
        if (sscanf(argv[i], "on@%lf:%d:%d", &t, &k, &v) == 3) evs.push_back({long(std::lround(t * sr)), true, k, float(v) / 127.f});
        else if (sscanf(argv[i], "off@%lf:%d", &t, &k) == 2) evs.push_back({long(std::lround(t * sr)), false, k, 0.f});
        else if (sscanf(argv[i], "cc@%lf:%d:%d", &t, &k, &v) == 3) evs.push_back({long(std::lround(t * sr)), false, v, 0.f, k});
        else if (sscanf(argv[i], "pb@%lf:%d", &t, &v) == 2) evs.push_back({long(std::lround(t * sr)), false, v, 0.f, 1000});
        else { fprintf(stderr, "bad event %s\n", argv[i]); return 2; }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    Host h(p, sr);
    if (const char* s = getenv("VC_SEED")) h.core->seed(uint32_t(strtoul(s, nullptr, 10)));
    const long blocks = long(secs * sr / B) + 1;
    std::vector<float> out; out.reserve(size_t(blocks * B * 2));
    alignas(64) float L[B], R[B];
    size_t ei = 0; std::vector<Ev> bev;
    for (long b = 0; b < blocks; ++b) {
        blockEvents(evs, ei, b, bev);
        h.block(L, R, bev.data(), bev.size());
        for (int i = 0; i < B; ++i) { out.push_back(L[i]); out.push_back(R[i]); }
    }
    FILE* f = fopen(argv[5], "wb");
    fwrite(out.data(), 4, out.size(), f); fclose(f);
    return 0;
}

static int cg(int argc, char** argv) {
    const std::string c = argv[2];
    const int n = (c == "held" || c == "shed") ? atoi(argv[3]) : 0;
    const int a0 = (c == "held" || c == "shed") ? 4 : 3;
    const double warm = atof(argv[a0]), meas = atof(argv[a0 + 1]);
    FILE* tsv = fopen(argv[argc - 1], "w");  // the last argument; flags may stand between
    bool whole = false, lanes1 = false;
    for (int i = a0 + 2; i < argc - 1; ++i) { whole |= !strcmp(argv[i], "whole"); lanes1 |= !strcmp(argv[i], "lanes1"); }
    const char* pp = getenv("VC_PATCH");  // another patch for the same case (per-stage costs)
    Host h(loadPatch(pp ? pp : "probeA"), 48000);
    if (lanes1) h.core->setInterleave(1);
    const long first = long(warm * 48000 / B), total = first + long(meas * 48000 / B);
    std::vector<Ev> evs = pattern(c, n, total, 48000), bev;
    alignas(64) float L[B], R[B];
    size_t ei = 0; uint64_t hash = 1469598103934665603ULL;
    for (long b = 0; b < total; ++b) {
        blockEvents(evs, ei, b, bev);
        const bool m = b >= first;
        if (c == "shed" && b == first)  // every held voice given up in this block (the shell's 3 ms choke)
            for (uint32_t v = 0; v < kMaxVoices; ++v) if (h.core->voice(v).sounding) h.core->choke(v, 144);
        if (m && (!whole || b == first)) CALLGRIND_ZERO_STATS;
        h.block(L, R, bev.data(), bev.size());
        if (m && !whole) CALLGRIND_DUMP_STATS;
        if (m) fprintf(tsv, "%ld\t%u\t%zu\n", b - first, h.core->voiceCount(), bev.size());
        for (int i = 0; i < B; ++i) for (float x : {L[i], R[i]}) { uint32_t bits; memcpy(&bits, &x, 4); hash = (hash ^ bits) * 1099511628211ULL; }
    }
    if (whole) CALLGRIND_DUMP_STATS;
    fclose(tsv);
    printf("impl %s W=%d lanes %d hash %016llx\n", VC_IMPL, W, lanes1 ? 1 : 2, (unsigned long long)hash);
    return 0;
}

static double us(const timespec& a, const timespec& b) { return double(b.tv_sec - a.tv_sec) * 1e6 + double(b.tv_nsec - a.tv_nsec) * 1e-3; }
static void stats(const char* what, std::vector<double> x) {
    std::sort(x.begin(), x.end()); double m = 0; for (double v : x) m += v; m /= double(x.size());
    printf("  %-4s mean %7.2f median %7.2f p99 %7.2f max %8.2f us\n", what, m, x[x.size() / 2], x[size_t(double(x.size()) * 0.99)], x.back());
}
static int timeAll(int argc, char** argv) {
    printf("build %s W=%d flags %s\n", VC_IMPL, W, VC_FLAGS);
#ifdef __linux__
    if (argc > 2) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(atoi(argv[2]), &s); printf("pin cpu %s: %s\n", argv[2], sched_setaffinity(0, sizeof s, &s) ? "FAILED" : "ok"); }
#else
    (void)argc; (void)argv;
#endif
    struct Case { const char* c; int n; const char* patch; };
    for (int lanes : {2, 1})
        for (Case k : {Case{"rest", 0, "probeA"}, {"held", 1, "probeA"}, {"held", 4, "probeA"}, {"held", 8, "probeA"}, {"held", 16, "probeA"}, {"pattern", 0, "probeA"},
                       {"held", 8, "probeA-boxes"}, {"held", 8, "probeA-osc3"}, {"held", 8, "probeA-pwm"}}) {
            Host h(loadPatch(k.patch), 48000); h.core->setInterleave(lanes);
            const bool moving = !strcmp(k.patch, "probeA-boxes");  // the mod wheel, breath and bend move every block
            const long total = 10 * 48000 / B, first = 4 * 48000 / B;
            std::vector<Ev> evs = pattern(k.c, k.n, total, 48000), bev;
            std::vector<double> wall, cpu; alignas(64) float L[B], R[B]; size_t ei = 0; double vs = 0; float sink = 0;
            for (long b = 0; b < total; ++b) {
                blockEvents(evs, ei, b, bev);
                if (moving) { const float x = float(b % 128) / 127.f; h.core->setController(0, 1, x); h.core->setController(0, 7, 1 - x); h.core->setPitchBend(0, x - 0.5f); }
                timespec c0, w0, w1, c1;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0); clock_gettime(CLOCK_MONOTONIC, &w0);
                h.block(L, R, bev.data(), bev.size());
                clock_gettime(CLOCK_MONOTONIC, &w1); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
                sink += L[b & (B - 1)];
                if (b >= first) { wall.push_back(us(w0, w1)); cpu.push_back(us(c0, c1)); vs += h.core->voiceCount(); }
            }
            printf("case %s %d %s lanes %d: voices mean %.2f (sink %g)\n", k.c, k.n, k.patch, lanes, vs / double(wall.size()), double(sink));
            stats("wall", wall); stats("cpu", cpu);
        }
    return 0;
}

static int check() {
    Host h(loadPatch("probeA"), 44100);
    alignas(64) float L[B], R[B]; uint32_t r = 1;
    auto rnd = [&] { r = r * 1664525u + 1013904223u; return r >> 8; };
    for (long b = 0; b < 44100 * 20 / B; ++b) {
        std::vector<Ev> ev;
        if (rnd() % 4 == 0) ev.push_back({long(rnd() % B), true, 84 + int(rnd() % 13), float(rnd() % 128) / 127.f});
        if (rnd() % 4 == 0) ev.push_back({long(rnd() % B), false, 84 + int(rnd() % 13), 0});
        std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
        h.block(L, R, ev.data(), ev.size());
        for (int i = 0; i < B; ++i)
            for (float x : {L[i], R[i]})
                if (!std::isfinite(x) || std::fpclassify(x) == FP_SUBNORMAL || std::fabs(x) > 4) { printf("bad sample %g at block %ld\n", double(x), b); return 1; }
    }
    printf("check ok\n");
    return 0;
}

int main(int argc, char** argv) {
    const std::string m = argc > 1 ? argv[1] : "";
    if (m == "render") return render(argc, argv);
    if (m == "cg" && argc > 4) return cg(argc, argv);
    if (m == "time") return timeAll(argc, argv);
    if (m == "check") return check();
    if (m == "coverage" && argc > 2) {
        Gap g[256];
        const uint32_t n = notCovered(loadPatch(argv[2]), g, 256);
        for (uint32_t i = 0; i < n && i < 256; ++i) printf("not covered: %s=%d: %s\n", kParamDefs[g[i].param].id, loadPatch(argv[2])[g[i].param], g[i].why);
        printf("%u parameter value(s) not covered\n", n);
        return n ? 1 : 0;
    }
    fprintf(stderr, "usage: see the head of tools/vcbench.cpp\n");
    return 2;
}
