// src/mixing/LockFreeQueue.h
//
// Lock-free primitives connecting the game thread, the engine mixer hook
// (game thread), the BASS DSP thread, the simulation thread and the audio
// thread.
//
//   SpscQueue<T>        single-producer/single-consumer bounded FIFO of
//                        movable objects (commands, shared_ptrs, ...).
//   SpscSampleRing      single-producer/single-consumer float sample FIFO
//                        with bulk read/write, used for streamed PCM
//                        (BASS channels, Lua procedural streams).
//   TimedSampleRing     single-producer/single-consumer ring indexed by an
//                        absolute sample clock. The engine mixer writes each
//                        channel's block at (paintStart + outputOffset); the
//                        audio thread reads every source at the same clock
//                        position so the relative timing between engine
//                        channels is preserved exactly.
//   SeqLock<T>          single-writer/multi-reader publication of a
//                        trivially-copyable snapshot (source parameters).
//
// Memory ordering: producers publish with release stores, consumers acquire.
// Indices are monotonically increasing 64-bit counters; capacities are powers
// of two so wrap-around is a mask.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace sa {

constexpr size_t kCacheLineSize = 64;

namespace detail {
inline size_t RoundUpPow2(size_t v)
{
    if (v < 2)
        return 2;
    --v;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        v |= v >> shift;
    return v + 1;
}
} // namespace detail

