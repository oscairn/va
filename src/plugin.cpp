// SPDX-License-Identifier: AGPL-3.0-or-later
// The plugin shell: CLAP entry, parameters, state, and the Oscairn host contract for an
// instrument (oscairn.sound-contexts, oscairn.voice-budget, oscairn.memory), around the DSP
// core behind engine.h. Sound contexts, selection, note ownership, NOTE_END and the voice
// budget's order come from the Oscairn SDK's helpers (oscairn-clap/sound-contexts-plugin.hpp and
// voice-budget-plugin.hpp); what is written here is what is specific to this plugin.
#include <oscairn-clap/memory.h>
#include <oscairn-clap/sound-contexts-plugin.hpp>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

#include "engine.h"
#include "identity.h"

namespace va {
namespace {

constexpr const char* kStateHeader = "oscairn-va-state 4";      // the CLAP state: the default sound
constexpr const char* kSnapshotHeader = "oscairn-va-contexts/4"; // the sound-contexts snapshot (one token)
constexpr size_t kMaxStateBytes = 1 << 18;  // eight sounds of every raw parameter, as text
constexpr uint32_t kMaxDeviceVoices = 128, kMaxDeviceVoicesPerSound = 64;

// A device cap from the environment: a whole decimal number in 1..ceiling, else none (0).
uint32_t envCap(const char* name, long ceiling) {
    const char* e = std::getenv(name);
    char* end = nullptr;
    const long v = e && *e ? std::strtol(e, &end, 10) : 0;
    return e && *e && !*end && v >= 1 && v <= ceiling ? uint32_t(v) : 0;
}
// The tighter of a host limit and a cap; 0 means none.
uint32_t tighter(uint32_t host, uint32_t cap) { return host && (!cap || host < cap) ? host : cap; }

float clampParam(Param p, double x) {
    const auto& i = kParams[p];
    x = std::min(std::max(x, i.min), i.max);
    return float(i.stepped ? std::round(x) : x);
}
bool validValue(Param p, double x) { return std::isfinite(x) && x >= kParams[p].min && x <= kParams[p].max; }

struct Voice : oscairn::contexts::VoiceBase {};

struct Plugin : oscairn::contexts::Engine<Plugin, Voice, Patch, kContexts, kVoices> {
    using Base = oscairn::contexts::Engine<Plugin, Voice, Patch, kContexts, kVoices>;
    static constexpr const char* stateVersion = kSnapshotHeader;
    static constexpr size_t maxStateBytes = kMaxStateBytes;
    static constexpr bool volumeExpressions = true;  // the core multiplies by the note's volume

    const clap_host_t* host;
    std::unique_ptr<va::Engine> core = makeEngine();
    std::array<std::atomic<float>, kParamCount> values{};  // what get_value shows: the sound on channel 0
    oscairn::voices::Limits budget;                         // audio thread: as set_limits gave them
    const uint32_t deviceCap = envCap(kEnvMaxVoices, kMaxDeviceVoices);
    const uint32_t soundCap = envCap(kEnvMaxVoicesPerSound, kMaxDeviceVoicesPerSound);
    std::array<bool, kVoices> choking{};  // the core was told to fade this voice
    struct Owed { int32_t id; int16_t channel, key; };
    std::array<Owed, kVoices> owed{};     // notes a snapshot load ended; NOTE_END at the next block's start
    uint32_t owedCount = 0;
    uint32_t shownSlot = 0;               // the sound whose values `values` holds
    double sampleRate = 48000;
    uint32_t maxBlock = 0;

    explicit Plugin(const clap_host_t* h) : host(h) {
        plugin = {&kDescriptor, this, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset, process, getExtension, onMainThread};
        for (uint32_t i = 0; i < kParamCount; ++i) values[i].store(float(kParams[i].def));
    }

