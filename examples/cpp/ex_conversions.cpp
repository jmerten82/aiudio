// Example: the device <-> engine sample-format boundary.
//
// Devices/hosts deliver interleaved, often-integer samples; the engine works in
// planar float32. This walks the exact round trip a Core Audio backend (M2/M3)
// performs each block:
//
//   device int16 interleaved
//     -> int16ToFloat   (float interleaved)
//     -> deinterleave   (planar float = engine buffers)   <-- run nodes here
//     -> interleave     (float interleaved)
//     -> floatToInt16   (device int16 interleaved)
//
// Run: ./ex_conversions     (prints max round-trip error, exits 0 on success)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "aiudio/io/conversions.hpp"

namespace io = aiudio::io;

int main() {
    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint32_t kFrames = 256;

    // 1) A device hands us an interleaved int16 stereo block: L = sine, R = quieter.
    std::vector<std::int16_t> deviceIn(kChannels * kFrames);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        const double t = 2.0 * 3.14159265358979 * 440.0 * f / 48'000.0;
        deviceIn[f * kChannels + 0] = static_cast<std::int16_t>(std::sin(t) * 30000.0);
        deviceIn[f * kChannels + 1] = static_cast<std::int16_t>(std::sin(t) * 12000.0);
    }

    // 2) int16 -> float (interleaved), then deinterleave to planar engine buffers.
    std::vector<float> interleavedF(kChannels * kFrames);
    io::int16ToFloat(deviceIn.data(), interleavedF.data(), interleavedF.size());

    std::vector<float> chL(kFrames), chR(kFrames);
    float* planar[kChannels] = {chL.data(), chR.data()};
    io::deinterleave(interleavedF.data(), planar, kChannels, kFrames);

    // (A node graph would process the planar buffers here. We pass them through.)

    // 3) planar -> interleaved float -> int16 back out to the "device".
    std::vector<float> interleavedOut(kChannels * kFrames);
    io::interleave(planar, interleavedOut.data(), kChannels, kFrames);

    std::vector<std::int16_t> deviceOut(kChannels * kFrames);
    io::floatToInt16(interleavedOut.data(), deviceOut.data(), deviceOut.size());

    // 4) Verify the round trip is within int16 quantization (±1 LSB).
    int maxErr = 0;
    for (std::size_t i = 0; i < deviceIn.size(); ++i) {
        maxErr = std::max(maxErr, std::abs(int(deviceIn[i]) - int(deviceOut[i])));
    }
    const bool ok = maxErr <= 1;
    std::printf("round-trip max error = %d LSB (<= 1) %s\n", maxErr, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