// ---------------------------------------------------------------------------
// SpscQueue<T>
// ---------------------------------------------------------------------------
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : m_capacity(detail::RoundUpPow2(capacity)), m_mask(m_capacity - 1),
          m_slots(static_cast<Slot*>(::operator new[](sizeof(Slot) * m_capacity, std::align_val_t{alignof(Slot)})))
    {
    }

    ~SpscQueue()
    {
        T tmp;
        while (TryPop(tmp)) {
        }
        ::operator delete[](m_slots, std::align_val_t{alignof(Slot)});
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    size_t Capacity() const { return m_capacity; }

    // Producer side. Returns false when the queue is full.
    template <typename U>
    bool TryPush(U&& value)
    {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        const uint64_t tail = m_tailCache;
        if (head - tail >= m_capacity) {
            m_tailCache = m_tail.load(std::memory_order_acquire);
            if (head - m_tailCache >= m_capacity)
                return false;
        }
        new (&m_slots[head & m_mask].storage) T(std::forward<U>(value));
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when the queue is empty.
    bool TryPop(T& out)
    {
        const uint64_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_headCache) {
            m_headCache = m_head.load(std::memory_order_acquire);
            if (tail == m_headCache)
                return false;
        }
        T* item = reinterpret_cast<T*>(&m_slots[tail & m_mask].storage);
        out = std::move(*item);
        item->~T();
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate (racy) size, for diagnostics only.
    size_t SizeApprox() const
    {
        const uint64_t head = m_head.load(std::memory_order_acquire);
        const uint64_t tail = m_tail.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    bool EmptyApprox() const { return SizeApprox() == 0; }

private:
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
    };

    const size_t m_capacity;
    const size_t m_mask;
    Slot* m_slots;

    alignas(kCacheLineSize) std::atomic<uint64_t> m_head{0}; // written by producer
    alignas(kCacheLineSize) uint64_t m_tailCache{0};           // producer-private
    alignas(kCacheLineSize) std::atomic<uint64_t> m_tail{0}; // written by consumer
    alignas(kCacheLineSize) uint64_t m_headCache{0};           // consumer-private
};

// ---------------------------------------------------------------------------
// SpscSampleRing
// ---------------------------------------------------------------------------
class SpscSampleRing {
public:
    explicit SpscSampleRing(size_t capacitySamples)
        : m_capacity(detail::RoundUpPow2(capacitySamples)), m_mask(m_capacity - 1), m_data(m_capacity, 0.f)
    {
    }

    SpscSampleRing(const SpscSampleRing&) = delete;
    SpscSampleRing& operator=(const SpscSampleRing&) = delete;

    size_t Capacity() const { return m_capacity; }

    // Producer: number of samples that can be written without overwriting.
    size_t WritableCount() const
    {
        const uint64_t w = m_write.load(std::memory_order_relaxed);
        const uint64_t r = m_read.load(std::memory_order_acquire);
        return m_capacity - static_cast<size_t>(w - r);
    }

    // Producer: writes up to `count` samples, returns how many were written.
    size_t Write(const float* samples, size_t count)
    {
        const uint64_t w = m_write.load(std::memory_order_relaxed);
        const uint64_t r = m_read.load(std::memory_order_acquire);
        const size_t writable = m_capacity - static_cast<size_t>(w - r);
        const size_t n = std::min(count, writable);
        if (n == 0)
            return 0;
        const size_t start = static_cast<size_t>(w & m_mask);
        const size_t first = std::min(n, m_capacity - start);
        std::memcpy(&m_data[start], samples, first * sizeof(float));
        if (n > first)
            std::memcpy(&m_data[0], samples + first, (n - first) * sizeof(float));
        m_write.store(w + n, std::memory_order_release);
        return n;
    }

    // Producer: writes `count` zero samples.
    size_t WriteSilence(size_t count)
    {
        const uint64_t w = m_write.load(std::memory_order_relaxed);
        const uint64_t r = m_read.load(std::memory_order_acquire);
        const size_t writable = m_capacity - static_cast<size_t>(w - r);
        const size_t n = std::min(count, writable);
        if (n == 0)
            return 0;
        const size_t start = static_cast<size_t>(w & m_mask);
        const size_t first = std::min(n, m_capacity - start);
        std::memset(&m_data[start], 0, first * sizeof(float));
        if (n > first)
            std::memset(&m_data[0], 0, (n - first) * sizeof(float));
        m_write.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer: number of readable samples.
    size_t ReadableCount() const
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        const uint64_t w = m_write.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    // Consumer: reads up to `count` samples, returns how many were read.
    size_t Read(float* out, size_t count)
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        const uint64_t w = m_write.load(std::memory_order_acquire);
        const size_t readable = static_cast<size_t>(w - r);
        const size_t n = std::min(count, readable);
        if (n == 0)
            return 0;
        const size_t start = static_cast<size_t>(r & m_mask);
        const size_t first = std::min(n, m_capacity - start);
        std::memcpy(out, &m_data[start], first * sizeof(float));
        if (n > first)
            std::memcpy(out + first, &m_data[0], (n - first) * sizeof(float));
        m_read.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer: reads one sample without consuming; index 0 is the oldest.
    float Peek(size_t index) const
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        return m_data[static_cast<size_t>((r + index) & m_mask)];
    }

    // Consumer: discards up to `count` samples.
    size_t Skip(size_t count)
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        const uint64_t w = m_write.load(std::memory_order_acquire);
        const size_t n = std::min(count, static_cast<size_t>(w - r));
        m_read.store(r + n, std::memory_order_release);
        return n;
    }

    uint64_t TotalWritten() const { return m_write.load(std::memory_order_acquire); }
    uint64_t TotalRead() const { return m_read.load(std::memory_order_acquire); }

private:
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<float> m_data;
    alignas(kCacheLineSize) std::atomic<uint64_t> m_write{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> m_read{0};
};

// ---------------------------------------------------------------------------
// TimedSampleRing
// ---------------------------------------------------------------------------
//
// Samples are addressed by an absolute 64-bit sample clock shared by all
// engine channels. The producer (engine mixer hook) writes blocks at
// arbitrary non-decreasing clock positions; gaps are implicitly silent. The
// consumer reads [clock, clock + n) where clock is the audio thread's shared
// engine-clock cursor; positions not yet written read as silence.
//
// Invariant maintained by the producer: it never writes at a position lower
// than `m_readCursor` (published by the consumer). Capacity must exceed the
// maximum producer lead over the consumer (engine paints up to ~200 ms ahead;
// we allocate 2 seconds).
class TimedSampleRing {
public:
    explicit TimedSampleRing(size_t capacitySamples)
        : m_capacity(detail::RoundUpPow2(std::max(capacitySamples, kConsumerGuard * 2))), m_mask(m_capacity - 1),
          m_data(m_capacity, 0.f)
    {
    }

    TimedSampleRing(const TimedSampleRing&) = delete;
    TimedSampleRing& operator=(const TimedSampleRing&) = delete;

    size_t Capacity() const { return m_capacity; }

    // Producer: initialize the ring so that `startClock` maps to a clean
    // buffer. Must be called before the consumer starts reading this ring.
    void Reset(uint64_t startClock)
    {
        std::fill(m_data.begin(), m_data.end(), 0.f);
        m_clearedUpTo = startClock;
        m_readCursor.store(startClock, std::memory_order_relaxed);
        m_writtenUpTo.store(startClock, std::memory_order_release);
    }

    // Producer: accumulates (adds) `count` samples at absolute `clock`.
    // Returns false if the block is entirely outside the writable window.
    bool Accumulate(uint64_t clock, const float* samples, size_t count)
    {
        if (count == 0)
            return true;
        const uint64_t readCursor = m_readCursor.load(std::memory_order_acquire);
        const uint64_t written = m_writtenUpTo.load(std::memory_order_relaxed);

        // Too far in the future for the ring: clamp. The guard keeps the
        // producer off the block the consumer is currently copying out
        // (ReadAt publishes clock + count before it reads).
        const uint64_t maxEnd = readCursor + m_capacity - kConsumerGuard;
        uint64_t begin = clock;
        uint64_t end = clock + count;
        if (begin < readCursor) {
            // Late data: drop the part the consumer already passed.
            const uint64_t drop = readCursor - begin;
            if (drop >= count)
                return false;
            samples += drop;
            begin = readCursor;
        }
        if (end > maxEnd) {
            end = maxEnd;
            if (end <= begin)
                return false;
        }

        // Zero any region between the previously written frontier and `end`
        // that has not been cleared yet (samples that wrapped from an older
        // cycle of the ring).
        ClearRange(std::max(m_clearedUpTo, readCursor), end);

        for (uint64_t pos = begin; pos < end; ++pos)
            m_data[static_cast<size_t>(pos & m_mask)] += samples[pos - begin];

        if (end > written)
            m_writtenUpTo.store(end, std::memory_order_release);
        return true;
    }

    // Producer: marks the clock as having advanced to `clock` (block start)
    // so the consumer can distinguish "silence" from "not yet mixed".
    void AdvanceFrontier(uint64_t clock)
    {
        const uint64_t written = m_writtenUpTo.load(std::memory_order_relaxed);
        if (clock > written) {
            const uint64_t readCursor = m_readCursor.load(std::memory_order_acquire);
            const uint64_t clamped = std::min(clock, readCursor + m_capacity - kConsumerGuard);
            ClearRange(std::max(m_clearedUpTo, readCursor), clamped);
            m_writtenUpTo.store(clamped, std::memory_order_release);
        }
    }

    // Consumer: reads `count` samples starting at `clock`. Positions beyond
    // the written frontier are returned as zeros and counted as missing.
    // Returns the number of samples that were actually available.
    size_t ReadAt(uint64_t clock, float* out, size_t count)
    {
        // Publish the cursor first so the producer stops touching this range.
        m_readCursor.store(clock + count, std::memory_order_release);
        const uint64_t written = m_writtenUpTo.load(std::memory_order_acquire);
        size_t available = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t pos = clock + i;
            if (pos < written) {
                out[i] = m_data[static_cast<size_t>(pos & m_mask)];
                ++available;
            } else {
                out[i] = 0.f;
            }
        }
        return available;
    }

    uint64_t WrittenUpTo() const { return m_writtenUpTo.load(std::memory_order_acquire); }
    uint64_t ReadCursor() const { return m_readCursor.load(std::memory_order_acquire); }

private:
    static constexpr size_t kConsumerGuard = 4096; // >= largest single ReadAt block

    const size_t m_capacity;
    const size_t m_mask;
    std::vector<float> m_data;
    uint64_t m_clearedUpTo = 0; // producer-private
    alignas(kCacheLineSize) std::atomic<uint64_t> m_writtenUpTo{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> m_readCursor{0};

    void ClearRange(uint64_t from, uint64_t to)
    {
        if (to <= from)
            return;
        if (to - from >= m_capacity) {
            std::fill(m_data.begin(), m_data.end(), 0.f);
        } else {
            for (uint64_t pos = from; pos < to; ++pos)
                m_data[static_cast<size_t>(pos & m_mask)] = 0.f;
        }
        m_clearedUpTo = to;
    }
};

// ---------------------------------------------------------------------------
// SeqLock<T>
// ---------------------------------------------------------------------------
template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable<T>::value, "SeqLock requires trivially copyable T");

