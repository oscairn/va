// SPDX-License-Identifier: AGPL-3.0-or-later
// engine.h on the DSP core (core/). A sound is compiled on the main thread (laws, libm) into a
// triple buffer per sound context, and the audio thread installs the newest one before it renders
// or starts a note: no lock and no data race between the two threads.
#include <atomic>
#include <cmath>
#include <mutex>

#include "engine.h"

namespace va {

bool keySounds(const Patch& p, int16_t key, double velocity) {
    return vcore::keySounds(p.raw, key, int(std::lround(velocity * 127)));
}

namespace {

// Nanoseconds per instruction of this core on the device (Cortex-A53, measured at commit 2549b96: 8 held voices,
// thread CPU time 62.0 us a block against callgrind's 54.9k instructions: 1.13; with a margin; docs/cost.md).
constexpr double kNanosPerInstruction = 1.15;

class CoreEngine final : public Engine {
public:
    void prepare(double sr, uint32_t maxBlock) override {
        std::lock_guard<std::mutex> g(write_);
        core_.prepare(sr > 0 ? sr : 48000);
        maxBlock_ = maxBlock ? maxBlock : 128;
        for (auto& s : staged_) s.middle.store(1, std::memory_order_relaxed), s.back = 0, s.front = 2;
    }
    // everything is preallocated with the instance; a context is declared at the size of its patch (over, not under)
    EngineMemory memory(double, uint32_t) const override { return {sizeof(CoreEngine), sizeof(Patch)}; }
    void loadPatch(uint32_t c, const Patch& p) override {
        if (c >= kContexts) return;
        std::lock_guard<std::mutex> g(write_);
        Staged& s = staged_[c];
        Entry& e = s.buf[s.back];
        vcore::Patch raw = p.raw;  // the knobs as the shell holds them (they start the sound's boxes)
        for (int k = 0; k < 8; ++k) raw.set(vcore::Param(vcore::PartAssignableKnob1Value + k), int(std::lround(p.p[kKnob1 + k].get() * 1023)));
        for (int d = 0; d < vcore::kDests; ++d) if (hasRaw(d)) raw.set(kDestRaw[d], int(std::lround(p.p[paramOf(d)].get())));  // as the shell holds them
        core_.compile(e.sound, raw);
        e.level = p.p[kLevel].get(); e.pan = p.p[kPan].get();
        const auto est = vcore::estimateCost(e.sound.plan);
        cost_[c].voice.store(est.voiceBlock * kNanosPerInstruction * 1e-3 * maxBlock_ / 128, std::memory_order_relaxed);
        cost_[c].rest.store(est.rest * kNanosPerInstruction * 1e-3 * maxBlock_ / 128, std::memory_order_relaxed);
        s.back = s.middle.exchange(s.back | kDirty, std::memory_order_acq_rel) & 3;
    }
    void noteOn(uint32_t v, uint32_t c, int16_t ch, int16_t key, double vel) override {
        sync();
        core_.noteOn(v, c, ch, key, float(vel));
    }
    void noteOff(uint32_t v) override { core_.noteOff(v); }
    void choke(uint32_t v, uint32_t frames) override { core_.choke(v, frames); }
    void kill(uint32_t v) override { core_.kill(v); }
    void setVolume(uint32_t v, double g) override { core_.setVolume(v, float(g)); }
    void setParam(uint32_t c, Param p, double x) override {
        if (c >= kContexts) return;
        sync();  // a sound published before this event is installed first, so the event is not undone
        if (p >= kKnob1 && p <= kKnob8) { core_.setKnob(c, int(p - kKnob1), float(x)); return; }
        if (p == kRibbon || p == kSwitch1 || p == kSwitch2 || p == kFoot2 || p == kFootSwitch) {
            core_.setPanel(c, p == kRibbon ? 6 : p == kSwitch1 ? 16 : p == kSwitch2 ? 17 : p == kFoot2 ? 4 : 5, float(x));
            return;
        }
        if (destOf(p) >= 0) { core_.setDestination(c, destOf(p), int(std::lround(x))); return; }
        if (p == kLevel) level_[c] = float(x);
        else if (p == kPan) pan_[c] = float(x);
        else return;
        core_.setGain(c, level_[c], pan_[c]);
    }
    void modParam(uint32_t c, Param p, double amount) override {
        if (c >= kContexts || destOf(p) < 0) return;
        sync();
        core_.modulateDestination(c, destOf(p), float(amount));
    }
    void setPitchBend(int16_t ch, double x) override { core_.setPitchBend(ch, float(x)); }
    void setController(int16_t ch, int source, double x) override {
        if (source < 0) core_.resetControllers(ch); else core_.setController(ch, source, float(x));
    }
    void render(float* l, float* r, uint32_t n) override { sync(); core_.render(l, r, n); }
    void render(float* const* l, float* const* r, uint32_t n, uint32_t only) override { sync(); core_.render(l, r, n, only); }
    uint32_t voiceCount() const override { return core_.voiceCount(); }
    VoiceInfo voice(uint32_t v) const override {
        const auto s = core_.voice(v);
        return {s.sounding, s.releasing, s.level, s.age};
    }
    SoundCost cost(uint32_t c) const override {
        if (c >= kContexts) return {0, 0};
        return {cost_[c].voice.load(std::memory_order_relaxed), cost_[c].rest.load(std::memory_order_relaxed)};
    }
    void reset() override { sync(); core_.reset(); }

private:
    static constexpr uint32_t kDirty = 4;
    struct Entry { vcore::Sound sound; float level = 1, pan = 0; };
    struct Staged {  // triple buffer: the writer owns `back`, the reader `front`, `middle` is exchanged
        Entry buf[3];
        std::atomic<uint32_t> middle{1};
        uint32_t back = 0, front = 2;
    };
    // audio thread: install every sound the main thread published since the last call
    void sync() {
        for (uint32_t c = 0; c < kContexts; ++c) {
            Staged& s = staged_[c];
            if (!(s.middle.load(std::memory_order_acquire) & kDirty)) continue;
            s.front = s.middle.exchange(s.front, std::memory_order_acq_rel) & 3;
            const Entry& e = s.buf[s.front];
            core_.install(c, e.sound);
            level_[c] = e.level; pan_[c] = e.pan;
            core_.setGain(c, level_[c], pan_[c]);
        }
    }
    vcore::Core core_;
    Staged staged_[kContexts];
    struct { std::atomic<double> voice{0}, rest{0}; } cost_[kContexts];
    float level_[kContexts] = {}, pan_[kContexts] = {};
    uint32_t maxBlock_ = 128;
    std::mutex write_;  // main-thread writers only
};

}  // namespace

std::unique_ptr<Engine> makeEngine() { return std::make_unique<CoreEngine>(); }

}  // namespace va
