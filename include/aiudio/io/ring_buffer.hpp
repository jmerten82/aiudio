// aiudio-io — single-producer / single-consumer wait-free ring buffer.
//
// THE mechanism for crossing a thread boundary in aiudio (ADR-0004): process
// taps, Python consumers, off-thread inference, recorders. Memory is allocated
// once in the constructor; push()/pop()/write()/read() are wait-free and
// allocation-free. Correct under acquire/release ordering for exactly one
// producer thread and one consumer thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace aiudio::io {

template <class T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "RingBuffer<T> requires a trivially copyable T for RT safety");

public:
    /// Construct a buffer that can hold at least `minCapacity` elements. The
    /// internal storage is rounded up to a power of two; one slot is reserved to
    /// distinguish full from empty, so usable capacity() == rounded - 1.
    explicit RingBuffer(std::size_t minCapacity)
        : capacity_(roundUpPow2(minCapacity + 1)),
          mask_(capacity_ - 1),
          data_(std::make_unique<T[]>(capacity_)) {}

    /// Maximum number of elements the buffer can hold.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_ - 1; }

    /// Producer: push one element. Returns false if full (no overwrite). A failed push
    /// is an **overrun** — counted (see overrunCount()).
    bool push(const T& value) noexcept {
        const std::size_t w = write_.index.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        if (next == read_.index.load(std::memory_order_acquire)) {  // full → drop
            write_.xruns.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        data_[w] = value;
        write_.index.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer: pop one element into `out`. Returns false if empty. A failed pop is an
    /// **underrun** — counted (see underrunCount()).
    bool pop(T& out) noexcept {
        const std::size_t r = read_.index.load(std::memory_order_relaxed);
        if (r == write_.index.load(std::memory_order_acquire)) {  // empty
            read_.xruns.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        out = data_[r];
        read_.index.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

    /// Producer: bulk push. Returns the number written (< count if it fills up); the
    /// shortfall is counted as an overrun.
    std::size_t write(const T* src, std::size_t count) noexcept {
        std::size_t w = write_.index.load(std::memory_order_relaxed);
        const std::size_t r = read_.index.load(std::memory_order_acquire);
        std::size_t n = 0;
        while (n < count) {
            const std::size_t next = (w + 1) & mask_;
            if (next == r) break;  // full
            data_[w] = src[n++];
            w = next;
        }
        write_.index.store(w, std::memory_order_release);
        if (n < count) write_.xruns.fetch_add(count - n, std::memory_order_relaxed);
        return n;
    }

    /// Consumer: bulk read. Returns the number read (< count if it empties); the
    /// shortfall is counted as an underrun.
    std::size_t read(T* dst, std::size_t count) noexcept {
        std::size_t r = read_.index.load(std::memory_order_relaxed);
        const std::size_t w = write_.index.load(std::memory_order_acquire);
        std::size_t n = 0;
        while (n < count) {
            if (r == w) break;  // empty
            dst[n++] = data_[r];
            r = (r + 1) & mask_;
        }
        read_.index.store(r, std::memory_order_release);
        if (n < count) read_.xruns.fetch_add(count - n, std::memory_order_relaxed);
        return n;
    }

    /// Elements the producer could not write because the buffer was full (xrun telemetry,
    /// off-thread readable). Written only by the producer.
    [[nodiscard]] std::uint64_t overrunCount() const noexcept {
        return write_.xruns.load(std::memory_order_relaxed);
    }
    /// Elements the consumer could not read because the buffer was empty (xrun telemetry).
    /// Written only by the consumer.
    [[nodiscard]] std::uint64_t underrunCount() const noexcept {
        return read_.xruns.load(std::memory_order_relaxed);
    }

    /// Approximate number of elements available to read (may be stale).
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t w = write_.index.load(std::memory_order_acquire);
        const std::size_t r = read_.index.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

    [[nodiscard]] bool empty() const noexcept { return sizeApprox() == 0; }

private:
    static std::size_t roundUpPow2(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // Keep the producer and consumer cursors on separate cache lines to avoid
    // false sharing between the two threads.
#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t kCacheLine = 64;
#endif
    struct alignas(kCacheLine) Cursor {
        std::atomic<std::size_t> index{0};
        std::atomic<std::uint64_t> xruns{0};  // producer: overruns; consumer: underruns
    };

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> data_;
    Cursor write_;
    Cursor read_;
};

}  // namespace aiudio::io
