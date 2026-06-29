// Example: cross a thread boundary with the lock-free SPSC RingBuffer.
//
// This is THE pattern for getting audio between threads in aiudio (ADR-0004):
// e.g. a Core Audio process-tap thread (producer) handing blocks to the engine
// (consumer). One producer thread + one consumer thread, no locks, no loss.
//
// Run: ./ex_ring_buffer        (prints OK / FAIL, exits 0 on success)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "aiudio/io/ring_buffer.hpp"

using aiudio::io::RingBuffer;

namespace {
constexpr std::size_t kBlock = 128;
constexpr std::size_t kTotalFrames = 480'000;  // ~10 s @ 48 kHz
constexpr std::size_t kModulo = 4096;          // values stay exactly representable

// Deterministic test signal so the consumer can verify integrity across threads.
float expectedSample(std::size_t i) { return static_cast<float>(i % kModulo); }
}  // namespace

int main() {
    // Buffer holds ~16 blocks — much smaller than the data, so it fills and drains
    // repeatedly, exercising the full/empty edges.
    RingBuffer<float> ring(kBlock * 16);
    std::printf("RingBuffer capacity = %zu frames\n", ring.capacity());

    // Producer: write the signal in blocks, spinning while the buffer is full.
    std::thread producer([&] {
        float block[kBlock];
        std::size_t produced = 0;
        while (produced < kTotalFrames) {
            const std::size_t n = std::min(kBlock, kTotalFrames - produced);
            for (std::size_t k = 0; k < n; ++k) block[k] = expectedSample(produced + k);
            std::size_t off = 0;
            while (off < n) off += ring.write(block + off, n - off);  // wait-free retry
            produced += n;
        }
    });

    // Consumer (this thread): read in blocks and verify order + integrity.
    float block[kBlock];
    std::size_t consumed = 0;
    bool ok = true;
    while (consumed < kTotalFrames) {
        const std::size_t got = ring.read(block, kBlock);
        for (std::size_t k = 0; k < got; ++k) {
            if (block[k] != expectedSample(consumed + k)) ok = false;
        }
        consumed += got;
    }
    producer.join();

    std::printf("moved %zu frames across the thread boundary, integrity %s\n",
                consumed, ok ? "OK" : "FAIL");
    return (ok && consumed == kTotalFrames) ? 0 : 1;
}
