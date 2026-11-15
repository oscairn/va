// SPDX-License-Identifier: AGPL-3.0-or-later
// Reporting asleep: process() returns CLAP_PROCESS_SLEEP only when nothing sounds, nothing is queued and the block wrote zeros.
#include "host.h"

namespace {
bool zero(const std::vector<float>& v, size_t from) { return peak(v, from) == 0.f; }
// Blocks until the note ends (its NOTE_END is logged), then one more block that holds that end's tail.
void untilEnded(Inst& a, int32_t id) { for (int i = 0; i < 400 && !a.ends(id); ++i) a.block(128); a.block(128); }
}  // namespace

TEST(sleeps_after_the_release_ends) {
    Inst a;
    a.block(128, {on(0, 1, 0, 60)});
    CHECK_EQ(a.status, CLAP_PROCESS_CONTINUE);  // a voice is active
    a.block(128, {off(0, 1, 0, 60)});
    CHECK_EQ(a.status, CLAP_PROCESS_CONTINUE);  // released, still above the end
    untilEnded(a, 1);
    CHECK_EQ(a.ends(1), 1);
    const size_t from = a.L.size();
    for (int i = 0; i < 4; ++i) {
        a.block(128);
        CHECK_EQ(a.status, CLAP_PROCESS_SLEEP);
    }
    CHECK(zero(a.L, from) && zero(a.R, from));  // the host's 123 fill was overwritten with zeros
    CHECK(!a.badStatus);
}

TEST(a_note_after_sleep_is_the_same_note) {
    Inst a, b;  // a: the idle blocks are called (and sleep); b: they are skipped
    for (int i = 0; i < 10; ++i) { a.block(128); CHECK_EQ(a.status, CLAP_PROCESS_SLEEP); }
    const size_t from = a.L.size();
    for (Inst* x : {&a, &b}) { x->block(128, {on(5, 1, 0, 60)}); x->block(128, {off(0, 1, 0, 60)}); x->run(6000); }
    CHECK(peak(a.L, from) > 0.01f);
    CHECK_EQ(a.L.size() - from, b.L.size());
    CHECK(std::equal(a.L.begin() + long(from), a.L.end(), b.L.begin()) && std::equal(a.R.begin() + long(from), a.R.end(), b.R.begin()));
}

TEST(a_selection_on_a_sleeping_plugin_is_applied_by_the_next_block) {
    Inst a;
    a.block(128);
    CHECK_EQ(a.status, CLAP_PROCESS_SLEEP);
    char msg[128] = {};
    const uint32_t b = a.contexts->prepare(a.p, "b", nullptr, 0, "level=0.5", nullptr, msg, sizeof msg);
    CHECK(a.contexts->select(a.p, b, 3));
    a.block(128);  // the host makes this call after an accepted select: it applies, nothing is left queued, nothing was rendered
    CHECK_EQ(a.status, CLAP_PROCESS_SLEEP);
    oscairn_sound_context_info_t info{};
    CHECK(a.contexts->get(a.p, 1, &info) && info.selected_channels == (1u << 3));
}

TEST(a_state_load_asks_for_a_block) {
    Inst a;
    a.block(128);
    const int wakes = g_wakes;
    CHECK(a.load(a.save()));
    CHECK(g_wakes > wakes);
}
