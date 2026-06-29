// Example: implement RenderCallback "nodes" and drive them with no audio device.
//
// Shows the single duplex contract process(in, out, frames, time) (ADR-0005) and
// how a backend will call it. Here the "backend" is the OfflineDriver manual pump
// — the same contract a Core Audio backend will satisfy in M2+.
//
// Run: ./ex_render_callback     (prints measured values, exits 0 on success)

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "aiudio/io/audio_buffer.hpp"
#include "example_support.hpp"

using namespace aiudio;

namespace {
constexpr double kSampleRate = 48'000.0;
constexpr std::uint32_t kBlock = 128;

double blockSumSquares(const io::AudioBuffer& b, std::size_t& count) {
    double acc = 0.0;
    for (std::uint32_t c = 0; c < b.numChannels; ++c) {
        for (std::uint32_t f = 0; f < b.numFrames; ++f) {
            const float v = b.channel(c)[f];
            acc += static_cast<double>(v) * v;
        }
    }
    count += static_cast<std::size_t>(b.numChannels) * b.numFrames;
    return acc;
}
}  // namespace

int main() {
    bool ok = true;

    // 1) Output-only node: a 440 Hz sine, stereo, rendered for ~1 second.
    {
        examples::OfflineDriver driver(/*in*/ 0, /*out*/ 2, kBlock, kSampleRate);
        examples::SineSource sine(440.0, kSampleRate, 0.2f);

        double sumSq = 0.0;
        std::size_t n = 0;
        std::uint64_t sampleTime = 0;
        const int blocks = static_cast<int>(kSampleRate / kBlock);
        for (int i = 0; i < blocks; ++i) {
            const io::AudioBuffer out = driver.renderBlock(sine, sampleTime);
            sumSq += blockSumSquares(out, n);
            sampleTime += kBlock;
        }
        const double rms = std::sqrt(sumSq / static_cast<double>(n));
        const double expected = 0.2 / std::sqrt(2.0);
        const bool good = std::fabs(rms - expected) < 1e-3;
        ok = ok && good;
        std::printf("SineSource: output RMS = %.4f (expected %.4f) %s\n", rms, expected,
                    good ? "OK" : "FAIL");
    }

    // 2) Duplex node: GainNode scales a known input by 0.5.
    {
        examples::OfflineDriver driver(/*in*/ 1, /*out*/ 1, kBlock, kSampleRate);
        examples::GainNode gain(0.5f);

        // Inject a constant 1.0 input signal for this block.
        auto& in = driver.inputChannel(0);
        for (auto& s : in) s = 1.0f;

        const io::AudioBuffer out = driver.renderBlock(gain, 0);
        bool good = true;
        for (std::uint32_t f = 0; f < out.numFrames; ++f) {
            if (std::fabs(out.channel(0)[f] - 0.5f) > 1e-6f) good = false;
        }
        ok = ok && good;
        std::printf("GainNode:   in=1.0 * gain=0.5 -> out[0]=%.3f %s\n", out.channel(0)[0],
                    good ? "OK" : "FAIL");
    }

    std::printf("%s\n", ok ? "all checks passed" : "FAILURES");
    return ok ? 0 : 1;
}
