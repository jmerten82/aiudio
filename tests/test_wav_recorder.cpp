// Tests for WavRecorder: record a planar block stream to a WAV off the audio thread. The
// producer (pushBlock) is the RT side; an internal writer thread drains a lock-free ring to
// disk. Covers a basic round-trip (pushed values land in the file), stop/drain completeness,
// and a multithreaded producer+writer run (race-free under TSan).
#include "aiudio/io/wav_recorder.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/wav_file.hpp"
#include "test_framework.hpp"

using namespace aiudio::io;

namespace {
constexpr double kSr = 48000.0;

std::string tmpWav(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// A planar block of `channels` rows, each filled with value[c].
struct Block {
    std::vector<std::vector<float>> store;
    std::vector<float*> ptrs;
    AudioBuffer buf;
    Block(std::uint32_t channels, std::uint32_t frames, const std::vector<float>& vals) {
        store.resize(channels);
        ptrs.resize(channels);
        for (std::uint32_t c = 0; c < channels; ++c) {
            store[c].assign(frames, vals[c % vals.size()]);
            ptrs[c] = store[c].data();
        }
        buf = AudioBuffer{ptrs.data(), channels, frames};
    }
};

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
}  // namespace

// Push N constant blocks, stop, read the WAV back: frame count + values must match.
AIUDIO_TEST(records_pushed_blocks_to_wav) {
    const std::string path = tmpWav("aiudio_rec_roundtrip.wav");
    const std::uint32_t ch = 2, frames = 128, blocks = 50;
    {
        WavRecorder rec;
        REQUIRE(rec.start(path, ch, kSr, WavFormat::Float32, /*ringFrames*/ 16384, /*maxBlock*/ frames));
        REQUIRE(rec.recording());
        Block b(ch, frames, {0.5f, -0.25f});
        for (std::uint32_t i = 0; i < blocks; ++i) rec.pushBlock(b.buf, frames);
        rec.stop();  // final drain + finalize
        CHECK(rec.framesWritten() == static_cast<std::uint64_t>(frames) * blocks);
        CHECK(rec.droppedFrames() == 0);
    }
    WavReader r(path);
    REQUIRE(r.ok());
    CHECK(r.channels() == ch);
    CHECK(r.totalFrames() == static_cast<std::uint64_t>(frames) * blocks);
    std::vector<float> c0(frames * blocks, 0.0f), c1(frames * blocks, 0.0f);
    float* planar[2] = {c0.data(), c1.data()};
    const std::uint32_t got = r.read(planar, ch, frames * blocks);
    CHECK(got == frames * blocks);
    CHECK(near(c0.front(), 0.5f) && near(c0.back(), 0.5f));
    CHECK(near(c1.front(), -0.25f) && near(c1.back(), -0.25f));
    std::filesystem::remove(path);
}

// stop() must drain everything already pushed, even a burst just before stopping.
AIUDIO_TEST(stop_drains_all_pending_frames) {
    const std::string path = tmpWav("aiudio_rec_drain.wav");
    const std::uint32_t ch = 1, frames = 256, blocks = 200;
    WavRecorder rec;
    REQUIRE(rec.start(path, ch, kSr, WavFormat::Float32, 1 << 16, frames));
    Block b(ch, frames, {0.1f});
    for (std::uint32_t i = 0; i < blocks; ++i) rec.pushBlock(b.buf, frames);
    rec.stop();
    CHECK(rec.framesWritten() == static_cast<std::uint64_t>(frames) * blocks);
    WavReader r(path);
    REQUIRE(r.ok());
    CHECK(r.totalFrames() == static_cast<std::uint64_t>(frames) * blocks);
    std::filesystem::remove(path);
}

// A producer thread (the RT side) pushing while the internal writer thread drains — race-free
// under TSan (single producer / single consumer on the ring).
AIUDIO_TEST(multithreaded_record_is_race_free) {
    const std::string path = tmpWav("aiudio_rec_mt.wav");
    const std::uint32_t ch = 2, frames = 64;
    WavRecorder rec;
    REQUIRE(rec.start(path, ch, kSr, WavFormat::Float32, 1 << 15, frames));

    std::atomic<bool> stop{false};
    std::atomic<long> pushed{0};
    Block b(ch, frames, {0.3f, 0.3f});
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            rec.pushBlock(b.buf, frames);
            pushed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (pushed.load(std::memory_order_relaxed) < 3000) { /* let both threads run */ }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    rec.stop();

    CHECK(rec.framesWritten() > 0);
    WavReader r(path);
    CHECK(r.ok());
    std::filesystem::remove(path);
}

AIUDIO_TEST_MAIN()
