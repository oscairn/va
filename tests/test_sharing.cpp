// SPDX-License-Identifier: AGPL-3.0-or-later
// Parts sharing one instance: each sound on its own channel and output bus plays as it would alone in an instance of
// its own; a sound saved alone (oscairn.sound-snapshots draft 4) prepares beside and saves the same; a retiring sound
// rings out and choke_retiring fades it.
#include "host.h"

namespace {
constexpr clap_id kBus = 44;
constexpr uint32_t kParamsWritable = 52;

// Part k's sound (k >= 1; 0 is the test Default): its own level, pan, cutoff, wave and release
std::string soundOf(int k) {
    return "level=" + std::to_string(0.9 - 0.05 * k) + " pan=" + std::to_string(-0.8 + 0.2 * k) + " " + kShortRelease +
           " Filter1FilterCutoff=" + std::to_string(300 + 80 * k) + " Osc1OscillatorWave=" + std::to_string(k % 3);
}
uint32_t prepare(Inst& a, const std::string& text) {
    char msg[200] = {};
    const uint32_t id = a.contexts->prepare(a.p, "part", nullptr, 0, text.c_str(), nullptr, msg, sizeof msg);
    if (id == CLAP_INVALID_ID) std::printf("  prepare: %s\n", msg);
    return id;
}
// The notes of part k on channel ch: two overlapping notes at odd frames, released in turn
void play(int k, int16_t ch, std::vector<std::vector<Bytes>>& blocks) {
    blocks[1].push_back(on(5 + 3 * k, 10 * k + 1, ch, int16_t(48 + k), 0.6 + 0.04 * k));
    blocks[4].push_back(on(70, 10 * k + 2, ch, int16_t(55 - k), 0.9));
    blocks[30].push_back(off(17, 10 * k + 1, ch, int16_t(48 + k)));
    blocks[60].push_back(off(100, 10 * k + 2, ch, int16_t(55 - k)));
}
double worstDiff(const std::vector<float>& x, const std::vector<float>& y) {
    if (x.size() != y.size()) return 1e9;
    double w = 0;
    for (size_t i = 0; i < x.size(); ++i) w = std::max(w, double(std::fabs(x[i] - y[i])));
    return w;
}
}  // namespace

// n sounds in one instance, sound k on channel k and output bus k + 1 (sound 0 the Default), play sample for sample what
// each plays alone in an instance of its own on its main port, port by port; the main port and the unused buses stay
// silent; nothing allocates in process().
TEST(shared_sounds_play_as_they_do_alone) {
    for (int n : {2, 5, 8}) {
        Inst a;
        a.outputs = 9;
        for (int k = 1; k < n; ++k) {
            const uint32_t id = prepare(a, soundOf(k));
            CHECK(id != CLAP_INVALID_ID);
            CHECK(a.contexts->select(a.p, id, int16_t(k)));
        }
        std::vector<std::vector<Bytes>> blocks(120);
        for (int k = 0; k < n; ++k) blocks[0].push_back(param(0, kBus, k + 1, int16_t(k)));
        for (int k = 0; k < n; ++k) play(k, int16_t(k), blocks);
        a.guard = allocGuardAvailable();
        long allocs = 0;
        for (auto& b : blocks) { a.block(128, b); allocs = std::max(allocs, a.allocations); }
        CHECK_EQ(allocs, 0);
        CHECK(silent(a, 0));
        for (int k = 0; k < n; ++k) {
            Inst b;
            if (k) b.contexts->select(b.p, prepare(b, soundOf(k)), -1);
            std::vector<std::vector<Bytes>> mine(120);
            play(k, 0, mine);
            for (auto& x : mine) b.block(128, x);
            CHECK(peak(b.L) > 0.01f);
            const double dl = worstDiff(a.PL[k], b.L), dr = worstDiff(a.PR[k], b.R);
            if (dl > 0 || dr > 0) std::printf("  %d sounds: bus %d differs by %g, %g\n", n, k + 1, dl, dr);
            CHECK(dl == 0 && dr == 0);
        }
        for (int k = n; k < 8; ++k) CHECK(peak(a.PL[k]) == 0 && peak(a.PR[k]) == 0);
        CHECK_EQ(a.endsTotal(), 2 * n);
        CHECK_EQ(a.voices(), 0u);
    }
}

