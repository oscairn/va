// SPDX-License-Identifier: AGPL-3.0-or-later
// Parameters and state: metadata, values set by events, and a versioned state blob that round
// trips byte for byte and refuses anything malformed without changing the instance.
#include "host.h"

TEST(parameter_metadata) {
    Inst a(false);
    const uint32_t n = a.params->count(a.p);
    CHECK_EQ(n, 50u);  // level, pan, polyphony limit, knobs 1-8, ribbon, switches 1-2, 24 destinations (ids 14-37), foot controller 2 and foot switch (38-39),
                       // the oscillators' waves and the portamento time (destinations, 40-43; docs/parameters.md), the output bus (44), the pitch, every LFO's speed and the amplitude LFO's depth, the unison detune and spread (destinations, 45-49)
    std::vector<clap_id> seen;
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        CHECK(a.params->get_info(a.p, i, &info));
        CHECK(std::find(seen.begin(), seen.end(), info.id) == seen.end());
        seen.push_back(info.id);
        CHECK(std::isfinite(info.min_value) && info.min_value <= info.default_value && info.default_value <= info.max_value);
        CHECK(info.name[0] != 0);
        // a destination shows the sound's own value (the test sound's amplitude release is 112), the others their default
        const bool dest = (info.id >= 14 && info.id <= 37) || (info.id >= 40 && info.id <= 43) || (info.id >= 45 && info.id <= 49);  // 44: the output bus
        if (!dest) CHECK(std::fabs(a.value(info.id) - info.default_value) < 1e-6);
        else CHECK(a.value(info.id) >= info.min_value && a.value(info.id) <= info.max_value);
        CHECK(((info.flags & CLAP_PARAM_IS_MODULATABLE) != 0) == dest);
        char text[64] = {};
        CHECK(a.params->value_to_text(a.p, info.id, info.default_value, text, sizeof text) && text[0]);
        double back = -9;
        CHECK(a.params->text_to_value(a.p, info.id, text, &back) && std::fabs(back - info.default_value) < 1e-2);
    }
    clap_param_info_t none{};
    CHECK(!a.params->get_info(a.p, n, &none));
}

TEST(parameter_events_change_values_and_sound) {
    Inst a;
    a.block(128, {on(0, 1, 0, 60, 1.0)});
    a.run(4800);
    const float loud = peak(a.L, a.L.size() - 1000);
    a.block(128, {param(0, 0, 0.1), param(0, 1, 1.0), param(0, 2, 12.4)});
    a.run(4800);
    CHECK(std::fabs(a.value(0) - 0.1) < 1e-6 && a.value(1) == 1.0 && a.value(2) == 12);  // stepped: rounded
    const float soft = peak(a.L, a.L.size() - 1000);
    CHECK(soft < 0.2f * loud);
    CHECK(peak(a.L, a.L.size() - 1000) < 1e-5f);  // panned hard right: no left
    CHECK(peak(a.R, a.R.size() - 1000) > 0.f);
    a.block(128, {param(0, 0, 7.0), param(0, 5, 1.0), param(0, 1, NAN)});  // clamped, unknown id, non-finite: ignored
    CHECK(a.value(0) == 1.0 && a.value(1) == 1.0);
}

TEST(state_round_trip_is_byte_identical) {
    Inst a;
    a.block(128, {param(0, 0, 0.3), param(0, 1, -0.5), param(0, 2, 10)});
    const std::string s = a.save();
    CHECK(s.rfind("oscairn-va-state 5\n", 0) == 0);
    Inst b;
    CHECK(b.load(s));
    CHECK_EQ(b.save() == s, true);
    CHECK(std::fabs(b.value(0) - 0.3) < 1e-6 && std::fabs(b.value(1) + 0.5) < 1e-6 && b.value(2) == 10);
    // and it plays as the saved instance does
    a.clearAudio(); b.clearAudio();
    a.block(128, {on(0, 1, 0, 60)}); b.block(128, {on(0, 1, 0, 60)});
    a.run(4800); b.run(4800);
    CHECK(a.L == b.L && a.R == b.R);
    // the default state round trips too
    Inst d1, d2;
    CHECK(d2.load(d1.save()) && d2.save() == d1.save());
}

TEST(state_refuses_what_is_malformed) {
    Inst a;
    a.block(128, {param(0, 0, 0.3)});
    const std::string good = a.save();
    Inst b;
    const std::string before = b.save();
    const char* bad[] = {"", "oscairn-va-state 4\n0.5 0 8\n", "oscairn-va-state 4\n", "oscairn-va-state 1\n0.5 0\n",
                         "oscairn-va-state 2\n0.5 0 8 \"vcore-patch/1\"\n",  // the format before the knobs: no reader
                         "oscairn-va-state 4\n2 0 8 0 0 0 0 0 0 0 0 0 0 0 \"vcore-patch/1\"\n", "oscairn-va-state 4\nnan 0 8 0 0 0 0 0 0 0 0 0 0 0 \"vcore-patch/1\"\n",
                         "oscairn-va-state 4\n0.5 0 8 0 0 0 0 0 0 0 1.5 0 0 0 \"vcore-patch/1\"\n", "oscairn-va-state 4\n0.5 0 8 9\n",
                         "oscairn-va-state 4\n0.5 0 8 0 0 0 0 0 0 0 0 0 0 0 \"vcore-patch/1\"\ngarbage\n", "not a state"};
    for (auto* s : bad) { CHECK(!b.load(s)); CHECK(b.save() == before); }
    CHECK(!b.load(std::string(1 << 20, 'x')));
    CHECK(b.save() == before);
    CHECK(b.load(good));
}