    // ---- notes -------------------------------------------------------------------------
    // A note-on the plugin cannot start still ends (the host holds a slot for it until then),
    // with the fields it came with.
    void endNoteAs(const clap_event_note_t& n) {
        if (!output) return;
        clap_event_note_t end{};
        end.header = {sizeof(end), eventTime, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0};
        end.note_id = n.note_id; end.port_index = n.port_index; end.channel = n.channel; end.key = n.key;
        output->try_push(output, &end.header);
    }
    void noteOn(const clap_event_note_t& n) {
        if (n.port_index != 0 || n.channel < 0 || n.channel > 15 || n.key < 0 || n.key > 127) return endNoteAs(n);
        Base::noteOn(n);
    }
    bool accepts(const Patch& p, const clap_event_note_t& n) { return keySounds(p, n.key, n.velocity); }
    bool mono(const Context& c) const { return c.patch.raw[vcore::PartMonoPolyMode] == 0; }  // its notes share one voice
    uint32_t index(const Voice& v) const { return uint32_t(&v - voices.data()); }
    void start(Voice& v, Context& c) {
        const uint32_t i = index(v);
        makeRoom(c, i);
        core->noteOn(i, uint32_t(&c - contexts.data()), v.channel, v.key, v.velocity);
    }
    void release(Voice& v) { core->noteOff(index(v)); }
    // Every way a note's voice ends (released out, choked, stolen, reset): free the core's slot.
    void voiceFinished(Voice& v) { core->kill(index(v)); choking[index(v)] = false; }
    double loudness(const Voice& v) const { return core->voice(index(v)).level; }
    void applyExpression(const clap_event_note_expression_t& e) {
        Base::applyExpression(e);
        if (e.port_index > 0) return;
        if (e.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE) {  // aftertouch, box source 2, on the note's channel (the latest wins)
            if (e.channel >= 0 && e.channel < 16 && std::isfinite(e.value)) core->setController(e.channel, 2, e.value);
            return;
        }
        if (e.expression_id != CLAP_NOTE_EXPRESSION_VOLUME) return;
        for (uint32_t i = 0; i < kVoices; ++i) if (matches(voices[i], e.note_id, e.key, e.channel)) core->setVolume(i, voices[i].volume);
    }
    // MIDI controllers the controller boxes take (docs/parameters.md): the reference's default numbers, of those a host
    // passes to a plugin (the ribbon, the switches and the knobs are parameters).
    void applyMidi(const clap_event_midi_t& e) {
        Base::applyMidi(e);
        if (e.port_index) return;
        const int ch = e.data[0] & 15, kind = e.data[0] & 0xf0;
        if (kind == 0xe0) core->setPitchBend(int16_t(ch), channelBend[ch]);
        else if (kind == 0xd0) core->setController(int16_t(ch), 2, (e.data[1] & 127) / 127.0);
        else if (kind == 0xb0) {
            int source = -2;
            switch (e.data[1]) {
            case 1: source = 1; break;    // mod wheel
            case 2: source = 7; break;    // breath
            case 11: source = 3; break;   // foot controller 1
            case 121: source = -1; break; // reset all controllers
            default: break;
            }
            if (source != -2) core->setController(int16_t(ch), source, (e.data[2] & 127) / 127.0);
        }
    }
    // A sound-context snapshot load (the helper's loadSnapshot calls this): sound stops at once,
    // and a note that was sounding still gets its NOTE_END at the start of the next block
    // (there is no output queue now).
    void resetVoices() {
        for (const auto& v : voices)
            if (v.active && owedCount < owed.size()) owed[owedCount++] = {v.noteId, v.channel, v.key};
        core->reset();
        choking.fill(false);
        Base::resetVoices();
    }
    // CLAP reset: immediate and complete. Every voice ends silently, every note id is forgotten
    // (also the ones a snapshot load still owed an end), and the next block says nothing about them.
    void silence() {
        owedCount = 0;
        core->reset();
        choking.fill(false);
        Base::resetVoices();
    }