// save_sound writes the sound alone, on the main port; prepared beside in another instance it selects one sound on
// every channel, saves byte for byte the same, and sounds the same. sound_values: every writable value of a sound no
// channel selects, none for a selected one. A state or a snapshot loaded selects its sound on every channel. A port
// never activated plays into main.
TEST(a_sound_saved_alone_prepares_beside_the_same) {
    Inst a;
    a.outputs = 9;
    const uint32_t id = prepare(a, soundOf(3) + " bus=4 knob1=0.25");
    CHECK(a.snapshots && a.snapshots->save_sound && a.snapshots->sound_values && a.snapshots->ring_out && a.snapshots->choke_retiring);
    if (!a.snapshots) return;
    Stream one;
    CHECK(a.snapshots->save_sound(a.p, id, &one.out));
    Stream none;
    CHECK(!a.snapshots->save_sound(a.p, id + 50, &none.out));
    oscairn_sound_parameter_t v[64] = {};
    CHECK_EQ(a.snapshots->sound_values(a.p, id, v, 64), kParamsWritable);
    CHECK(v[kBus].id == kBus && v[kBus].value == 4 && std::fabs(v[3].value - 0.25) < 1e-6);
    CHECK_EQ(a.snapshots->sound_values(a.p, 0, v, 64), 0u);  // the Default: selected
    Inst b;
    uint32_t sel[16] = {};
    Stream in; in.data = one.data;
    char msg[200] = {};
    CHECK(b.snapshots->prepare(b.p, &in.in, sel, msg, sizeof msg));
    for (int ch = 0; ch < 16; ++ch) CHECK(sel[ch] == sel[0] && sel[0] != 0);
    Stream again;
    CHECK(b.snapshots->save_sound(b.p, sel[0], &again.out));
    CHECK(again.data == one.data);
    CHECK(b.snapshots->sound_values(b.p, sel[0], v, 64) == kParamsWritable && v[kBus].value == 0);
    a.contexts->select(a.p, id, 2);
    b.contexts->select(b.p, sel[0], -1);
    a.block(128); b.block(128);
    a.clearAudio(); b.clearAudio();
    for (auto& x : a.PL) x.clear();
    for (auto& x : a.PR) x.clear();
    std::vector<std::vector<Bytes>> ea(60);
    ea[0] = {on(9, 1, 2, 60)}; ea[20] = {off(3, 1, 2, 60)};
    for (auto& e : ea) { a.block(128, e); b.block(128, e); }
    CHECK(peak(b.L) > 0.01f);
    CHECK(worstDiff(a.PL[3], b.L) == 0 && worstDiff(a.PR[3], b.R) == 0);
    CHECK(silent(a, 0));
    Inst c;
    Stream whole; whole.data = one.data;
    CHECK(c.contexts->state->load(c.p, &whole.in));
    oscairn_sound_context_info_t info{};
    CHECK(c.contexts->get(c.p, 0, &info) && info.selected_channels == 0xffff);
    Inst d;
    const uint32_t other = prepare(d, "level=1");
    d.contexts->select(d.p, other, -1);
    d.block(128);
    CHECK(d.load(d.save()));
    d.block(128);
    CHECK(d.contexts->get(d.p, 0, &info) && info.selected_channels == 0xffff);
    Inst e;
    e.outputs = 9;
    e.activatePorts = false;
    e.contexts->select(e.p, prepare(e, "level=1 bus=6"), -1);
    e.block(128, {on(0, 1, 0, 60)});
    e.run(1024);
    CHECK(peak(e.L, e.L.size() - 512) > 0.01f);
}

// A retiring sound's main output rings out into ring-out channels 0 and 1 until its notes finish; at a second change
// choke_retiring fades the older tail out through channels 2 and 3 (the choke time) while the newer tail takes 0 and 1.
// Every note ends once and the voices reach zero.
TEST(retiring_sounds_ring_out_and_choke) {
    Inst a;
    a.ring = true;
    const uint32_t first = prepare(a, "level=1 AmplitudeEGReleaseTime=60");
    a.contexts->select(a.p, first, -1);
    a.block(128, {on(0, 1, 0, 60)});
    a.run(2400);
    CHECK(peak(a.L) > 0.01f && peak(a.RL[0]) == 0);
    auto change = [&](bool chokeFirst) {
        Stream s; CHECK(a.snapshots->save_sound(a.p, first, &s.out));
        Stream in; in.data = s.data;
        uint32_t sel[16] = {}; char msg[200] = {};
        CHECK(a.snapshots->prepare(a.p, &in.in, sel, msg, sizeof msg));
        if (chokeFirst) CHECK(a.snapshots->choke_retiring(a.p));
        CHECK(a.contexts->select(a.p, sel[0], -1));
        return sel[0];
    };
    change(false);
    size_t at = a.L.size();
    a.block(128);
    a.run(2400);
    CHECK(peak(a.L, at) < 1e-6f && peak(a.RL[0], at) > 0.01f && peak(a.RL[1], at) == 0);
    a.block(128, {on(0, 2, 0, 62)});
    a.run(2400);
    CHECK(peak(a.L, a.L.size() - 1200) > 0.01f);
    change(true);
    at = a.L.size();
    a.block(128);
    a.run(2400);
    CHECK(peak(a.RL[1], at) > 1e-3f && peak(a.RL[1], at + kChoke + 16) < 1e-6f);  // faded out within the choke time (and its control step)
    CHECK_EQ(a.ends(1), 1);
    CHECK_EQ(a.ends(2), 0);
    CHECK(peak(a.RL[0], a.L.size() - 1200) > 0.01f && peak(a.L, at) < 1e-6f);
    a.block(128, {off(0, 2, 0, 62)});
    a.run(48000 * 4);
    CHECK_EQ(a.ends(2), 1);
    CHECK_EQ(a.endsTotal(), 2);
    CHECK_EQ(a.voices(), 0u);
}

// Each sound sharing the instance keeps its own voice limit and its own voice count.
TEST(shared_sounds_keep_their_own_voices) {
    Inst a;
    const uint32_t x = prepare(a, "level=1 bus=1"), y = prepare(a, "level=1 bus=2");
    a.contexts->select(a.p, x, 1);
    a.contexts->select(a.p, y, 2);
    a.block(128);
    a.limits(0, {{x, 1, 0, 5, 0}});
    a.block(128, {on(0, 1, 1, 60), on(0, 2, 1, 62), on(0, 3, 1, 64), on(0, 4, 2, 60), on(0, 5, 2, 62), on(0, 6, 2, 64)});
    a.run(1024);
    oscairn_sound_context_info_t ix{}, iy{};
    for (uint32_t slot = 0; slot < a.contexts->capacity(a.p); ++slot) {
        oscairn_sound_context_info_t i{};
        if (!a.contexts->get(a.p, slot, &i)) continue;
        if (i.id == x) ix = i;
        if (i.id == y) iy = i;
    }
    CHECK_EQ(ix.active_voices, 1u);
    CHECK_EQ(iy.active_voices, 3u);
    CHECK_EQ(a.voices(), 4u);
    oscairn_memory_t m{};
    CHECK(a.memory->current(a.p, &m) && m.context_bytes > 0);
}
