// SPDX-License-Identifier: AGPL-3.0-or-later
// The interface between the plugin shell (plugin.cpp: CLAP, sound contexts, voice budget, note
// lifetime, state) and the DSP core. The core implements Engine; the shell never looks inside it.
//
// Voices are slots 0..kVoices-1. The shell owns the slot assignment: it picks a free slot (or
// frees one) and starts the note there, so a slot is one note's voice and the shell's record of
// the note has the same index. The core ends a voice by itself when its release finishes
// (voice(i).sounding turns false); the shell then ends the note.
//
// Threads: audio thread = process(); "main" = any other. Nothing the audio thread calls may
// allocate, lock or block.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "../core/vcore.h"

namespace va {

constexpr uint32_t kVoices = 64;   // the pool: the most voices the core can sound at once
constexpr uint32_t kContexts = 8;  // sound contexts (slot 0 is the default sound)

// The parameters, per sound. level: linear, 1 = the patch's own level; polyphony: the most voices
// this sound may keep (1..kVoices).
// knob1..knob8: the part's knobs 1-8 (raw 0..1023 as 0..1), live: the controller boxes they drive
// follow at the next control step. A sound starts at its patch's own knob values. Ids are stable (docs/parameters.md).
// ribbon, switch1, switch2: the part's ribbon and switches 1-2 as controller box sources (0..1 as a 7-bit controller's 0..127).
// The parameters before kStateCount are the shell's values (saved in the state's value list). From kDest0 on: the controller
// box destinations the core moves (vcore::Dest order), in the patch's raw units: a value event sets the destination, a
// modulation adds raw steps to it with its boxes (docs/parameters.md); their values live in the raw patch.
enum Param : uint32_t { kLevel, kPan, kPolyphony, kKnob1, kKnob8 = kKnob1 + 7, kRibbon, kSwitch1, kSwitch2, kStateCount,
                        kDest0 = kStateCount, kDestLast = kDest0 + vcore::kDests - 1, kParamCount };
struct ParamInfo { const char* name; const char* key; double min, max, def; bool stepped; };
inline constexpr vcore::Param kDestRaw[vcore::kDests] = {  // each destination parameter's raw patch parameter
    vcore::Osc1OscillatorPitchLFODepth, vcore::Osc2OscillatorPitchLFODepth, vcore::Osc1OscillatorOutLevel, vcore::Osc2OscillatorOutLevel,
    vcore::Filter1FilterCutoff, vcore::Filter1FilterCutoffEGDepth, vcore::Filter1FilterResonance, vcore::FilterCutoffEGAttackTime,
    vcore::FilterCutoffEGDecayTime, vcore::FilterCutoffEGReleaseTime, vcore::AmplitudeEGAttackTime, vcore::AmplitudeEGDecayTime,
    vcore::AmplitudeEGReleaseTime, vcore::Osc3OscillatorPitchLFODepth};
inline constexpr double kKnobDefault = 512.0 / 1023;  // the model's default knob value
inline constexpr ParamInfo kParams[] = {
    {"Level", "level", 0, 1, 1, false}, {"Pan", "pan", -1, 1, 0, false}, {"Polyphony Limit", "polyphony", 1, kVoices, kVoices, true},
    {"Knob 1", "knob1", 0, 1, kKnobDefault, false}, {"Knob 2", "knob2", 0, 1, kKnobDefault, false},
    {"Knob 3", "knob3", 0, 1, kKnobDefault, false}, {"Knob 4", "knob4", 0, 1, kKnobDefault, false},
    {"Knob 5", "knob5", 0, 1, kKnobDefault, false}, {"Knob 6", "knob6", 0, 1, kKnobDefault, false},
    {"Knob 7", "knob7", 0, 1, kKnobDefault, false}, {"Knob 8", "knob8", 0, 1, kKnobDefault, false},
    {"Ribbon", "ribbon", 0, 1, 0, false}, {"Switch 1", "switch1", 0, 1, 0, true}, {"Switch 2", "switch2", 0, 1, 0, true},
    {"Osc 1 Pitch LFO Depth", "", 0, 494, 247, false}, {"Osc 2 Pitch LFO Depth", "", 0, 494, 247, false},
    {"Osc 1 Level", "", 0, 511, 511, false}, {"Osc 2 Level", "", 0, 511, 0, false}, {"Cutoff", "", 0, 1023, 1023, false},
    {"Filter EG Depth", "", 64, 448, 256, false}, {"Resonance", "", 0, 255, 0, false}, {"Filter EG Attack", "", 0, 255, 0, false},
    {"Filter EG Decay", "", 0, 255, 160, false}, {"Filter EG Release", "", 0, 255, 160, false}, {"Amp EG Attack", "", 0, 255, 0, false},
    {"Amp EG Decay", "", 0, 255, 115, false}, {"Amp EG Release", "", 0, 255, 160, false}, {"Osc 3 Pitch LFO Depth", "", 0, 494, 247, false}};
static_assert(sizeof kParams / sizeof kParams[0] == kParamCount, "one entry a parameter");

// A float that can be copied (a Patch is copied) and read by one thread while another writes it.
struct Cell {
    std::atomic<float> v{0};
    Cell() = default;
    Cell(const Cell& o) : v(o.v.load(std::memory_order_relaxed)) {}
    Cell& operator=(const Cell& o) { set(o.get()); return *this; }
    float get() const { return v.load(std::memory_order_relaxed); }
    void set(float x) { v.store(x, std::memory_order_relaxed); }
};
// One sound's values, as the shell holds, saves and loads them: the shell's parameters and the
// sound itself (the reference model's raw parameters, core/params.inc).
struct Patch {
    Cell p[kParamCount];
    vcore::Patch raw;
    Patch() { for (uint32_t i = 0; i < kParamCount; ++i) p[i].set(float(kParams[i].def)); }
};
// Whether a note sounds at all in this patch (its key and velocity limits); velocity 0..1.
bool keySounds(const Patch&, int16_t key, double velocity);

struct VoiceInfo {
    bool sounding = false;   // false: the slot is free (never started, ended, killed or faded out)
    bool releasing = false;  // in its release, or fading out after choke()
    float level = 0;         // linear, 1 at full: envelope, velocity and volume; not the patch's level
    uint64_t age = 0;        // frames since noteOn
};

struct EngineMemory { uint64_t shared, perContext; };  // bytes, for oscairn.memory
// A sound's estimated cost before any note sounds (for oscairn.voice-budget): one voice for a block of
// maxBlock frames, and the engine with no voice sounding, in microseconds on the device.
struct SoundCost { double voiceMicros, restMicros; };

class Engine {
public:
    virtual ~Engine() = default;
    // Main thread, before processing. May allocate; may drop every loaded patch and voice.
    virtual void prepare(double sampleRate, uint32_t maxBlock) = 0;
    virtual EngineMemory memory(double sampleRate, uint32_t maxBlock) const = 0;
    // Main thread, also while processing: the sound for `context` (kContexts slots), which the
    // audio thread will not use before the shell publishes it. May allocate.
    virtual void loadPatch(uint32_t context, const Patch&) = 0;