    // ---- oscairn.voice-budget -----------------------------------------------------------
    // Past the host's limits and the device caps, voices are given up in voice-budget.h's order
    // (the SDK's victim()) and fade out over the choke time. Audio thread only.
    static void setLimits(const clap_plugin_t* p, uint32_t limit, const oscairn_voice_budget_context_t* list, uint32_t count) {
        self(p).budget.set(limit, list, count);
    }
    static uint32_t activeVoices(const clap_plugin_t* p, double* cost) {
        auto& s = self(p);
        if (cost) *cost = s.voiceCost();
        return s.core->voiceCount();
    }
    // One voice's estimated cost (microseconds a block): the costliest sound in use, from its plan,
    // before any note has sounded.
    double voiceCost() const {
        double worst = 0;
        for (uint32_t i = 0; i < kContexts; ++i)
            if (contexts[i].prepared) worst = std::max(worst, core->cost(i).voiceMicros);
        return worst;
    }
    uint32_t instanceLimit() const { return tighter(budget.limit, deviceCap); }
    oscairn_voice_budget_context_t soundLimits(const Context& c) const {
        auto own = budget.of(c.id, c.selected.load(std::memory_order_relaxed) != 0);
        own.limit = tighter(tighter(own.limit, soundCap), uint32_t(std::lround(clampParam(kPolyphony, c.patch.p[kPolyphony].get()))));
        return own;
    }
    struct Described {
        std::array<oscairn::voices::Candidate, kVoices> c{};
        std::array<bool, kVoices> present{};
        uint32_t keeping(uint32_t id = oscairn::voices::none) const {
            uint32_t n = 0;
            for (uint32_t i = 0; i < kVoices; ++i) n += present[i] && !c[i].ending && (id == oscairn::voices::none || c[i].context == id);
            return n;
        }
    };
    // Every voice but `except`, as the order sees it. Held: its key is down (a pedal does not count).
    void describe(Described& d, uint32_t except) const {
        for (uint32_t i = 0; i < kVoices; ++i) {
            const auto& v = voices[i];
            d.present[i] = v.active && i != except;
            if (!d.present[i]) continue;
            const auto& c = contexts[v.context];
            d.c[i] = {c.id, c.selected.load(std::memory_order_relaxed) != 0, v.held, v.chokeLength != 0, false, core->voice(i).level, v.started};
        }
    }
    int victim(const Described& d, uint32_t incoming, uint32_t only = oscairn::voices::none) const {
        return oscairn::voices::victim(kVoices, [&](size_t i) { return d.present[i] ? &d.c[i] : nullptr; }, budget, incoming, only);
    }
    bool giveUp(Described& d, int i) {
        if (i < 0) return false;
        fadeOut(i);
        core->choke(uint32_t(i), chokeFrames);
        choking[size_t(i)] = true;
        d.c[size_t(i)].ending = true;
        return true;
    }
    // Before a note starts in `c`: room for its voice within the limits.
    void makeRoom(const Context& c, uint32_t self_) {
        Described d; describe(d, self_);
        const auto own = soundLimits(c);
        while (own.limit && d.keeping(c.id) + 1 > own.limit && giveUp(d, victim(d, c.id, c.id))) {}
        const uint32_t cap = instanceLimit();
        while (cap && d.keeping() + 1 > cap && giveUp(d, victim(d, c.id))) {}
    }
    // At a block's start: owed NOTE_ENDs, then voices past a limit set_limits lowered fade out.
    void keepWithinLimits() {
        for (uint32_t i = 0; i < owedCount; ++i) {
            clap_event_note_t n{}; n.note_id = owed[i].id; n.channel = owed[i].channel; n.key = owed[i].key;
            endNoteAs(n);
        }
        owedCount = 0;
        if (selected[0] != shownSlot) selectedGlobally(contexts[selected[0]], output);  // channel 0 was given another sound
        Described d; describe(d, kVoices);
        for (uint32_t slot = 0; slot < kContexts; ++slot) {
            const auto& c = contexts[slot];
            if (!c.users.load(std::memory_order_acquire)) continue;
            const auto own = soundLimits(c);
            while (own.limit && d.keeping(c.id) > own.limit && giveUp(d, victim(d, oscairn::voices::none, c.id))) {}
        }
        const uint32_t cap = instanceLimit();
        while (cap && d.keeping() > cap && giveUp(d, victim(d, oscairn::voices::none))) {}
    }

