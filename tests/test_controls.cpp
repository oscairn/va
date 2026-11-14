// SPDX-License-Identifier: AGPL-3.0-or-later
// The controller box sources and destinations through the CLAP ABI: a parameter modulation on a destination sounds as the
// linear box that would move it as far; the ribbon and switch parameters, the mod wheel, breath, pitch bend and pressure
// drive their boxes; the controllers a host never passes (the ribbon's and the switches' CCs) do nothing.
#include <oscairn-clap/ui.h>
#include "host.h"

namespace {
// A saw through LPF24 at 500, one voice: a box on the cutoff (destination 142) moves it audibly.
const std::string kBase = "VoltageDrift=0 Osc2OscillatorOutLevel=0 Osc3OscillatorOutLevel=0 Filter1FilterCutoff=500 Filter1FilterResonance=0 "
                          "AmplitudeEGSustainLevel=511 AmplitudeEGDecayTime=255 PartReceivePitchBend=0 ";  // the bend only as a box source
std::string box(int source, int ratio = 136) {
    return " Box1ControllerSetSwitch=1 Box1ControllerSetSource=" + std::to_string(source) +
           " Box1ControllerSetDestination=142 Box1ControllerSetPolarity=0 Box1ControllerSetRatio=" + std::to_string(ratio);
}
inline Bytes mod(uint32_t t, clap_id id, double amount, int16_t ch = -1, int32_t note = -1) {
    clap_event_param_mod_t e{};
    e.header = {sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
    e.param_id = id; e.note_id = note; e.port_index = -1; e.channel = ch; e.key = -1; e.amount = amount;
    return bytes(e);
}
inline Bytes pressure(uint32_t t, int16_t ch, int16_t key, double v) {
    clap_event_note_expression_t e{};
    e.header = {sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
    e.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE; e.note_id = -1; e.port_index = 0; e.channel = ch; e.key = key; e.value = v;
    return bytes(e);
}
// One note of 0.5 s on a fresh instance whose default sound is `text`, after the events `pre` (a block of their own).
std::vector<float> play(const std::string& text, const std::vector<Bytes>& pre) {
    Inst a;
    if (!a.state) return {};
    CHECK(a.load("oscairn-va-state 4\n1 0 1" + std::string(kKnobs) + " \"vcore-patch/1 " + text + "\"\n"));
    a.block(128, pre);
    a.clearAudio();
    a.block(128, {on(0, 1, 0, 60, 0.8)});
    a.run(24000);
    return a.L;
}
double rmsDiff(const std::vector<float>& x, const std::vector<float>& y) {
    double d = 0, s = 0;
    for (size_t i = 0; i < std::min(x.size(), y.size()); ++i) { d += double(x[i] - y[i]) * (x[i] - y[i]); s += double(x[i]) * x[i]; }
    return x.empty() || y.size() != x.size() ? 1e9 : std::sqrt(d / (s + 1e-30));
}
}  // namespace

// A modulation of a raw steps on the cutoff parameter (id 18) sounds as a linear box moving the cutoff a steps: the mod wheel
// at 127 through a unipolar box of ratio +8 gives (1024 x 8 / 8) = 1024 of 8192, 127.875 raw steps (toward zero: 127).
TEST(param_mod_on_a_destination_equals_the_linear_box) {
    const auto withBox = play(kBase + box(1), {midi(0, 0xB0, 1, 127)});
    const auto withMod = play(kBase, {mod(0, 18, 127.875)});
    const auto none = play(kBase, {});
    const double same = rmsDiff(withBox, withMod), moved = rmsDiff(withBox, none);
    std::printf("  box against modulation: %.2e relative; against neither: %.2e\n", same, moved);
    CHECK(same < 1e-6 && moved > 0.1);
    const auto perNote = play(kBase, {mod(0, 18, 127.875, -1, 7)});  // a per-note modulation is not applied
    CHECK(rmsDiff(perNote, none) < 1e-6);
    const auto absolute = play(kBase, {param(0, 18, 627)});  // a value event sets the destination: 500 + 127
    CHECK(rmsDiff(absolute, withBox) < 1e-6);
}

// Every live source the host passes, at the same 7-bit value, drives its box as the mod wheel does: breath (CC 2), pitch bend
// (8192 + 64: the box sees 516, as CC value 64), pressure (64 / 127), and the ribbon and switch parameters. The ribbon's and the
// switches' CCs (16, 86, 87) never reach a plugin in the host and move nothing.
TEST(sources_drive_their_boxes) {
    const auto wheel = play(kBase + box(1), {midi(0, 0xB0, 1, 64)});
    const auto none = play(kBase + box(1), {});
    CHECK(rmsDiff(wheel, none) > 0.05);
    const struct { const char* what; int source; std::vector<Bytes> ev; } cases[] = {
        {"breath", 7, {midi(0, 0xB0, 2, 64)}}, {"pitch bend", 0, {midi(0, 0xE0, 64, 64)}}, {"pressure", 2, {pressure(0, 0, -1, 64 / 127.0)}},
        {"channel pressure", 2, {midi(0, 0xD0, 64, 0)}}, {"ribbon", 6, {param(0, 11, 64 / 127.0)}}};
    for (const auto& c : cases) {
        const double d = rmsDiff(play(kBase + box(c.source), c.ev), wheel);
        std::printf("  %s against the mod wheel: %.2e\n", c.what, d);
        CHECK(d < 1e-6);
    }
    const auto sw = play(kBase + box(16), {param(0, 12, 1.0)}), wheelTop = play(kBase + box(1), {midi(0, 0xB0, 1, 127)});
    CHECK(rmsDiff(sw, wheelTop) < 1e-6 && rmsDiff(play(kBase + box(17), {param(0, 13, 1.0)}), wheelTop) < 1e-6);
    const auto off = play(kBase + box(6), {});
    CHECK(rmsDiff(play(kBase + box(6), {midi(0, 0xB0, 16, 127)}), off) < 1e-6);
    CHECK(rmsDiff(play(kBase + box(16), {midi(0, 0xB0, 86, 127)}), off) < 1e-6);
}

// The ribbon and switches are saved with the sound; a modulation is not (only the value).
TEST(panel_controls_saved_modulation_not) {
    Inst a;
    if (!a.state) return;
    const std::string before = a.save();
    a.block(128, {param(0, 11, 0.5), param(0, 12, 1.0), mod(0, 18, -40)});
    const std::string s = a.save();
    CHECK(s != before && s.find(" 0.5 1 0 ") != std::string::npos);
    Inst b;
    CHECK(b.load(s) && b.save() == s && std::fabs(b.value(11) - 0.5) < 1e-6 && b.value(12) == 1.0);
    a.block(128, {param(0, 11, 0), param(0, 12, 0)});
    CHECK(a.save() == before);  // the modulation left no trace
}

// oscairn.ui: a hint for every parameter, the roles the panel's banks need, two envelope views.
TEST(ui_hints) {
    Inst a(false);
    const auto* ui = static_cast<const oscairn_plugin_ui_t*>(a.p->get_extension(a.p, OSCAIRN_EXT_UI));
    CHECK(ui && ui->param_hint);
    if (!ui) return;
    int roles[64] = {}, views = 0;
    for (uint32_t i = 0; i < a.params->count(a.p); ++i) {
        clap_param_info_t info{}; a.params->get_info(a.p, i, &info);
        oscairn_param_hint_t h{};
        CHECK(ui->param_hint(a.p, info.id, &h));
        if (h.role < 64) ++roles[h.role];
        views += h.view != 0;
    }
    CHECK(roles[OSCAIRN_ROLE_CUTOFF] == 1 && roles[OSCAIRN_ROLE_RESONANCE] == 1 && roles[OSCAIRN_ROLE_FILTER_ENVELOPE] == 1);
    CHECK(roles[OSCAIRN_ROLE_ATTACK] == 1 && roles[OSCAIRN_ROLE_RELEASE] == 1 && roles[OSCAIRN_ROLE_LEVEL] == 1 && views == 6);
    oscairn_param_hint_t h{};
    CHECK(!ui->param_hint(a.p, 999, &h));
}
