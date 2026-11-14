// SPDX-License-Identifier: AGPL-3.0-or-later
// The plugin's identity: the one place its name, id and version are written in source. The
// descriptor is a literal because tools/release/version.py reads the version out of it.
// (Also named in CMakeLists.txt, VA_NAME, and tools/release/release.conf.)
#pragma once
#include <clap/clap.h>

namespace va {
inline const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                        CLAP_PLUGIN_FEATURE_STEREO, nullptr};
inline const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION, "org.oscairn.va", "Oscairn VA", "Oscairn", "https://github.com/oscairn/va",
    "", "", "0.11.0", "Virtual analog instrument", kFeatures};

// Device voice caps, read when an instance is created (see plugin.cpp).
inline constexpr const char* kEnvMaxVoices = "OSCAIRN_VA_MAX_VOICES";               // 1..128
inline constexpr const char* kEnvMaxVoicesPerSound = "OSCAIRN_VA_MAX_VOICES_PER_SOUND"; // 1..64
}  // namespace va
