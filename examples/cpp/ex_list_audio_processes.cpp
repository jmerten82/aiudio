// Example: list the processes Core Audio knows about, so you can pick a PID to
// tap with ex_tap_capture. No capture, no permission required. macOS 14.4+.
//
//   ./ex_list_audio_processes

#include <cstdio>

#ifdef __APPLE__
#include "aiudio/io/coreaudio_process_tap_backend.hpp"

using aiudio::io::CoreAudioProcessTapBackend;

int main() {
    const auto procs = CoreAudioProcessTapBackend::listProcesses();
    std::printf("%zu audio process object(s):\n", procs.size());
    std::printf("  %-8s  %s\n", "PID", "bundle id");
    for (const auto& p : procs) {
        std::printf("  %-8d  %s\n", p.pid, p.bundleId.empty() ? "(none)" : p.bundleId.c_str());
    }
    std::printf("\nTap one with:  ./ex_tap_capture --pid <PID>\n");
    std::printf("Tap everything: ./ex_tap_capture --system\n");
    return procs.empty() ? 1 : 0;
}
#else
int main() { std::printf("Core Audio process taps are macOS-only.\n"); return 0; }
#endif