    // ---- rendering ----------------------------------------------------------------------
    void render(const clap_process_t* pr, uint32_t from, uint32_t to) {
        for (uint32_t i = 0; i < kVoices; ++i)  // a choke (NOTE_CHOKE or the helper's own) fades in the core too
            if (voices[i].active && voices[i].chokeLength && !choking[i]) {
                core->choke(i, std::max<uint32_t>(1, voices[i].chokeLeft));
                choking[i] = true;
            }
        float* const* out = pr->audio_outputs[0].data32;
        if (!out || !out[0] || !out[1]) return;
        std::memset(out[0] + from, 0, (to - from) * sizeof(float));
        std::memset(out[1] + from, 0, (to - from) * sizeof(float));
        core->render(out[0] + from, out[1] + from, to - from);
        for (uint32_t i = 0; i < kVoices; ++i)  // a voice the core finished: its note ends here
            if (voices[i].active && !core->voice(i).sounding) {
                const auto at = eventTime; eventTime = to ? to - 1 : 0;
                finishVoice(voices[i]);
                eventTime = at;
            }
    }

    // ---- parameters ---------------------------------------------------------------------
    template <class F> void forTargets(int16_t channel, F&& f) {
        uint32_t seen = 0;
        for (int ch = 0; ch < 16; ++ch) {
            if (channel >= 0 && channel < 16 && channel != ch) continue;
            const auto slot = selected[ch];
            if (!(seen >> slot & 1)) { seen |= 1u << slot; f(slot); }
        }
    }
    void applyParam(const clap_event_header_t* h) {
        if (h->type == CLAP_EVENT_PARAM_MOD) {  // a destination's modulation, raw steps; not saved, not shown as its value
            const auto* e = reinterpret_cast<const clap_event_param_mod_t*>(h);
            if (e->param_id < kDest0 || e->param_id > kDestLast || !std::isfinite(e->amount) || e->note_id >= 0 || e->key >= 0) return;  // global only
            forTargets(e->channel, [&](uint32_t slot) { core->modParam(slot, Param(e->param_id), e->amount); });
            return;
        }
        if (h->type != CLAP_EVENT_PARAM_VALUE) return;
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        if (e->param_id >= kParamCount || !std::isfinite(e->value)) return;
        const auto id = Param(e->param_id); const float x = clampParam(id, e->value);
        forTargets(e->channel, [&](uint32_t slot) {
            contexts[slot].patch.p[id].set(x);
            core->setParam(slot, id, x);
            if (slot == selected[0]) values[id].store(x, std::memory_order_relaxed);
        });
    }
    // After a selection for every channel: the parameters show the selected sound's values.
    void selectedGlobally(Context& next, const clap_output_events_t* out) {
        shownSlot = uint32_t(&next - contexts.data());
        for (uint32_t i = 0; i < kParamCount; ++i) {
            const float v = next.patch.p[i].get();
            if (v == values[i].load(std::memory_order_relaxed)) continue;
            values[i].store(v, std::memory_order_relaxed);
            if (!out) continue;
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = i; ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1; ev.value = v;
            out->try_push(out, &ev.header);
        }
    }

