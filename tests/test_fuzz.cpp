// SPDX-License-Identifier: AGPL-3.0-or-later
// Stability under anything reachable live: random patches over the full raw ranges of the parameters the
// core implements, controller boxes from every source the core takes (knobs, mod wheel, bend, aftertouch, foot, breath,
// ribbon, switches) on every destination it moves, at any ratio, polarity and curve, random live knob, ribbon and switch
// values, destination values and modulations far past their ranges, the MIDI controllers a host passes, pressure, pitch
// bends and notes, at most 8 voices. The output must stay finite and under +12 dBFS, and the state must still round-trip afterwards.
#include <random>
#include <string>
#include "host.h"

namespace {
struct Def { const char* id; int lo, hi; };
const Def kDefs[] = {
#define X(id, name, lo, hi, def) {#id, lo, hi},
#include "../core/params.inc"
#undef X
};
// the same draws with every standard library (the <random> distributions are not portable)
int in(std::mt19937& r, int lo, int hi) { return lo + int(r() % uint32_t(hi - lo + 1)); }
double real(std::mt19937& r, double lo, double hi) { return lo + (hi - lo) * (r() >> 8) * (1.0 / 16777216); }
const Def* def(const char* id) { for (auto& d : kDefs) if (!std::strcmp(d.id, id)) return &d; return nullptr; }

// The parameters the core reads (core/laws.cpp, core/patch.cpp describe), each over its whole raw range.
const char* const kFuzzed[] = {
    "VoltageDrift", "FilterCutoffEGAttackTime", "FilterCutoffEGDecayTime", "FilterCutoffEGSustainLevel",
    "FilterCutoffEGReleaseTime", "AmplitudeLevel", "PartFEGDepth", "PartFilterCutoffFrequency", "AmplitudeEGAttackTime", "AmplitudeEGDecayTime",
    "AmplitudeEGSustainLevel", "AmplitudeEGReleaseTime", "Osc1OscillatorOctave", "Osc1OscillatorPitch",
    "Osc1OscillatorPulseWidth", "Osc1OscillatorOutLevel", "Osc2OscillatorOctave", "Osc2OscillatorPitch",
    "Osc2OscillatorPulseWidth", "Osc2OscillatorOutLevel", "Filter1FilterCutoff", "Filter1FilterCutoffEGDepth",
    "Filter1FilterResonance", "Filter1FilterOutLevel", "Filter2FilterCutoff", "Filter2FilterResonance",
    "PartPitchBendRangeUpper", "PartPitchBendRangeLower", "PartNoteShift", "PartMonoPolyMode", "PartKeyAssignMode", "PortamentoMaster",
    "PartPortamentoSwitch", "PartPortamentoTime", "PartPortamentoMode", "PartPortamentoTimeMode",
    "PitchLFOWave", "PitchLFOSpeed", "PitchLFOKeyOnReset", "PitchLFOPhase", "PitchLFODelayTime", "PitchLFOFadeInTime",
    "Osc1OscillatorPitchLFODepth", "Osc2OscillatorPitchLFODepth", "Osc3OscillatorPitchLFODepth", "Osc3OscillatorOctave", "Osc3OscillatorPitch",
    "Osc3OscillatorOutLevel", "Osc1OscillatorPulseWidthLFODepth", "Osc2OscillatorPulseWidthLFODepth", "Osc1OscillatorLFOWave", "Osc1OscillatorLFOSpeed",
    "Osc1OscillatorLFOKeyOnReset", "Osc2OscillatorLFOWave", "Osc2OscillatorLFOSpeed", "FilterCutoffLFOWave", "FilterCutoffLFOSpeed",
    "Filter1FilterCutoffLFODepth", "AmplitudeLFOWave", "AmplitudeLFOSpeed", "AmplitudeLFOKeyOnReset", "AmplitudeLevelLFODepth",
};

std::string randomPatch(std::mt19937& r) {
    auto in = [&](int lo, int hi) { return ::in(r, lo, hi); };
    std::string t = "vcore-patch/1";
    for (const char* id : kFuzzed) { const Def* d = def(id); t += std::string(" ") + id + "=" + std::to_string(in(d->lo, d->hi)); }
    for (int n = 1; n <= 3; ++n) t += " Osc" + std::to_string(n) + "OscillatorWave=" + std::to_string(in(0, 4));
    t += " Filter1FilterType=" + std::to_string(in(0, 1)) + " Filter2FilterType=" + std::to_string(in(0, 1) * 5);
    static const int kSrc[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    static const int kDst[] = {124, 136, 139, 140, 141, 142, 143, 145, 147, 148, 149};
    for (int b = 1; b <= 6; ++b) {
        const std::string B = " Box" + std::to_string(b) + "ControllerSet";
        t += B + "Switch=1" + B + "Source=" + std::to_string(kSrc[in(0, 15)]) + B + "Destination=" + std::to_string(kDst[in(0, 10)]) +
             B + "Polarity=" + std::to_string(in(0, 1)) + B + "Ratio=" + std::to_string(in(0, 255)) +
             B + "CurveType=" + std::to_string(in(0, 2) * in(0, 1) * 2) + B + "CurveParameter1=" + std::to_string(in(0, 10)) +
             B + "CurveParameter2=" + std::to_string(in(0, 4));
    }
    return t;
}
}  // namespace

TEST(fuzz_output_stays_finite_and_bounded) {
    std::mt19937 r(20261114);
    Inst a;
    if (!a.state) return;
    const std::string knobs = kKnobs;
    float worst = 0;
    std::string worstText;
    long blocks = 0;
    bool finite = true;
    for (int patch = 0; patch < 40; ++patch) {
        const std::string text = randomPatch(r);
        CHECK(a.load("oscairn-va-state 4\n1 0 8" + knobs + " \"" + text + "\"\n"));
        a.clearAudio();
        int32_t id = patch * 1000;
        std::vector<int> held;
        for (int k = 0; k < 100; ++k, ++blocks) {
            std::vector<Bytes> ev;
            const uint32_t t = in(r, 0, 127);
            switch (in(r, 0, 7)) {
            case 0: case 1: {
                const int key = in(r, 0, 127);
                ev.push_back(on(t, ++id, 0, int16_t(key), real(r, 0.01, 1)));
                held.push_back(key);
                break;
            }
            case 2:
                if (!held.empty()) { ev.push_back(off(t, -1, 0, int16_t(held.back()))); held.pop_back(); }
                break;
            case 3:  // a knob jumps anywhere, often to an end
                {
                const int how = in(r, 0, 2);
                ev.push_back(param(t, clap_id(3 + in(r, 0, 7)), how < 2 ? double(how) : real(r, 0, 1)));
                }
                break;
            case 4: {
                const int v = in(r, 0, 16383);
                ev.push_back(midi(t, 0xE0, uint8_t(v & 127), uint8_t(v >> 7)));
                break;
            }
            case 5: {  // a controller a host passes jumps, often to an end; or channel or note pressure; or reset all controllers
                static const int kCc[] = {1, 2, 4, 11, 64, 121};
                const int how = in(r, 0, 2), v = how < 2 ? how * 127 : in(r, 0, 127);
                const int kind = in(r, 0, 7);
                if (kind == 0) ev.push_back(midi(t, 0xD0, uint8_t(v), 0));
                else if (kind == 1) {
                    clap_event_note_expression_t e{};
                    e.header = {sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
                    e.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE; e.note_id = -1; e.port_index = 0; e.channel = 0; e.key = -1; e.value = v / 127.0;
                    ev.push_back(bytes(e));
                } else ev.push_back(midi(t, 0xB0, uint8_t(kCc[in(r, 0, 5)]), uint8_t(v)));
                break;
            }
            case 6: {  // the ribbon or a switch (ids 11-13) jumps; or a destination (14-27) is set anywhere, or modulated far past its range
                const clap_id id = clap_id(in(r, 11, 27));
                clap_param_info_t info{}; a.params->get_info(a.p, id, &info);
                if (id >= 14 && in(r, 0, 1)) {
                    clap_event_param_mod_t e{};
                    e.header = {sizeof e, t, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
                    e.param_id = id; e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
                    e.amount = real(r, -2, 2) * (info.max_value - info.min_value);
                    ev.push_back(bytes(e));
                } else ev.push_back(param(t, id, in(r, 0, 2) < 2 ? (in(r, 0, 1) ? info.max_value : info.min_value) : real(r, info.min_value, info.max_value)));
                break;
            }
            default: break;
            }
            a.block(128, ev);
        }
        for (size_t i = 0; i < a.L.size(); ++i) {
            const float x = std::max(std::fabs(a.L[i]), std::fabs(a.R[i]));
            if (!std::isfinite(x)) finite = false; else if (x > worst) { worst = x; worstText = text; }
        }
        a.block(128, {off(0, -1, 0, -1)});
    }
    std::printf("  %ld blocks, peak %.3f (%.1f dBFS)\n", blocks, worst, 20 * std::log10(worst + 1e-30));
    CHECK(finite);
    if (worst >= 3.98f) std::printf("  loudest patch: %s\n", worstText.c_str());
    CHECK(worst < 3.98f);  // +12 dBFS
    const std::string s = a.save();
    Inst b;
    CHECK(b.load(s) && b.save() == s);
}

// The case that blew up (peak 2.7e8): a knob box on the filter EG depth at ratio +127, the knob moved to 1 live.
TEST(knob_box_at_full_ratio_stays_bounded) {
    Inst a;
    if (!a.state) return;
    CHECK(a.load(std::string("oscairn-va-state 4\n1 0 64") + kKnobs + " \"vcore-patch/1 Filter1FilterCutoff=636 Filter1FilterResonance=50 "
                 "Filter1FilterCutoffEGDepth=257 FilterCutoffEGAttackTime=30 FilterCutoffEGDecayTime=117 FilterCutoffEGSustainLevel=41 "
                 "Box1ControllerSetSwitch=1 Box1ControllerSetSource=8 Box1ControllerSetDestination=143 Box1ControllerSetRatio=255 "
                 "Box1ControllerSetPolarity=0\"\n"));
    a.block(128, {param(0, 3, 1.0)});
    for (int key : {36, 60, 84, 96}) { a.block(128, {on(0, key, 0, int16_t(key))}); a.run(9600); a.block(128, {off(0, key, 0, int16_t(key))}); a.run(4800); }
    std::printf("  peak %.3f\n", peak(a.L));
    CHECK(peak(a.L) < 1.f && peak(a.R) < 1.f);
}
