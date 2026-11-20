// SPDX-License-Identifier: AGPL-3.0-or-later
// oscairn.voice-budget: the host's limits, the order voices are given up in, per-sound limits,
// the device caps from the environment, and what notes() and active_voices report.
#include "host.h"

namespace {
struct Env {
    explicit Env(const char* instance, const char* sound = nullptr) {
        set("OSCAIRN_VA_MAX_VOICES", instance); set("OSCAIRN_VA_MAX_VOICES_PER_SOUND", sound);
    }
    ~Env() { set("OSCAIRN_VA_MAX_VOICES", nullptr); set("OSCAIRN_VA_MAX_VOICES_PER_SOUND", nullptr); }
    static void set(const char* n, const char* v) { if (v) setenv(n, v, 1); else unsetenv(n); }
};
// Notes ended so far, as a set of ids.
std::vector<int> ended(const Inst& a) {
    std::vector<int> v;
    for (auto& r : a.log) if (r.type == CLAP_EVENT_NOTE_END) v.push_back(r.id);
    std::sort(v.begin(), v.end());
    return v;
}
}  // namespace

TEST(host_limit_gives_up_the_oldest_held) {
    Inst a;
    a.limits(3);
    for (int i = 0; i < 6; ++i) {
        a.block(128, {on(0, 1 + i, 0, 60 + i)});
        a.run(640);  // the 3 ms fade finishes
        CHECK(a.voices() <= 3u);
    }
    CHECK(a.voices() == 3u);
    CHECK((ended(a) == std::vector<int>{1, 2, 3}));
    // raising the limit keeps what sounds; lowering it takes voices at the next block
    a.limits(0);
    a.block(128, {on(0, 7, 0, 70)});
    a.run(640);
    CHECK_EQ(a.voices(), 4u);
    a.limits(2);
    a.run(640);
    CHECK_EQ(a.voices(), 2u);
    CHECK((ended(a) == std::vector<int>{1, 2, 3, 4, 5}));
}

TEST(released_voices_go_before_held_and_the_quietest_first) {
    Inst a;
    a.limits(3);
    // three notes, loudness by volume expression (velocity does not change this sound); all released: the
    // quietest releasing one goes first
    a.block(128, {on(0, 1, 0, 60), on(0, 2, 0, 62), on(0, 3, 0, 64), volume(0, 1, 0, 60, 0.9), volume(0, 2, 0, 62, 0.2), volume(0, 3, 0, 64, 0.5)});
    a.run(1280);
    a.block(128, {off(0, 1, 0, 60), off(0, 2, 0, 62), off(0, 3, 0, 64)});
    a.run(256);
    a.block(128, {on(0, 4, 0, 66, 0.8)});
    a.run(640);
    CHECK((ended(a) == std::vector<int>{2}));
    a.block(128, {on(0, 5, 0, 67, 0.8)});
    a.run(640);
    CHECK((ended(a) == std::vector<int>{2, 3}));  // 3 (0.5) before 1 (0.9), while 4 is held
    a.block(128, {on(0, 6, 0, 69, 0.8)});
    a.run(640);
    CHECK((ended(a) == std::vector<int>{1, 2, 3}));
    a.block(128, {on(0, 7, 0, 71, 0.8)});  // only held voices remain: the oldest goes
    a.run(640);
    CHECK((ended(a) == std::vector<int>{1, 2, 3, 4}));
    CHECK_EQ(a.voices(), 3u);
}

TEST(per_sound_limit_and_reserve) {
    Inst a;
    char msg[64];
    const uint32_t b = a.contexts->prepare(a.p, "b", nullptr, 0, nullptr, nullptr, msg, sizeof msg);
    a.contexts->select(a.p, b, 1);
    a.limits(0, {{b, 2, 0, 5, 0}});  // sound b: at most 2 voices
    a.block(128);
    for (int i = 0; i < 4; ++i) { a.block(128, {on(0, 10 + i, 1, 60 + i)}); a.run(640); }  // each reaches full level before the next: the oldest then goes
    CHECK((ended(a) == std::vector<int>{10, 11}));
    // the default sound is not limited by b's entry
    for (int i = 0; i < 4; ++i) { a.block(128, {on(0, 20 + i, 0, 60 + i)}); a.run(640); }
    CHECK_EQ(a.voices(), 6u);
    // a reserve on the default sound (ID 0): its held voices are not given up for b's note
    Inst c;
    const uint32_t b2 = c.contexts->prepare(c.p, "b", nullptr, 0, nullptr, nullptr, msg, sizeof msg);
    c.contexts->select(c.p, b2, 1);
    c.limits(2, {{0, 0, 2, 5, 0}, {b2, 0, 0, 5, 0}});
    c.block(128);
    c.block(128, {on(0, 1, 0, 60), on(0, 2, 0, 62)});
    c.run(1280);
    c.block(128, {on(0, 3, 1, 64)});  // b's own note has to take one of the default's reserved voices
    c.run(640);
    // the default sound's two held voices are its reserve: the only voice that may go is b's own new one
    CHECK_EQ(c.voices(), 2u);
    CHECK((ended(c) == std::vector<int>{3}));
}