    // ---- patches and state --------------------------------------------------------------
    // The raw patch's knob values and destinations follow the shell's parameters (main thread: the patch is not shared yet).
    static void syncKnobs(Patch& p) {
        for (int k = 0; k < 8; ++k) p.raw.set(vcore::Param(vcore::PartAssignableKnob1Value + k), int(std::lround(p.p[kKnob1 + k].get() * 1023)));
        for (int d = 0; d < vcore::kDests; ++d) p.raw.set(kDestRaw[d], int(std::lround(p.p[kDest0 + d].get())));
    }
    // The destination parameters from the raw patch (they are its values).
    static void destsFromRaw(Patch& p) { for (int d = 0; d < vcore::kDests; ++d) p.p[kDest0 + d].set(float(p.raw[kDestRaw[d]])); }
    // The shell's values, then the sound's raw parameters as one quoted vcore-patch text.
    bool writePatch(std::ostream& out, const Patch& p) {
        for (uint32_t i = 0; i < kStateCount; ++i) out << ' ' << p.p[i].get();
        Patch q = p; syncKnobs(q);  // the knobs as they are now (set live, the raw text still holds where they started)
        std::string text(vcore::writePatch(q.raw, nullptr, 0) + 1, '\0');
        text.resize(vcore::writePatch(q.raw, &text[0], text.size()));
        out << ' ' << std::quoted(text);
        return bool(out);
    }
    bool readPatch(std::istream& in, Patch& p) {
        double x[kStateCount];
        for (uint32_t i = 0; i < kStateCount; ++i) if (!(in >> x[i]) || !validValue(Param(i), x[i])) return false;
        std::string text;
        vcore::Patch raw;
        if (!(in >> std::quoted(text)) || !vcore::readPatch(text.c_str(), raw, nullptr, 0)) return false;
        for (uint32_t i = 0; i < kStateCount; ++i) p.p[i].set(clampParam(Param(i), x[i]));
        p.raw = raw;
        destsFromRaw(p);
        syncKnobs(p);
        return true;
    }
    void loadAll() { for (uint32_t i = 0; i < kContexts; ++i) if (contexts[i].prepared) core->loadPatch(i, contexts[i].patch); }
    void restored() {
        loadAll();
        shownSlot = selected[0];
        for (uint32_t i = 0; i < kParamCount; ++i) values[i].store(contexts[selected[0]].patch.p[i].get(), std::memory_order_relaxed);
    }
    // Main thread, concurrently with process(): the sound is the engine's, not yet selected.
    bool buildPreset(Context& c, std::string&) { core->loadPatch(uint32_t(&c - contexts.data()), c.patch); return true; }

    static bool saveState(const clap_plugin_t* p, const clap_ostream_t* stream) {
        auto& s = self(p);
        std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9) << kStateHeader << '\n';
        s.writePatch(out, s.contexts[0].patch); out << '\n';
        const auto data = out.str();
        for (size_t at = 0; at < data.size();) {
            const auto n = stream->write(stream, data.data() + at, data.size() - at);
            if (n <= 0 || size_t(n) > data.size() - at) return false;
            at += size_t(n);
        }
        return true;
    }
    static bool loadState(const clap_plugin_t* p, const clap_istream_t* stream) {
        auto& s = self(p);
        std::string data; char buf[256];
        for (;;) {
            const auto n = stream->read(stream, buf, sizeof buf);
            if (n < 0 || n > int64_t(sizeof buf) || data.size() + size_t(n) > kMaxStateBytes) return false;
            if (!n) break;
            data.append(buf, size_t(n));
        }
        std::istringstream in(data); in.imbue(std::locale::classic());
        std::string line;
        if (!std::getline(in, line) || line != kStateHeader) return false;
        Patch patch;
        if (!s.readPatch(in, patch)) return false;
        in >> std::ws;
        if (!in.eof()) return false;
        for (uint32_t i = 0; i < kParamCount; ++i) {
            s.contexts[0].patch.p[i].set(patch.p[i].get());
            if (s.contexts[0].selected.load() & 1) s.values[i].store(patch.p[i].get(), std::memory_order_relaxed);
        }
        s.contexts[0].patch.raw = patch.raw;
        s.core->loadPatch(0, patch);
        return true;
    }

