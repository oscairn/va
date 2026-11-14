// SPDX-License-Identifier: AGPL-3.0-or-later
// Runs the tests against a built plugin: va_tests <plugin.clap> [name filter]. The tests reach the
// plugin only through the CLAP ABI.
#include <cstring>
#include "check.h"
#include "host.h"

std::vector<TestCase>& testCases() { static std::vector<TestCase> v; return v; }
int g_failures = 0;
const char* g_pluginPath = nullptr;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: va_tests <plugin.clap> [filter]\n"); return 2; }
    g_pluginPath = argv[1];
    int run = 0;
    for (auto& t : testCases()) {
        if (argc > 2 && !std::strstr(t.name, argv[2])) continue;
        const int before = g_failures;
        t.fn();
        ++run;
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d tests, %d failed checks (allocation interposer: %s)\n", run, g_failures, allocGuardName());
    return g_failures ? 1 : 0;
}
