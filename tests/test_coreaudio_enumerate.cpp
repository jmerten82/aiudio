// Test: the Core Audio backend enumerates devices and finds a default output.
// Runs without opening a stream or playing audio (no permissions needed), so it
// is safe in CI / headless. macOS-only (guarded; only built on Apple by CMake).
#include <cstdio>

#include "test_framework.hpp"

#ifdef __APPLE__
#include "aiudio/io/coreaudio_backend.hpp"

using aiudio::io::CoreAudioBackend;

AIUDIO_TEST(enumerate_lists_devices_with_a_default_output) {
    CoreAudioBackend backend;
    const auto devices = backend.enumerate();

    std::printf("  enumerated %zu Core Audio device(s):\n", devices.size());
    bool hasDefaultOutput = false;
    for (const auto& d : devices) {
        std::printf("    %-30s in=%u out=%u%s%s\n", d.name.c_str(), d.inputChannels,
                    d.outputChannels, d.isDefaultOutput ? "  [default out]" : "",
                    d.isDefaultInput ? "  [default in]" : "");
        if (d.isDefaultOutput) hasDefaultOutput = true;
    }

    CHECK(!devices.empty());     // at least one device present
    CHECK(hasDefaultOutput);     // a system default output exists
}
#else
AIUDIO_TEST(coreaudio_is_macos_only) { /* nothing to test off-Apple */ }
#endif

AIUDIO_TEST_MAIN()