TEST(polyphony_parameter_limits_the_sound) {
    Inst a;
    a.block(128, {param(0, 2, 2)});
    for (int i = 0; i < 5; ++i) { a.block(128, {on(0, 1 + i, 0, 60 + i)}); a.run(640); }
    CHECK_EQ(a.voices(), 2u);
    CHECK((ended(a) == std::vector<int>{1, 2, 3}));
    CHECK(a.value(2) == 2);
}

TEST(device_caps_parse_like_analog) {
    // valid: a whole decimal 1..128 (instance), 1..64 (each sound); anything else is no cap
    struct Case { const char* v; uint32_t expect; } cases[] = {{"3", 3}, {"1", 1}, {"64", 64}, {"0", 0}, {"-2", 0}, {"abc", 0}, {"3x", 0}, {"", 0}, {"129", 0}, {" 3", 3 /*strtol skips leading space*/}};
    for (auto& c : cases) {
        Env env(c.v);
        Inst a;
        for (int i = 0; i < 70; ++i) a.block(128, {on(0, 1 + i, 0, 30 + i % 60)});
        a.run(640);
        const uint32_t want = c.expect ? c.expect : 64;  // no cap: the pool (64 voices)
        if (a.voices() != want) std::printf("  MAX_VOICES=\"%s\": %u voices, wanted %u\n", c.v, a.voices(), want);
        CHECK_EQ(a.voices(), want);
    }
    {
        Env env(nullptr, "65");  // above the per-sound ceiling: ignored
        Inst a;
        for (int i = 0; i < 70; ++i) a.block(128, {on(0, 1 + i, 0, 30 + i % 60)});
        a.run(640);
        CHECK_EQ(a.voices(), 64u);
    }
    {
        Env env(nullptr, "2");  // each sound: 2
        Inst a;
        char msg[64];
        const uint32_t b = a.contexts->prepare(a.p, "b", nullptr, 0, nullptr, nullptr, msg, sizeof msg);
        a.contexts->select(a.p, b, 1);
        a.block(128);
        for (int i = 0; i < 5; ++i) a.block(128, {on(0, 1 + i, 0, 60 + i), on(0, 11 + i, 1, 60 + i)});
        a.run(640);
        CHECK_EQ(a.voices(), 4u);
    }
}

TEST(caps_stand_without_a_host_limit_and_bound_a_higher_one) {
    Env env("4");
    Inst a;
    for (int i = 0; i < 6; ++i) a.block(128, {on(0, 1 + i, 0, 60 + i)});
    a.run(640);
    CHECK_EQ(a.voices(), 4u);
    a.limits(10);  // higher than the cap: the cap bounds it
    for (int i = 6; i < 9; ++i) a.block(128, {on(0, 1 + i, 0, 60 + i)});
    a.run(640);
    CHECK_EQ(a.voices(), 4u);
    a.limits(2);  // lower than the cap: the host's stands
    a.run(640);
    CHECK_EQ(a.voices(), 2u);
}

TEST(full_pool_cuts_a_voice_and_every_note_still_ends) {
    Inst a;
    for (int i = 0; i < 100; ++i) a.block(64, {on(0, 1 + i, 0, 20 + i % 90)});
    CHECK(a.voices() <= 64u);
    a.run(4800);
    CHECK(a.voices() <= 64u);
    a.block(128, {off(0, -1, -1, -1)});
    a.run(24000);
    for (int i = 0; i < 100; ++i) CHECK_EQ(a.ends(1 + i), 1);
}

TEST(notes_and_active_voices_report) {
    Inst a;
    a.block(128, {on(0, 1, 0, 60, 1.0), on(0, 2, 3, 62, 1.0), volume(0, 2, 3, 62, 0.25)});
    a.run(2400);
    double cost = -1;
    CHECK_EQ(a.budget->active_voices(a.p, &cost), 2u);
    CHECK(cost >= 0);
    oscairn_voice_budget_note_t n[8]{};
    CHECK_EQ(a.budget->notes(a.p, n, 8), 2u);
    CHECK_EQ(a.budget->notes(a.p, n, 1), 1u);
    a.budget->notes(a.p, n, 8);
    float l1 = 0, l2 = 0;
    for (auto& x : n) { if (x.note_id == 1) { l1 = x.loudness; CHECK(x.channel == 0 && x.key == 60 && x.voices == 1); } if (x.note_id == 2) { l2 = x.loudness; CHECK(x.channel == 3 && x.key == 62); } }
    CHECK(l1 > 0.9f && l1 <= 1.01f && l2 > 0.2f && l2 < 0.3f);
    a.block(128, {off(0, 1, 0, 60)});  // releasing voices are still counted until they end
    CHECK_EQ(a.voices(), 2u);
    a.run(24000);
    CHECK_EQ(a.voices(), 1u);
}

