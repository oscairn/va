// SPDX-License-Identifier: AGPL-3.0-or-later
// The saved states of every published version (tests/states/<version>/, see its README): the current build opens
// each one, reads its parameters back, and plays it as the same sound made from patch text.
#include <filesystem>

#include "host.h"

#ifndef VA_STATES_DIR
#error "VA_STATES_DIR: the directory of tests/states (CMake sets it)"
#endif

namespace {
struct Saved { const char* name; const char* text; std::vector<std::pair<clap_id, double>> values; };
// The sound each state was saved with (tests/states/README.md), as patch text, and the parameters it reads back.
const Saved kSaved[] = {
    {"default", "Osc2OscillatorOutLevel=0", {{0, 1}, {1, 0}, {2, 64}}},
    {"shell", "level=0.6 pan=-0.3 polyphony=8 Osc2OscillatorOutLevel=0", {{0, 0.6}, {1, -0.3}, {2, 8}}},
    {"tone", "knob1=0.25 knob8=1 Filter1FilterCutoff=400 Filter1FilterCutoffEGDepth=300 Osc2OscillatorOutLevel=0",
     {{3, 0.25}, {10, 1}, {18, 400}, {19, 300}}},
};

std::string slurp(const std::filesystem::path& f) {
    FILE* in = std::fopen(f.string().c_str(), "rb");
    std::string s; char buf[4096]; size_t n;
    while (in && (n = std::fread(buf, 1, sizeof buf, in)) > 0) s.append(buf, n);
    if (in) std::fclose(in);
    return s;
}
void play(Inst& a) { a.block(128, {on(0, 1, 0, 48)}); a.run(9600); }
}  // namespace

TEST(saved_states_of_every_published_version_open) {
    int opened = 0;
    for (const auto& dir : std::filesystem::directory_iterator(VA_STATES_DIR)) {
        if (!dir.is_directory()) continue;
        for (const auto& s : kSaved) {
            const auto file = dir.path() / (std::string(s.name) + ".state");
            const std::string data = slurp(file);
            Inst a;
            Stream in; in.data = data;
            if (data.empty() || !a.contexts->state->load(a.p, &in.in)) {
                std::printf("  does not open: %s\n", file.string().c_str());
                ++g_failures;
                continue;
            }
            ++opened;
            for (const auto& [id, v] : s.values) {
                if (std::fabs(a.value(id) - v) > 1e-6) { std::printf("  %s: parameter %u is %g, not %g\n", file.string().c_str(), id, a.value(id), v); ++g_failures; }
            }
            // The current format saves it again as it was saved, and it plays as the sound made from the text.
            Stream again;
            CHECK(a.contexts->state->save(a.p, &again.out) && again.data == data);
            Inst b;
            char msg[160] = {};
            const uint32_t id = b.contexts->prepare(b.p, "Default", nullptr, 0, s.text, nullptr, msg, sizeof msg);
            CHECK(id != CLAP_INVALID_ID);
            b.contexts->select(b.p, id, 0);
            a.clearAudio();
            play(a); play(b);
            CHECK(peak(a.L) > 1e-4f);
            CHECK(a.L == b.L && a.R == b.R);
        }
    }
    CHECK(opened >= 3);
}