    // ---- sound contexts: prepare ---------------------------------------------------------
    // Patch text: whitespace-separated name=value pairs: the shell's (level, pan, polyphony, knob1..knob8) and the
    // sound's raw parameters by their core ids (core/params.inc; an optional leading "vcore-patch/1");
    // absent: defaults.
    static bool parsePatchText(const char* text, Patch& patch, std::string& error) {
        std::istringstream in(text); in.imbue(std::locale::classic());
        std::string tok, raw = "vcore-patch/1";
        bool given[kParamCount] = {};
        while (in >> tok) {
            const auto eq = tok.find('=');
            const auto key = tok.substr(0, eq);
            uint32_t id = kParamCount;
            for (uint32_t i = 0; i < kStateCount; ++i) if (key == kParams[i].key) id = i;
            if (tok == "vcore-patch/1") continue;
            if (id == kParamCount && eq != std::string::npos && vcore::findParam(key.c_str(), key.size()) >= 0) { raw += ' ' + tok; continue; }
            if (id == kParamCount || eq == std::string::npos) { error = "line 1: unknown or malformed '" + tok + "'"; return false; }
            char* end = nullptr; const double x = std::strtod(tok.c_str() + eq + 1, &end);
            if (*end || end == tok.c_str() + eq + 1 || !validValue(Param(id), x)) { error = "line 1: bad value in '" + tok + "'"; return false; }
            patch.p[id].set(clampParam(Param(id), x));
            given[id] = true;
        }
        char why[160] = {};
        if (!vcore::readPatch(raw.c_str(), patch.raw, why, sizeof why)) { error = std::string("line 1: ") + why; return false; }
        for (int k = 0; k < 8; ++k)  // a knob not named starts at the patch's own value
            if (!given[kKnob1 + k]) patch.p[kKnob1 + k].set(float(patch.raw[vcore::Param(vcore::PartAssignableKnob1Value + k)]) / 1023.f);
        destsFromRaw(patch);
        syncKnobs(patch);
        return true;
    }
    static uint32_t prepare(const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t* values, uint32_t count,
                            const char* text, const oscairn_asset_ref_t*, char* message, uint32_t capacity) {
        auto fail = [&](const std::string& why) { if (message && capacity) std::snprintf(message, capacity, "%s", why.c_str()); return CLAP_INVALID_ID; };
        if (!name || std::strlen(name) > 63) return fail("invalid sound name");
        auto& s = self(p);
        Patch patch; std::string error;
        if (text && !parsePatchText(text, patch, error)) return fail(error);
        for (uint32_t i = 0; i < count; ++i) {
            if (!values || values[i].id >= kParamCount || !validValue(Param(values[i].id), values[i].value)) return fail("invalid parameter");
            patch.p[values[i].id].set(clampParam(Param(values[i].id), values[i].value));
        }
        syncKnobs(patch);
        auto* c = s.freeSlot();
        if (!c) return fail("no free sound context");
        c->patch = patch;
        s.core->loadPatch(uint32_t(c - s.contexts.data()), patch);
        return s.publish(*c, name);
    }

    // ---- CLAP plugin ----------------------------------------------------------------------
    static bool init(const clap_plugin_t*) { return true; }
    static void destroy(const clap_plugin_t* p) { delete &self(p); }
    static bool activate(const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
        auto& s = self(p);
        s.sampleRate = rate; s.maxBlock = maxFrames;
        s.setSampleRate(rate);
        s.core->prepare(rate, maxFrames);
        s.loadAll();
        return true;
    }
    static void deactivate(const clap_plugin_t*) {}
    static bool startProcessing(const clap_plugin_t*) { return true; }
    static void stopProcessing(const clap_plugin_t*) {}
    static void reset(const clap_plugin_t* p) { self(p).silence(); }
    static void onMainThread(const clap_plugin_t*) {}  // nothing is deferred to it; a host may still call it
    static clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) { return Base::process(p, pr); }

