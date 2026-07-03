// Example: render a RenderCallback offline to a playable WAV file.
//
// Ties M1 together end-to-end: a SineSource node, the OfflineDriver "manual pump"
// (the ADR-0005 offline clock), the conversion helpers, and a tiny WAV writer.
// This is the shape of the future offline backend (docs/pipeline/71 M6).
//
// Run: ./ex_offline_render_wav [out.wav]   then:  afplay out.wav

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "example_support.hpp"

using namespace aiudio;

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "out.wav";
    constexpr double kSampleRate = 48'000.0;
    constexpr std::uint32_t kBlock = 128;
    constexpr std::uint32_t kChannels = 2;
    constexpr double kSeconds = 2.0;

    examples::OfflineDriver driver(/*in*/ 0, kChannels, kBlock, kSampleRate);
    examples::SineSource sine(220.0, kSampleRate, 0.3f);
    examples::WavWriter wav(path, static_cast<std::uint32_t>(kSampleRate),
                            static_cast<std::uint16_t>(kChannels));

    // Per-block scratch buffers, allocated once (the render loop allocates nothing).
    std::vector<float> interleaved(kChannels * kBlock);
    std::vector<std::int16_t> pcm(kChannels * kBlock);

    const int totalBlocks = static_cast<int>(kSeconds * kSampleRate / kBlock);
    std::uint64_t sampleTime = 0;
    for (int i = 0; i < totalBlocks; ++i) {
        const io::AudioBuffer out = driver.renderBlock(sine, sampleTime);
        io::interleave(out.channels, interleaved.data(), kChannels, kBlock);
        io::floatToInt16(interleaved.data(), pcm.data(), pcm.size());
        wav.writeInterleavedInt16(pcm.data(), pcm.size());
        sampleTime += kBlock;
    }
    wav.finalize();

    std::printf("wrote %.1f s of 220 Hz stereo to %s  (play: afplay %s)\n", kSeconds,
                path.c_str(), path.c_str());
    return 0;
}
