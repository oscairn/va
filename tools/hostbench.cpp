// SPDX-License-Identifier: AGPL-3.0-or-later
// The hosted cost: drives the built plugin through its CLAP ABI as the host does (dlopen, a sound-context snapshot of probe A
// loaded, 128-frame blocks at 48 kHz, after every block oscairn.voice-budget's active_voices and its notes), one callgrind dump
// per measured block (tools/cost/cg.sh).
//   hostbench PLUGIN.clap CASE WARM MEAS      CASE: rest | held N | pattern (as tools/vcbench.cpp's); with HOSTBENCH_WHOLE set,
//                                             one dump for the whole measured window
#include <clap/clap.h>
#include <dlfcn.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/voice-budget.h>
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_ZERO_STATS
#define CALLGRIND_DUMP_STATS
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* kProbeA =
#include "probeA.inc"
;

struct Reader {
    std::string d; size_t at = 0;
    clap_istream_t in{this, [](const clap_istream_t* s, void* b, uint64_t n) -> int64_t {
        auto* r = static_cast<Reader*>(s->ctx); const size_t k = std::min<size_t>(size_t(n), r->d.size() - r->at);
        std::memcpy(b, r->d.data() + r->at, k); r->at += k; return int64_t(k); }};
};
struct Events {
    std::vector<clap_event_note_t> ev;
    clap_input_events_t api{this, [](const clap_input_events_t* l) { return uint32_t(static_cast<const Events*>(l->ctx)->ev.size()); },
                            [](const clap_input_events_t* l, uint32_t i) { return &static_cast<const Events*>(l->ctx)->ev[i].header; }};
};
static const clap_output_events_t kOut{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
static const clap_host_t kHost = {CLAP_VERSION, nullptr, "hostbench", "oscairn", "", "1", [](const clap_host_t*, const char*) -> const void* { return nullptr; },
                                  [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}};

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: hostbench PLUGIN.clap CASE [N] WARM MEAS\n"); return 2; }
    const std::string c = argv[2];
    const int n = c == "held" ? std::atoi(argv[3]) : 0, a0 = c == "held" ? 4 : 3;
    if (argc < a0 + 2) return 2;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    entry->init(argv[1]);
    auto* f = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_t* p = f->create_plugin(f, &kHost, "org.oscairn.va");
    if (!p || !p->init(p)) return 1;
    auto* sc = static_cast<const oscairn_plugin_sound_contexts_t*>(p->get_extension(p, OSCAIRN_EXT_SOUND_CONTEXTS));
    auto* vb = static_cast<const oscairn_plugin_voice_budget_t*>(p->get_extension(p, OSCAIRN_EXT_VOICE_BUDGET));
    // the snapshot: probe A as the default sound, its knobs as the shell's values (level, pan, polyphony, knobs 1-8, ribbon, switches)
    const std::string text = kProbeA;
    std::string snap = "oscairn-va-contexts/4\n1 \"Default\" 1 0 64";
    for (int k = 1; k <= 8; ++k) {
        const std::string key = "PartAssignableKnob" + std::to_string(k) + "Value=";
        const size_t at = text.find(key);
        snap += ' ' + std::to_string((at == std::string::npos ? 512 : std::atoi(text.c_str() + at + key.size())) / 1023.0);
    }
    snap += " 0 0 0 \"" + text + "\"\n";
    for (int i = 1; i < 8; ++i) snap += "0\n";
    for (int ch = 0; ch < 16; ++ch) snap += "0 ";
    snap += '\n';
    Reader r; r.d = snap;
    if (!sc->state->load(p, &r.in)) { std::fprintf(stderr, "snapshot not loaded\n"); return 1; }
    p->activate(p, 48000, 1, 128); p->start_processing(p);

    const int B = 128;
    const long first = long(std::atof(argv[a0]) * 48000 / B), total = first + long(std::atof(argv[a0 + 1]) * 48000 / B);
    struct Ev { long frame; bool on; int key; };
    std::vector<Ev> evs;
    if (c == "pattern") {
        const double step = 60.0 / 84 / 4 * 48000, gate = 0.9 * step;
        const int keys[] = {84, 88, 91, 88};
        for (long k = 0; k * step < double((total + 4) * B); ++k) {
            evs.push_back({long(std::floor(k * step)), true, keys[k % 4]});
            evs.push_back({long(std::floor(k * step + gate)), false, keys[k % 4]});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    } else for (int k = 0; k < n; ++k) evs.push_back({0, true, 84 + k % 13});
    float l[B], rr[B]; float* ch[2] = {l, rr};
    clap_audio_buffer_t out{}; out.data32 = ch; out.channel_count = 2;
    std::vector<oscairn_voice_budget_note_t> notes(256);
    Events in; in.ev.reserve(64);
    const bool whole = std::getenv("HOSTBENCH_WHOLE") != nullptr;
    size_t ei = 0; int32_t id = 0; double sum = 0; uint32_t voices = 0;
    for (long b = 0; b < total; ++b) {
        in.ev.clear();
        while (ei < evs.size() && evs[ei].frame < (b + 1) * B) {
            const Ev& e = evs[ei++];
            clap_event_note_t x{};
            x.header = {sizeof x, uint32_t(e.frame - b * B), CLAP_CORE_EVENT_SPACE_ID, uint16_t(e.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            x.note_id = e.on ? ++id : -1; x.channel = 0; x.key = int16_t(e.key); x.velocity = e.on ? 0.8 : 0;
            in.ev.push_back(x);
        }
        clap_process_t pr{};
        pr.steady_time = b * B; pr.frames_count = B; pr.in_events = &in.api; pr.out_events = &kOut;
        pr.audio_outputs = &out; pr.audio_outputs_count = 1;
        const bool m = b >= first;
        if (m && (!whole || b == first)) CALLGRIND_ZERO_STATS;
        p->process(p, &pr);
        double cost = 0;
        voices = vb->active_voices(p, &cost);
        vb->notes(p, notes.data(), uint32_t(notes.size()));
        if (m && !whole) CALLGRIND_DUMP_STATS;
        for (int i = 0; i < B; ++i) sum += std::fabs(l[i]) + std::fabs(rr[i]);
    }
    if (whole) CALLGRIND_DUMP_STATS;
    std::printf("voices at the end %u, mean |sample| %.6f\n", voices, sum / double(total * B * 2));
    p->stop_processing(p); p->deactivate(p); p->destroy(p);
    return 0;
}