    static uint32_t paramCount(const clap_plugin_t*) { return kParamCount; }
    static bool paramInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
        if (i >= kParamCount) return false;
        *info = {};
        info->id = i;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | (kParams[i].stepped ? CLAP_PARAM_IS_STEPPED : 0) | (i >= kDest0 ? CLAP_PARAM_IS_MODULATABLE : 0);
        std::snprintf(info->name, sizeof info->name, "%s", kParams[i].name);
        info->min_value = kParams[i].min; info->max_value = kParams[i].max; info->default_value = kParams[i].def;
        return true;
    }
    static bool paramValue(const clap_plugin_t* p, clap_id id, double* v) {
        if (id >= kParamCount) return false;
        *v = self(p).values[id].load(std::memory_order_relaxed);
        return true;
    }
    static bool paramText(const clap_plugin_t*, clap_id id, double v, char* out, uint32_t n) {
        if (id >= kParamCount) return false;
        std::snprintf(out, n, kParams[id].stepped || id >= kDest0 ? "%.0f" : "%.3f", v);
        return true;
    }
    static bool paramFromText(const clap_plugin_t*, clap_id id, const char* text, double* v) {
        if (id >= kParamCount) return false;
        char* end = nullptr; *v = std::strtod(text, &end);
        return end != text;
    }
    static void paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
        auto& s = self(p);
        for (uint32_t i = 0, n = in->size(in); i < n; ++i) s.applyParam(in->get(in, i));
    }

    // oscairn.ui: every parameter has a hint. Roles for the role banks; the filter and amplitude envelopes' times as two
    // envelope views (attack, decay, release: their sustain levels are not parameters); the knobs, ribbon and switches, and
    // filter 2's cutoff, EG depth and resonance as one group each.
    static bool paramHint(const clap_plugin_t*, clap_id id, oscairn_param_hint_t* h) {
        if (id >= kParamCount || !h) return false;
        *h = {};
        h->range_high = CLAP_INVALID_ID;
        switch (id) {
        case kLevel: h->role = OSCAIRN_ROLE_LEVEL; h->priority = 1; break;
        case kPan: h->role = OSCAIRN_ROLE_PAN; break;
        case kSwitch1: case kSwitch2: h->widget = OSCAIRN_WIDGET_SWITCH; h->group = 2; break;
        case kRibbon: h->group = 2; break;
        case kDest0 + vcore::DOscLevel1: h->role = OSCAIRN_ROLE_LEVEL_1; break;
        case kDest0 + vcore::DOscLevel2: h->role = OSCAIRN_ROLE_LEVEL_1 + 1; break;
        case kDest0 + vcore::DCutoff: h->role = OSCAIRN_ROLE_CUTOFF; h->priority = 2; break;
        case kDest0 + vcore::DReso: h->role = OSCAIRN_ROLE_RESONANCE; h->priority = 3; break;
        case kDest0 + vcore::DFegDepth: h->role = OSCAIRN_ROLE_FILTER_ENVELOPE; h->priority = 4; break;
        case kDest0 + vcore::DFegAttack: h->role = OSCAIRN_ROLE_FILTER_ATTACK; h->view = 2; h->field = OSCAIRN_FIELD_ATTACK; h->group = 3; break;
        case kDest0 + vcore::DFegDecay: h->role = OSCAIRN_ROLE_FILTER_DECAY; h->view = 2; h->field = OSCAIRN_FIELD_DECAY; h->group = 3; break;
        case kDest0 + vcore::DFegRelease: h->role = OSCAIRN_ROLE_FILTER_RELEASE; h->view = 2; h->field = OSCAIRN_FIELD_RELEASE; h->group = 3; break;
        case kDest0 + vcore::DAmpAttack: h->role = OSCAIRN_ROLE_ATTACK; h->view = 1; h->field = OSCAIRN_FIELD_ATTACK; h->group = 4; break;
        case kDest0 + vcore::DAmpDecay: h->role = OSCAIRN_ROLE_DECAY; h->view = 1; h->field = OSCAIRN_FIELD_DECAY; h->group = 4; break;
        case kDest0 + vcore::DAmpRelease: h->role = OSCAIRN_ROLE_RELEASE; h->view = 1; h->field = OSCAIRN_FIELD_RELEASE; h->group = 4; break;
        case kDest0 + vcore::DCutoff2: case kDest0 + vcore::DFegDepth2: case kDest0 + vcore::DReso2: h->group = 5; break;  // filter 2 (no role of its own)
        default: if (id >= kKnob1 && id <= kKnob8) h->group = 1; break;
        }
        return true;
    }

    static bool memoryEstimate(const clap_plugin_t* p, double rate, uint32_t frames, oscairn_memory_t* out) {
        const auto m = self(p).core->memory(rate, frames);
        *out = {m.shared + sizeof(Plugin), m.perContext, 0};
        return true;
    }
    static bool memoryCurrent(const clap_plugin_t* p, oscairn_memory_t* out) { return memoryEstimate(p, self(p).sampleRate, self(p).maxBlock, out); }

    static const void* getExtension(const clap_plugin_t*, const char* id);
};

