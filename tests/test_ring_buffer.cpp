// Tests for the SPSC lock-free ring buffer, including a 2-thread stress test
// that is the M1 acceptance criterion (no data loss, in order, wait-free).
#include "aiudio/io/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

#include "test_framework.hpp"

using aiudio::io::RingBuffer;

AIUDIO_TEST(capacity_is_power_of_two_minus_one) {
    RingBuffer<int> rb(10);          // rounds storage to 16
    CHECK(rb.capacity() == 15);
    RingBuffer<int> rb2(16);         // rounds storage to 32 (one slot reserved)
    CHECK(rb2.capacity() == 31);
}

AIUDIO_TEST(push_pop_fifo_order) {
    RingBuffer<int> rb(8);
    for (int i = 0; i < 5; ++i) CHECK(rb.push(i));
    int v = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(rb.pop(v));
        CHECK(v == i);
    }
    CHECK(!rb.pop(v));  // now empty
}

AIUDIO_TEST(reports_full_without_overwriting) {
    RingBuffer<int> rb(3);  // capacity() == 3
    CHECK(rb.capacity() == 3);
    CHECK(rb.push(1));
    CHECK(rb.push(2));
    CHECK(rb.push(3));
    CHECK(!rb.push(4));     // full → refused
    int v = 0;
    CHECK(rb.pop(v) && v == 1);  // oldest still intact (no overwrite)
}

AIUDIO_TEST(bulk_write_read_roundtrip) {
    RingBuffer<float> rb(1024);
    float in[256];
    for (int i = 0; i < 256; ++i) in[i] = static_cast<float>(i);
    CHECK(rb.write(in, 256) == 256);
    CHECK(rb.sizeApprox() == 256);
    float out[256] = {};
    CHECK(rb.read(out, 256) == 256);
    bool ok = true;
    for (int i = 0; i < 256; ++i) ok = ok && (out[i] == static_cast<float>(i));
    CHECK(ok);
    CHECK(rb.empty());
}

// M9.1: failed pushes/pops are counted as over/underruns (xrun telemetry).
AIUDIO_TEST(xrun_counters) {
    RingBuffer<int> rb(3);  // capacity 3
    CHECK(rb.overrunCount() == 0);
    CHECK(rb.underrunCount() == 0);

    int v = 0;
    CHECK(!rb.pop(v));                 // empty → underrun
    CHECK(rb.underrunCount() == 1);

    for (int i = 0; i < 3; ++i) CHECK(rb.push(i));
    CHECK(!rb.push(99));               // full → overrun
    CHECK(!rb.push(98));               // full → overrun
    CHECK(rb.overrunCount() == 2);

    // Bulk over/underrun count the shortfall in elements.
    RingBuffer<float> rb2(3);          // capacity 3
    float in[5] = {0, 1, 2, 3, 4};
    CHECK(rb2.write(in, 5) == 3);      // wrote 3, dropped 2
    CHECK(rb2.overrunCount() == 2);
    float out[5] = {};
    CHECK(rb2.read(out, 5) == 3);      // read 3, missing 2
    CHECK(rb2.underrunCount() == 2);
}

// The acceptance test: one producer, one consumer, a million items, small buffer.
AIUDIO_TEST(spsc_stress_no_loss_in_order) {
    constexpr std::uint32_t kN = 1'000'000;
    RingBuffer<std::uint32_t> rb(1024);

    std::thread producer([&] {
        std::uint32_t i = 0;
        while (i < kN) {
            if (rb.push(i)) ++i;  // spin while full
        }
    });

    std::uint32_t expected = 0;
    std::uint32_t value = 0;
    bool orderOk = true;
    while (expected < kN) {
        if (rb.pop(value)) {
            if (value != expected) orderOk = false;
            ++expected;
        }
    }
    producer.join();

    CHECK(orderOk);
    CHECK(expected == kN);
    CHECK(rb.empty());
}

AIUDIO_TEST_MAIN()
