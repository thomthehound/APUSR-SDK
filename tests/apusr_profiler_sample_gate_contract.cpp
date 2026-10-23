// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_profiler.h>

#include <cstdlib>
#include <iostream>

namespace {

void setEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main()
{
#if !APUSR_ENABLE_PROFILER
    return 0;
#else
    setEnvironment("APUSR_PROFILE", "sampled");
    setEnvironment("APUSR_PROFILE_CPU", "1");
    setEnvironment("APUSR_PROFILE_GPU", "0");
    setEnvironment("APUSR_PROFILE_INTERVAL_MS", "600000");
    setEnvironment("APUSR_PROFILE_BURST", "3");

    apusr::profile::SampleGate gate;

    // Two logical contexts are first observed, then alternate without changing.
    // The first observation of B may legitimately refresh the burst once. After
    // both contexts are known, ordinary A/B alternation must not keep sampled
    // mode permanently open.
    const auto a0 = gate.decideStable(1u, true);
    const auto b0 = gate.decideStable(2u, true);
    const auto b1 = gate.decideStable(3u, false);
    const auto a1 = gate.decideStable(4u, false);
    const auto b2 = gate.decideStable(5u, false);
    const auto a2 = gate.decideStable(6u, false);

    if (!a0.capture || !a0.configurationChanged ||
        !b0.capture || !b0.configurationChanged ||
        !b1.capture || b1.configurationChanged ||
        !a1.capture || a1.configurationChanged) {
        std::cerr << "sampled burst/configuration semantics are incorrect\n";
        return 1;
    }
    if (b2.capture || a2.capture) {
        std::cerr << "stable alternating contexts kept sampled mode continuously open\n";
        return 2;
    }
    if (b0.burstSequence == a0.burstSequence || b1.burstSequence != b0.burstSequence ||
        a1.burstSequence != b0.burstSequence) {
        std::cerr << "configuration refresh did not produce one coherent replacement burst\n";
        return 3;
    }
    return 0;
#endif
}