public:
    SeqLock() = default;
    explicit SeqLock(const T& initial) { Store(initial); }

    // Single writer.
    void Store(const T& value)
    {
        const uint32_t current = m_published.load(std::memory_order_seq_cst);
        for (;;) {
            for (uint32_t n = 1; n < m_slots.size(); ++n) {
                const uint32_t index = (current + n) % static_cast<uint32_t>(m_slots.size());
                Slot& slot = m_slots[index];
                uint32_t expected = 0;
                if (!slot.readers.compare_exchange_strong(expected, kWriting, std::memory_order_seq_cst))
                    continue;
                std::memcpy(&slot.value, &value, sizeof(T));
                slot.readers.fetch_and(~kWriting, std::memory_order_seq_cst);
                m_published.store(index, std::memory_order_seq_cst);
                return;
            }
            std::this_thread::yield();
        }
    }

    // Any number of readers; lock-free (retries while a write is in flight).
    T Load() const
    {
        for (;;) {
            const uint32_t index = m_published.load(std::memory_order_seq_cst);
            const Slot& slot = m_slots[index];
            const uint32_t previous = slot.readers.fetch_add(1, std::memory_order_seq_cst);
            if (!(previous & kWriting)) {
                T out;
                std::memcpy(&out, &slot.value, sizeof(T));
                slot.readers.fetch_sub(1, std::memory_order_seq_cst);
                return out;
            }
            slot.readers.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

private:
    static constexpr uint32_t kWriting = uint32_t(1) << 31;
    struct alignas(kCacheLineSize) Slot {
        mutable std::atomic<uint32_t> readers{0};
        T value{};
    };
    std::array<Slot, 8> m_slots;
    alignas(kCacheLineSize) std::atomic<uint32_t> m_published{0};
};

} // namespace sa
