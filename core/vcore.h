// SPDX-License-Identifier: AGPL-3.0-or-later
// The DSP core: a plain C++ library with no dependency on a plugin API, the OS, threads, files or a
// heap. It never allocates; the caller owns the Core object's memory (sizeof(Core), alignof(Core)).
//
// A sound is a Patch: the reference analog model's own parameters in their raw units (params.inc).
// The core applies the reference's measured laws to them (laws.cpp, docs/coverage.md). What it
// does not cover is reported by notCovered(), never approximated in silence.
//
// A sound's voice plan is data (PlanDesc: which stages run, in order); the kernels are plain
// functions over structure-of-arrays voice state, compiled once per plan (core.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace vcore {

// ---- the patch ---------------------------------------------------------------------------------
enum Param : uint16_t {
#define X(id, name, lo, hi, def) id,
#include "params.inc"
#undef X
    kParamCount
};
struct ParamDef { const char* id; const char* name; int16_t lo, hi, def; };
extern const ParamDef kParamDefs[kParamCount];

constexpr int kPatchFormat = 1;  // the text form's version: "vcore-patch/1"
struct Patch {
    int16_t v[kParamCount];
    Patch();  // every parameter at its default
    int operator[](Param p) const { return v[p]; }
    bool set(Param p, int x);  // false (and unchanged) outside the parameter's range
};
bool operator==(const Patch&, const Patch&);
// Text form: "vcore-patch/1" then whitespace-separated Id=value tokens (the ids of params.inc);
// a parameter not named keeps its default. writePatch names only the values that differ.
// writePatch returns the length the text needs (without the NUL) and writes at most cap bytes.
size_t writePatch(const Patch&, char* out, size_t cap);
bool readPatch(const char* text, Patch& out, char* error, size_t errorCap);
int findParam(const char* id, size_t length);  // -1 if none

// Every parameter value the core does not reproduce by a measured law. Returns how many there are
// (all of them, also past cap); fills at most cap entries.
struct Gap { Param param; const char* why; };
uint32_t notCovered(const Patch&, Gap* out, uint32_t cap);
// Whether a key and velocity (0..127) sound at all: the zone's and the part's limits.
bool keySounds(const Patch&, int key, int velocity);

// ---- the plan ----------------------------------------------------------------------------------
enum class Stage : uint8_t { Saw1, Pulse1, Saw2, Pulse2, Filter1, Filter2, Amp, Table3 };
// Saw: one band-limited table read (any wave the tables hold: saw, saw 2, triangle, sine, the square of width 0.5);
// Pulse: two reads of the saw tables (any other width, or a width the oscillator's LFO moves).
enum class Wave : uint8_t { Off, Saw, Pulse };
struct PlanDesc {
    Wave osc[3];             // oscillator 3: Off or Saw only
    bool oscLfo[2];          // oscillators 1-2: their own LFO moves the pulse width
    uint8_t filter[2];       // each filter's type (Filter1FilterType's numbers; 0 thru: no stage; the band-passes are not built)
    bool lfo[3];             // the pitch, filter and amplitude LFOs (control rate; absent: not computed)
    uint8_t boxes;           // controller boxes switched on (control rate, once per sound)
    uint8_t count;           // the stages in signal order
    Stage stage[7];
};
PlanDesc describe(const Patch&);
bool highPassAsGain(const Patch&);  // filter 2 left out of the plan, its pass-band gain applied to the level
// Estimated cost, in instructions (aarch64, -O3, measured; docs/cost.md): one voice for one 128-frame
// block, and the sound's share of a block with no voice sounding.
struct Cost { double voiceBlock, rest; };
Cost estimateCost(const PlanDesc&);

// ---- the engine --------------------------------------------------------------------------------
constexpr uint32_t kMaxVoices = 64;  // voice slots; the caller assigns them
constexpr uint32_t kMaxSounds = 8;   // sound slots (each has its own voices' lanes)
}  // namespace vcore
#include "voices.h"  // Sound (a patch with its laws applied) and Pool (one sound's voices): plain data
namespace vcore {

struct VoiceState { bool sounding, releasing; float level; uint64_t age; };


class Core {
public:
    Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    void prepare(double sampleRate);  // drops every voice; reloads no sound (call load again)
    // Applies the laws to a patch for this core's rate (any thread; uses libm, never allocates).
    void compile(Sound& out, const Patch& patch) const;
    // Puts a compiled sound in a slot (a copy; the slot keeps its gain and pan). Audio thread, or
    // whenever the slot's voices do not render.
    void install(uint32_t sound, const Sound& s);
    void load(uint32_t sound, const Patch& patch) { Sound s; compile(s, patch); install(sound, s); }
    void setGain(uint32_t sound, float gain, float pan);  // the caller's level (linear) and pan (-1..1)