// A unison note is 2 (mode 1) or 4 (mode 2) engine voices but one note: the limits, active_voices and notes() count it once,
// and it is given up whole (both copies fade and end together; never one copy alone).
TEST(unison_note_counts_as_one_and_goes_whole) {
    for (int mode : {1, 2}) {
        Inst a;
        char msg[64];
        const uint32_t u = a.contexts->prepare(a.p, "u", nullptr, 0, mode == 1 ? "Unison=1 UnisonDetune=6 AmplitudeEGReleaseTime=64" : "Unison=2 UnisonDetune=6 AmplitudeEGReleaseTime=64", nullptr, msg, sizeof msg);
        a.contexts->select(a.p, u, 1);
        a.limits(2);
        a.block(128);
        for (int i = 0; i < 3; ++i) { a.block(128, {on(0, 1 + i, 1, 60 + i)}); a.run(640); }  // the third note gives up the first, whole
        CHECK_EQ(a.voices(), 2u);
        CHECK((ended(a) == std::vector<int>{1}));
        oscairn_voice_budget_note_t n[8]{};
        CHECK_EQ(a.budget->notes(a.p, n, 8), 2u);
        for (int k = 0; k < 2; ++k) CHECK_EQ(n[k].voices, 1u);
        a.limits(1);
        a.run(640);
        CHECK_EQ(a.voices(), 1u);
        CHECK((ended(a) == std::vector<int>{1, 2}));
        a.block(128, {off(0, 3, 1, 62)});
        a.run(48000);
        CHECK_EQ(a.voices(), 0u);
        CHECK_EQ(a.ends(3), 1);
    }
}

// An arpeggiator's stream: sixteenths at 84 BPM, each note-off on the same frame as the next note-on (off first, or on
// first), the same key struck again in a row as well as other keys, at the block's first, middle and last frame, with the
// device's cap of 6 voices a sound and without. Every note sounds at once and at full level, and ends exactly once.
TEST(arpeggio_same_frame_off_on_never_loses_a_note) {
    const int keys[] = {48, 52, 55, 52, 48, 48, 48, 55, 55, 52};
    const uint32_t step = 8571;  // 48000 x 60 / 84 / 4
    for (const char* cap : {(const char*)nullptr, "6"})
        for (bool offFirst : {true, false})
            for (uint32_t at : {0u, 64u, 127u}) {
                Env env(nullptr, cap);
                Inst a;
                const int n = int(sizeof keys / sizeof keys[0]) * 3;
                std::vector<float> level(size_t(n), 0.f);
                for (int i = 0; i <= n; ++i) {
                    std::vector<Bytes> ev;
                    const int k = keys[i % 10], prev = keys[(i + 9) % 10];
                    if (i > 0 && offFirst) ev.push_back(off(at, i - 1, 0, int16_t(prev)));
                    if (i < n) ev.push_back(on(at, i, 0, int16_t(k)));
                    if (i > 0 && !offFirst) ev.push_back(off(at, i - 1, 0, int16_t(prev)));
                    const size_t from = a.L.size() + at;
                    a.block(128, ev);
                    for (uint32_t done = 128; done < step; done += 128) a.block(std::min<uint32_t>(128, step - done));
                    if (i == n) break;
                    oscairn_voice_budget_note_t r[64]{};
                    const uint32_t m = a.budget->notes(a.p, r, 64);
                    for (uint32_t j = 0; j < m; ++j) if (r[j].note_id == i) level[size_t(i)] = r[j].loudness;
                    CHECK(peak(a.L, from, from + 960) > 0.05f);  // sounding within 20 ms
                }
                a.run(48000 * 3);
                for (int i = 0; i < n; ++i) {
                    if (level[size_t(i)] < 0.9f) std::printf("  cap %s, %s, frame %u: note %d at %.3f\n", cap ? cap : "none", offFirst ? "off first" : "on first", at, i, level[size_t(i)]);
                    CHECK(level[size_t(i)] >= 0.9f);  // still at full level (the amplitude envelope, sustain 1) one step after its start
                    CHECK_EQ(a.ends(i), 1);
                }
                CHECK(!a.badStatus && !a.badTime);
            }
}