TEST(sound_context_snapshot_round_trip) {
    Inst a;
    char msg[64];
    const uint32_t b = a.contexts->prepare(a.p, "bright \"b\"", nullptr, 0, "level=0.25 pan=0.5 polyphony=7", nullptr, msg, sizeof msg);
    CHECK(b != CLAP_INVALID_ID);
    a.contexts->select(a.p, b, 2);
    a.block(128);
    Stream s;
    CHECK(a.contexts->state->save(a.p, &s.out));
    CHECK(s.data.rfind("oscairn-va-contexts/5\n", 0) == 0);
    Inst c;
    Stream in; in.data = s.data;
    CHECK(c.contexts->state->load(c.p, &in.in));
    Stream s2;
    CHECK(c.contexts->state->save(c.p, &s2.out));
    // contexts get fresh IDs on load, which the snapshot does not carry
    CHECK(s2.data == s.data);
    oscairn_sound_context_info_t info{};
    CHECK(c.contexts->get(c.p, 1, &info) && !std::strcmp(info.name, "bright \"b\"") && (info.selected_channels == 4));
    // a snapshot that is wrong changes nothing
    const char* bad[] = {"", "oscairn-va-contexts/4\n", "oscairn-va-contexts/4\n0\n", "oscairn-va-contexts/2\n1 \"Default\" 0.5 0 8\n"};
    for (auto* t : bad) { Stream x; x.data = t; CHECK(!c.contexts->state->load(c.p, &x.in)); }
    Stream s3;
    CHECK(c.contexts->state->save(c.p, &s3.out) && s3.data == s.data);
}

TEST(memory_is_declared) {
    Inst a(false);
    oscairn_memory_t m{};
    CHECK(a.memory->estimate(a.p, kRate, 4096, &m));
    CHECK(m.shared_bytes > 0 && m.context_bytes > 0 && m.asset_bytes == 0);
    a.p->activate(a.p, kRate, 1, 4096);
    oscairn_memory_t c{};
    CHECK(a.memory->current(a.p, &c) && c.shared_bytes > 0);
    a.p->deactivate(a.p);
}

// The live knobs. A controller box from knob 1 to the filter EG's depth (ratio +64, bipolar) at knob 0, 0.5
// and 1 sounds as the same patch without the box at depth 161, 257 and 353: 1.5 raw steps a unit of
// ratio x curve, which the reference shows exactly (a knob-chain measurement run, batch b5: the 2nd-12th harmonics of
// box_k0 / k512 / k1023 equal static_161 / 257 / 353 within 0.1 dB after 0.5 s).
constexpr double kKnobDefault = 512.0 / 1023;  // the model's default knob value
static const char* kBoxBase = "polyphony=1 VoltageDrift=0 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterCutoff=636 Filter1FilterResonance=50 "
    "FilterCutoffEGAttackTime=30 FilterCutoffEGDecayTime=117 FilterCutoffEGSustainLevel=41 FilterCutoffEGReleaseTime=145 ";
static double brightness(Inst& a, uint32_t ctx, double knob) {  // dB: the first difference's power re the signal's, 0.5-1.0 s
    a.contexts->select(a.p, ctx, 0);
    a.block(128, {param(0, 3, knob, 0)});
    a.clearAudio();
    a.block(128, {on(0, 1, 0, 60)});
    a.run(48000);
    a.block(128, {off(0, 1, 0, 60)});
    a.run(48000);
    double d = 0, x = 0;
    for (size_t i = 24000; i < 48000; ++i) { const double v = a.L[i], u = v - a.L[i - 1]; x += v * v; d += u * u; }
    return 10 * std::log10(d / x);
}
TEST(knob_drives_a_filter_eg_box_live) {
    Inst a;
    char msg[160] = {};
    const std::string base = kBoxBase;
    auto make = [&](const std::string& t) { const uint32_t id = a.contexts->prepare(a.p, "s", nullptr, 0, t.c_str(), nullptr, msg, sizeof msg); CHECK(id != CLAP_INVALID_ID); return id; };
    const uint32_t box = make(base + "Filter1FilterCutoffEGDepth=257 Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 "
                                     "Box1ControllerSetDestination=143 Box1ControllerSetRatio=192 Box1ControllerSetPolarity=1");
    const uint32_t s161 = make(base + "Filter1FilterCutoffEGDepth=161"), s257 = make(base + "Filter1FilterCutoffEGDepth=257"),
                   s353 = make(base + "Filter1FilterCutoffEGDepth=353");
    const double b0 = brightness(a, box, 0), b5 = brightness(a, box, 512.0 / 1023), b1 = brightness(a, box, 1);
    const double r0 = brightness(a, s161, kKnobDefault), r5 = brightness(a, s257, kKnobDefault), r1 = brightness(a, s353, kKnobDefault);
    std::printf("  box at knob 0 / 0.5 / 1: %.2f / %.2f / %.2f dB; static 161 / 257 / 353: %.2f / %.2f / %.2f dB\n", b0, b5, b1, r0, r5, r1);
    CHECK(std::fabs(b0 - r0) < 0.3 && std::fabs(b5 - r5) < 0.3 && std::fabs(b1 - r1) < 0.3);
    CHECK(b5 > b0 + 0.7 && b1 > b5 + 0.7);  // and the knob does move it
}

TEST(knob_values_round_trip_in_the_state) {
    Inst a;
    a.block(128, {param(0, 3, 0.25), param(0, 10, 1.0)});
    const std::string s = a.save();
    Inst b;
    CHECK(b.load(s) && b.save() == s);
    CHECK(std::fabs(b.value(3) - 0.25) < 1e-6 && b.value(10) == 1.0 && std::fabs(b.value(4) - kKnobDefault) < 1e-6);
}