    // Audio thread from here on.
    virtual void noteOn(uint32_t voice, uint32_t context, int16_t channel, int16_t key, double velocity) = 0;
    virtual void noteOff(uint32_t voice) = 0;                       // enter the release
    virtual void choke(uint32_t voice, uint32_t fadeFrames) = 0;    // fade out over fadeFrames (the give-up), then free
    virtual void kill(uint32_t voice) = 0;                          // free the slot now; no-op if free
    virtual void setVolume(uint32_t voice, double gain) = 0;        // the note's volume expression, linear 0..4
    virtual void setParam(uint32_t context, Param, double value) = 0;
    virtual void modParam(uint32_t context, Param, double amount) = 0;  // a destination's modulation, raw steps (others: ignored)
    virtual void setPitchBend(int16_t channel, double value) = 0;   // -1..1 of the core's range
    // A channel's controller as a controller box source (vcore::Core::setController: the reference's source number, 0..1);
    // source -1: every source of the channel back to its rest value (reset all controllers).
    virtual void setController(int16_t channel, int source, double value) = 0;
    // Adds the sound of every voice of every context into left/right.
    virtual void render(float* left, float* right, uint32_t frames) = 0;
    virtual uint32_t voiceCount() const = 0;                        // voices sounding, fading ones included
    virtual VoiceInfo voice(uint32_t voice) const = 0;
    virtual SoundCost cost(uint32_t context) const = 0;              // any thread
    virtual void reset() = 0;                                       // every voice off now, controllers to rest
};

std::unique_ptr<Engine> makeEngine();  // the core's factory

}  // namespace va
