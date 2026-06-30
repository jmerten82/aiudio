// macOS-only test for the Core Audio device-died listener wiring (M9.4 hardware path).
// It opens the default output device, which registers the kAudioDevicePropertyDeviceIsAlive
// listener, and exercises the listener-callback logic deterministically: a LIVE device must
// NOT fire the disconnect handler. The actual device-death trigger (a physical unplug) is
// hardware-verified — not reproducible headlessly — so this proves the wiring runs cleanly
// (register on open, the alive-check, no spurious disconnect, remove on destruct) without
// requiring an unplug. Skips cleanly where there is no output device (e.g. a CI runner).
#include <atomic>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"
#include "test_framework.hpp"

// Note: <CoreAudio/CoreAudio.h> (pulled in by coreaudio_backend.hpp) declares a struct
// `AudioBuffer` too, so qualify ours as aiudio::io::AudioBuffer rather than `using`-importing it.
namespace io = aiudio::io;

namespace {
struct SilentCallback final : io::RenderCallback {
    void process(const io::AudioBuffer&, io::AudioBuffer& out, std::uint32_t frames,
                 const io::TimeInfo&) noexcept override {
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* d = out.channel(c);
            for (std::uint32_t f = 0; f < frames; ++f) d[f] = 0.0f;
        }
    }
};
}  // namespace

// Opening the default output device registers the alive listener; a live device does not
// trigger a disconnect, and tearing down removes the listener cleanly.
AIUDIO_TEST(alive_listener_registers_without_spurious_disconnect) {
    io::CoreAudioBackend be;
    auto devices = be.enumerate();
    bool hasOutput = false;
    for (const auto& d : devices)
        if (d.outputChannels > 0) hasOutput = true;
    if (!hasOutput) {
        std::printf("    (no output device — skipping live HAL listener test)\n");
        return;  // CI runner without audio — nothing to register against
    }

    std::atomic<int> disconnects{0};
    be.setDisconnectHandler([&] { disconnects.fetch_add(1, std::memory_order_relaxed); });

    io::StreamConfig cfg;
    cfg.outputChannels = 2;
    cfg.blockSize = 256;
    SilentCallback cb;
    if (!be.open(cfg, &cb)) {
        std::printf("    (open() failed — skipping; no usable default output)\n");
        return;
    }

    CHECK(!be.disconnected());  // a freshly opened, live device is connected

    // Drive the listener callback directly: the device is alive, so it must NOT disconnect.
    be.handleDeviceAliveChanged();
    CHECK(!be.disconnected());
    CHECK(disconnects.load() == 0);

    be.stop();  // and the destructor removes the listener — no crash
}

AIUDIO_TEST_MAIN()
