// SPDX-License-Identifier: AGPL-3.0-or-later
// Internal plain data of the core (included by vcore.h): a sound with its laws applied, and one
// sound's voices as structure of arrays. Every value here is a bounded float or integer.
#pragma once
#include <cstdint>

namespace vcore {

constexpr int kSub = 16;
// Band-limited oscillator tables: kLevels levels of kTabN samples (plus one guard sample each).
constexpr int kTabN = 2048, kLevels = 41;
constexpr float kTabPeak = 1.281f;  // the largest |sample| of any table (test_core checks it)
// The table sets (laws.cpp bandTables): saw, square (width 0.5), triangle, saw 2, each kLevels levels; then the sine, one level.
enum TabSet { TSaw, TSquare, TTriangle, TSaw2, TSine };  // control rate: at most this many frames between two control steps

// One envelope's laws, per frame at the prepared rate. A segment moves toward its target as
// level = target + (level - target) * 2^(l * frames).
struct Env {
    float la, ld, lr;   // attack, decay, release (log2 of the per-frame coefficient, negative)
    float lrLow;        // release below the low-release threshold (slower)
    float sustain;      // linear 0..1
    float endHigh, endLow;  // release target below zero, as a fraction of the level at note-off
    float threshold;    // the level at note-off that separates the two release laws
};

// The LFOs run at control rate: at most 1/16 cycle a 16-frame step (172 Hz at 44.1 kHz, about speed 340; the reference's speeds
// above 320 are audio rate, not covered). Faster, the per-step values alias and a resonant filter swept by them gains without bound
// (the fuzz test: +17 dBFS from a filter LFO at speed 408 and depth 388 with resonance 209).
constexpr float kLfoMaxRate = 1.f / 256;
// One LFO's laws (pitch, filter cutoff or amplitude): per frame at the prepared rate.
struct Lfo {
    float rate = 0;          // cycles per frame
    uint8_t wave = 0;        // 0 saw (rising from 0), 1 square (+1 first), 2 triangle, 3 sine, 4 random (held a cycle)
    bool reset = false;      // key-on reset: the cycle starts at phase0 on each note
    float phase0 = 0;        // 0..1
    float delay = 0;         // frames from the note-on before the LFO sounds (it runs during the delay)
    float fadeRate = 1e30f;  // 1 / fade-in frames; the fade-in level is 1 - (1 - u)^2, u = 0..1
};

// Controller box destinations the core moves (an oscillator's own destination is one entry per oscillator).
enum Dest : uint8_t { DPitchDepth1, DPitchDepth2, DOscLevel1, DOscLevel2, DCutoff, DFegDepth, DReso, DFegAttack, DFegDecay,
                      DFegRelease, DAmpAttack, DAmpDecay, DAmpRelease, DPitchDepth3, DCutoff2, DFegDepth2, DReso2,
                      DPw1, DPw2, DLfoCutDepth, DLfoSpeed, DFreq1, DFreq2, DFreq3, DPorta, DWave1, DWave2, DWave3, kDests };  // DFreq: cents, no patch parameter
// Box sources: the reference's numbers 0-17 (0 pitch bend, 1 mod wheel, 2 aftertouch, 3-4 foot controllers, 5 foot switch,
// 6 ribbon, 7 breath, 8-15 knobs 1-8, 16-17 switches 1-2); a channel keeps kSources values.
constexpr int kSources = 18;
// The reference's destination numbers the core takes.
constexpr int kDestPitchDepth = 124, kDestOscLevel = 136, kDestFegAttack = 139, kDestFegDecay = 140, kDestFegRelease = 141,
              kDestCutoff = 142, kDestFegDepth = 143, kDestReso = 145, kDestAmpAttack = 147, kDestAmpDecay = 148, kDestAmpRelease = 149,
              kDestFreq = 123, kDestPw = 125, kDestLfoCutDepth = 144, kDestPitchLfoSpeed = 401, kDestWave = 122, kDestPorta = 52;
struct DestDef { int16_t span, lo, hi; };
extern const DestDef kDestDefs[kDests];
// An oscillator's table set for a wave (raw 0-4); the pulse reads the square's set (width 0.5) or, any other width, the saw's twice.
const float* waveTable(int wave, bool square);
// Curves with a measured law (laws.cpp): 0 linear; the others by their knots at kCurveS.
struct CurveDef { uint8_t type, p1, p2; float y[17]; };
constexpr int kCurveCount = 7;
extern const CurveDef kCurves[kCurveCount];
extern const float kCurveS[17];
int findCurve(int bank, int type, int p1, int p2);  // -1: not measured
float curveAt(int id, float s);
// The laws at every raw value, built once per process (laws.cpp): the boxes look them up at control rate.
constexpr float kFegSemisPerStep = 0.5f;  // filter EG depth: semitones of the cutoff a step (laws.cpp)
enum TauKind { TAttack, TAmpDecay, TAmpRelease, TFegDecay, TFegRelease, kTaus };
// The filters (laws.cpp): cutSemi is the log of the reference's cutoff frequency fc (semitones re 1 Hz) at every raw cutoff;
// hpGain the high-passes' (HPF24, 18, 12, 6) output gain at whole semitones of fc from 1 Hz; resGain
// the pass-band gain of each resonance value. kTopSemi: 22050 Hz.
constexpr int kHpN = 182;
constexpr float kTopSemi = 173.2756f;
constexpr int kFilterTypes = 9;  // thru, LPF24, LPF18, LPF12, LPF6, HPF24, HPF18, HPF12, HPF6 (the band-passes are not built)
struct LawTables { float cutSemi[1024], hpGain[4][kHpN], pitchSemi[495], oscLevel[512], resGain[256], invTau[kTaus][256], lfoHz[416], portaRate[128]; };
const LawTables& lawTables();

struct Kernel;  // core.cpp
struct Sound {
    bool loaded = false;
    PlanDesc plan{};
    const Kernel* kernel = nullptr;
    float osc[3] = {};      // linear output level of each oscillator (0 when off)
    float pw[3] = {};       // pulse width 0..1
    float pwRaw[2] = {};    // the pulse width's raw value (0..255) and its LFO depth in raw steps at LFO value 1
    float pwDepth[2] = {};
    const float* tab[3] = {};  // each oscillator's band-limited table set
    float stride[3] = {};   // floats from one level to the next (0: a one-level set, the sine)
    float pitch[3] = {};    // log2 of the phase increment at key 0, at the prepared rate
    float tabU = 0;         // 4 log2(F / sample rate): the table level is tabU - 4 log2(increment)
    float keyOffset = 0;    // semitones added to the key (zone octave and transpose, note shift)
    // Portamento: a new note's pitch starts at the previous note's and moves linearly to its own: portaRate semitones a frame,
    // or (time modes 1 and 3) the whole way in the time an octave takes at that rate; modes 2 and 3 start at most an octave away.
    // portaFull: every note glides; else (fingered) only while another key of the sound is held.
    bool porta = false, portaFull = true, mono = false, single = false;
    uint8_t portaTimeMode = 0;
    float portaRate = 0;
    float bendUp = 0, bendDown = 0;  // semitones at full bend
    // Voltage drift, per voice and oscillator (semitones): mean + a static deviation (uniform +-driftStatic) + a
    // random movement driftAmp x r(t), r linear between uniform values in [-1, 1] drawn every driftPeriod frames.
    float driftMean = 0, driftStatic = 0, driftAmp = 0, driftRate = 0;  // driftRate: knots per frame
    float driftCutMean = 0, driftCutSpread = 0;                          // cutoff: static per voice (semitones)
    // Filters 1 and 2 (laws.cpp): the type, the cutoff (semitones re 1 Hz of the reference's fc), the filter EG's move at EG level 1
    // (semitones), the resonance (raw), key follow (semitones a key from key 60), and the velocity's laws (cutoff semitones x 8, EG
    // depth factor x 128, resonance steps at each velocity 0..127). The cutoff, EG depth and resonance are box destinations.
    struct Filter { uint8_t type = 0; float cut = 0, feg = 0, res = 0, keyFollow = 0; };
    Filter flt[2];
    int16_t velCut[2][128] = {};
    int8_t velRes[2][128] = {};
    uint8_t velFeg[2][128] = {};  // the EG depth factor x 128
    float rateRatio = 1;    // 44100 / the prepared rate: the filters' poles are the reference's at 44.1 kHz (1: no mapping)
    int16_t cutOffset = 0;  // the part's cutoff offset in raw steps (a destination value set from outside gets it too)
    float fegOffSteps = 0;  // the part's FEG Depth offset in depth steps (added before the depth's stop, also under the boxes)
    // Controller boxes (laws.cpp boxRaw): a box's output is (c + off - 1024) x ratio / 8 within +-8192, c its curve at the
    // source's value (0..1023); a destination moves by its boxes' sum (within +-8192) x span / 8192 raw steps.
    struct Box { uint8_t src, curve; float ratio, off; };
    struct Target { uint8_t box, dest; };
    Box box[16] = {};
    Target target[32] = {};
    uint8_t boxes = 0, targets = 0;
    int16_t stored[kDests] = {};  // each destination's raw value before the boxes
    float knob[8] = {};      // the patch's own knob values, 0..1
    float invSrLn2 = 0;      // 1 / (sample rate x ln 2): an envelope time's per-frame log2 coefficient is -invTau x this
    Env amp{}, feg{};
    Lfo lfo[3];              // pitch, filter cutoff, amplitude
    Lfo oscLfo[2];           // oscillators 1-2's own LFOs (the pulse width's)
    float lfoPitch[3] = {};  // semitones at LFO value 1, per oscillator
    float lfoCut = 0;        // filter LFO: semitones of cutoff at LFO value 1
    float lfoCutPerStep = 0; // the same a step of its depth (a box moves the depth)
    float lfoAmp = 0;        // amplitude LFO: log2 gain = lfoAmp x value - |lfoAmp| (the top is the level; the sign inverts)
    float level = 0;        // linear: amplitude level law, filter out level, calibration (and the saturators' gain)
    // The saturators (laws.cpp): after the filters, y = x + (satC + satCa e) x^2 per sample (x the filters' output in the engine's units,
    // e the amplitude EG: the amplitude saturator acts after it), their gain in `level`; sat: any saturator acts.
    bool sat = false;
    float satC = 0, satCa = 0;
    float satK = 0;  // the x^2 term's DC is taken out (the reference's output carries none): a one-pole mean of it at 5 Hz, this a frame
    float gain = 1, panL = 0.70710678f, panR = 0.70710678f;  // the caller's level and pan
    // Unison (laws.cpp): a note sounds as `unison` engine voices (1, 2 or 4), each a whole voice with its own free-running phases and
    // drift; copy c is detuned by uniDet[c] semitones and weighed by uniL[c] / uniR[c] (its level and its place in the spread).
    uint8_t unison = 1;
    float uniDet[4] = {}, uniL[4] = {1, 1, 1, 1}, uniR[4] = {1, 1, 1, 1};
};
// The destinations' raw values with the boxes at the given source values (src: 0..1023 per source; knob: 0..1).
// stored: each destination's value before the boxes (null: the sound's own); mod: added to each destination's box sum before
// its clamp (null: none).
void boxRaw(const Sound& s, const float* src, const float* knob, int16_t* raw, const int16_t* stored = nullptr, const float* mod = nullptr);

// Per-voice fields. Oscillators, filters, amplitude ramps, envelopes, voice data.
enum Field {
    PH1, PH2, PH3, INC1, DINC1, INC2, DINC2, INC3, DINC3, TA1, TB1, TW1, DTW1, TA2, TB2, TW2, DTW2, TA3, TB3, TW3, DTW3,
    PW1, DPW1, PW2, DPW2, LO1, LO2, LOR1, LOR2,
    L1, L2, L3, L4, LF, DLF, LQA, DLQA, LQB, DLQB, LFB, DLFB,
    H1, H2, H3, H4, HF, DHF, HQA, DHQA, HQB, DHQB, HFB, DHFB,
    GL, DGL, GR, DGR,
    AENV, ASTG, AEND, ALOW, FENV, FSTG, FEND, FLOW,
    LP0, LP1, LP2, LR0, LR1, LR2, LT,
    KEY, GOFF, GRATE, DS1, DS2, DS3, DA1, DB1, DP1, DA2, DB2, DP2, DA3, DB3, DP3, DCUT, DCUT2, VFEG, VFEG2, VRES, VRES2, VOL, FADE, DFADE, FRESH, SATC, SDC, UDET, UGL, UGR,
    QEND,  // released: the amplitude envelope level at or under which the note's output gain is below -96 dBFS (0: none; the end test only)
    NFIELDS
};
constexpr int kStageAttack = 0, kStageDecay = 1, kStageRelease = 2;

struct Pool {
    alignas(64) float f[NFIELDS][kMaxVoices];
    uint8_t voice[kMaxVoices];   // the caller's voice slot of each position
    uint8_t channel[kMaxVoices];
    uint32_t rng[kMaxVoices];    // each voice's random source (drift knots)
    uint32_t n = 0;              // voices 0..n-1 are live
};

}  // namespace vcore