const clap_plugin_audio_ports_t kAudioPorts = {
    [](const clap_plugin_t*, bool isInput) -> uint32_t { return isInput ? 0 : 1; },
    [](const clap_plugin_t*, uint32_t i, bool isInput, clap_audio_port_info_t* info) {
        if (isInput || i) return false;
        *info = {};
        info->id = 0; std::snprintf(info->name, sizeof info->name, "Main");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->channel_count = 2; info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }};
const clap_plugin_note_ports_t kNotePorts = {
    [](const clap_plugin_t*, bool isInput) -> uint32_t { return isInput ? 1 : 0; },
    [](const clap_plugin_t*, uint32_t i, bool isInput, clap_note_port_info_t* info) {
        if (!isInput || i) return false;
        *info = {};
        info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof info->name, "Notes");
        return true;
    }};
const clap_plugin_params_t kParamsExt = {Plugin::paramCount, Plugin::paramInfo, Plugin::paramValue, Plugin::paramText, Plugin::paramFromText, Plugin::paramFlush};
const clap_plugin_state_t kState = {Plugin::saveState, Plugin::loadState};
const clap_plugin_state_t kSnapshot = {Plugin::saveSnapshot, Plugin::loadSnapshot};
const oscairn_plugin_sound_contexts_t kSoundContexts = {Plugin::capacity, Plugin::get, Plugin::prepare, Plugin::select, Plugin::discard, &kSnapshot, nullptr};
const oscairn_plugin_voice_budget_t kVoiceBudget = {Plugin::honorsVolume, Plugin::setLimits, Plugin::activeVoices, Plugin::voiceNotes};
const oscairn_plugin_memory_t kMemory = {Plugin::memoryEstimate, Plugin::memoryCurrent};
const oscairn_plugin_ui_t kUi = {Plugin::paramHint, nullptr, nullptr};

const void* Plugin::getExtension(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS)) return &kSoundContexts;
    if (!std::strcmp(id, OSCAIRN_EXT_VOICE_BUDGET)) return &kVoiceBudget;
    if (!std::strcmp(id, OSCAIRN_EXT_MEMORY)) return &kMemory;
    if (!std::strcmp(id, OSCAIRN_EXT_UI) || !std::strcmp(id, OSCAIRN_EXT_UI_DRAFT_4)) return &kUi;
    return nullptr;
}

const clap_plugin_factory_t kFactory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t i) -> const clap_plugin_descriptor_t* { return i ? nullptr : &kDescriptor; },
    [](const clap_plugin_factory_t*, const clap_host_t* h, const char* id) -> const clap_plugin_t* {
        if (!id || std::strcmp(id, kDescriptor.id)) return nullptr;
        return &(new Plugin(h))->plugin;
    }};

}  // namespace
}  // namespace va

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION, [](const char*) { return true; }, []() {},
    [](const char* id) -> const void* { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &va::kFactory : nullptr; }};