    void noteOn(uint32_t voice, uint32_t sound, int channel, int key, float velocity);  // velocity 0..1
    void noteOff(uint32_t voice);
    void choke(uint32_t voice, uint32_t fadeFrames);  // linear fade, then the slot is free
    void kill(uint32_t voice);                        // free now
    void setVolume(uint32_t voice, float gain);       // the note's volume expression, linear
    void setPitchBend(int channel, float value);      // -1..1 of the patch's bend range (also the boxes' source 0)
    // A channel's controller as a controller box source (vcore source numbers: 1 mod wheel, 2 aftertouch, 3 foot controller 1,
    // 4 foot controller 2, 5 foot switch, 7 breath), 0..1 (a 7-bit controller is value / 127). A sound's boxes read the
    // channel of its latest note. The ribbon (6) and switches 1-2 (16-17) are the sound's own (setPanel), as the knobs. reset() puts every controller at its rest value
    // (the reference's reset: foot controllers and breath at the top, the others at 0, the bend centred).
    void setController(int channel, int source, float value);
    void resetControllers(int channel);               // the channel's box sources to their rest values
    // A sound's knob k (0..7) now, 0..1 (raw 0..1023): the controller boxes it drives follow at the
    // next control step. install() starts a sound at its patch's own knob values.
    void setKnob(uint32_t sound, int k, float value);
    // A sound's ribbon (source 6) or switch 1-2 (sources 16-17) now, 0..1 (as a 7-bit controller: value x 1023, rounded);
    // other sources are ignored. They rest at 0; install() keeps them.
    void setPanel(uint32_t sound, int source, float value);
    // A controller box destination of a sound (vcore::Dest) moved from outside the boxes: setDestination sets its value
    // (the patch's raw value, e.g. Filter1FilterCutoff 0-1023, Filter1FilterCutoffEGDepth 64-448) until the next install;
    // modulateDestination adds `steps` raw steps to it, summed with its boxes' outputs before their clamp (a box sum of
    // steps x 8192 / span), kept across installs. Both follow at the next control step; nothing is recompiled.
    void setDestination(uint32_t sound, int dest, int value);
    void modulateDestination(uint32_t sound, int dest, float steps);
    void reset();                                     // every voice off, bends to rest
    // Adds every voice into left/right. Any frame count; a note starts on the frame its noteOn
    // falls between two render calls.
    void render(float* left, float* right, uint32_t frames);

    uint32_t voiceCount() const;
    VoiceState voice(uint32_t v) const;

    // Test hook: render with 1 or 2 lanes interleaved in the filter loops (2 is the default).
    void setInterleave(int lanes) { interleave_ = lanes == 1 ? 1 : 2; }
    // The random source of free-running phases and drift (tests and measurements; never 0).
    void seed(uint32_t s) { rng_ = s ? s : 1; }

private:
    // parked: a held key whose voice another note took over (a mono sound's, or single key assignment's); owner: that note's slot
    struct Slot { int8_t sound = -1; uint8_t pos = 0; bool parked = false; uint8_t owner = 0; uint64_t age = 0; };
    void park_(uint32_t sound, uint32_t from, uint32_t to);   // `from` parked under `to`; slots parked under `from` follow
    void free_(uint32_t sound, uint32_t pos);
    void glide_(uint32_t sound, uint32_t pos, float from);     // portamento from the pitch `from` to the voice's key
    void retrigger_(uint32_t sound, uint32_t pos);             // the envelopes to their attack from where they are, LFO resets
    float pitchNow_(uint32_t sound) const;                     // the pitch a new note of the sound glides from
    void unhold_(uint32_t sound, uint32_t voice);
    void reap_(uint32_t sound);
    double sr_ = 48000;
    int interleave_ = 2;
    uint32_t rng_ = 0x9e3779b9u;
    float bend_[16] = {};
    float knob_[kMaxSounds][8] = {};
    float panel_[kMaxSounds][3] = {};       // ribbon, switch 1, switch 2: 0..1023
    int16_t dest_[kMaxSounds][kDests] = {}; // each destination's value before the boxes (install: the patch's)
    float mod_[kMaxSounds][kDests] = {};    // modulations, in box output units (+-8192 a full box)
    bool live_[kMaxSounds] = {};            // a destination set or modulated from outside: the boxes' path runs
    float src_[16][kSources] = {};    // each channel's box sources, 0..1023
    uint8_t chan_[kMaxSounds] = {};   // the channel of each sound's latest note
    float lastKey_[kMaxSounds] = {};  // the pitch (semitones, with the sound's offsets) of each sound's latest note
    int16_t lastVoice_[kMaxSounds] = {};  // its voice slot (-1 none)
    struct Held { uint8_t voice; float key; };
    Held held_[kMaxSounds][kMaxVoices] = {};  // a mono sound's keys held, oldest first (the newest owns the voice)
    uint8_t nheld_[kMaxSounds] = {};
    Slot slot_[kMaxVoices];
    Sound sound_[kMaxSounds];
    Pool pool_[kMaxSounds];
};

}  // namespace vcore
