// tests/TestLockFree.cpp
#include "TestFramework.h"
#include "mixing/LockFreeQueue.h"

#include <atomic>
#include <numeric>
#include <thread>

using namespace sa;

SA_TEST(SpscQueue_PushPopOrder)
{
    SpscQueue<int> q(8);
    for (int i = 0; i < 8; ++i)
        SA_CHECK(q.TryPush(i));
    SA_CHECK(!q.TryPush(99));
    for (int i = 0; i < 8; ++i) {
        int v = -1;
        SA_CHECK(q.TryPop(v));
        SA_CHECK_EQ(v, i);
    }
    int v;
    SA_CHECK(!q.TryPop(v));
}

SA_TEST(SpscQueue_ThreadedTransfer)
{
    constexpr int kCount = 200000;
    SpscQueue<int> q(64);
    std::atomic<bool> done{false};
    long long sum = 0;
    std::thread consumer([&] {
        int v;
        int got = 0;
        while (got < kCount) {
            if (q.TryPop(v)) {
                sum += v;
                ++got;
            } else {
                std::this_thread::yield();
            }
        }
        done.store(true);
    });
    for (int i = 0; i < kCount; ++i)
        while (!q.TryPush(i))
            std::this_thread::yield();
    consumer.join();
    SA_CHECK(done.load());
    SA_CHECK_EQ(sum, static_cast<long long>(kCount) * (kCount - 1) / 2);
}

SA_TEST(SpscSampleRing_WrapsAndCounts)
{
    SpscSampleRing ring(16);
    float in[10];
    std::iota(in, in + 10, 1.f);
    SA_CHECK_EQ(ring.Write(in, 10), size_t(10));
    float out[6];
    SA_CHECK_EQ(ring.Read(out, 6), size_t(6));
    SA_CHECK_NEAR(out[5], 6.f, 1e-6);
    SA_CHECK_EQ(ring.Write(in, 10), size_t(10)); // wraps around
    SA_CHECK_EQ(ring.ReadableCount(), size_t(14));
    float rest[14];
    SA_CHECK_EQ(ring.Read(rest, 14), size_t(14));
    SA_CHECK_NEAR(rest[0], 7.f, 1e-6);
    SA_CHECK_NEAR(rest[4], 1.f, 1e-6);
    SA_CHECK_NEAR(rest[13], 10.f, 1e-6);
    SA_CHECK_EQ(ring.ReadableCount(), size_t(0));
}

SA_TEST(TimedSampleRing_AccumulatesAndReadsSilenceBeyondFrontier)
{
    TimedSampleRing ring(8192 * 2);
    ring.Reset(1000);
    float block[4] = {1.f, 2.f, 3.f, 4.f};
    SA_CHECK(ring.Accumulate(1000, block, 4));
    SA_CHECK(ring.Accumulate(1002, block, 4)); // overlaps two samples
    float out[8];
    const size_t avail = ring.ReadAt(1000, out, 8);
    SA_CHECK_EQ(avail, size_t(6));
    SA_CHECK_NEAR(out[0], 1.f, 1e-6);
    SA_CHECK_NEAR(out[2], 4.f, 1e-6); // 3 + 1
    SA_CHECK_NEAR(out[3], 6.f, 1e-6); // 4 + 2
    SA_CHECK_NEAR(out[5], 4.f, 1e-6);
    SA_CHECK_NEAR(out[6], 0.f, 1e-6);
}

SA_TEST(TimedSampleRing_WraparoundClearsStaleSamples)
{
    TimedSampleRing ring(8192 * 2);
    const size_t cap = ring.Capacity();
    ring.Reset(0);
    std::vector<float> ones(256, 1.f);
    uint64_t clock = 0;
    // Drive producer and consumer through several full cycles of the ring.
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (size_t i = 0; i < cap; i += 256) {
            SA_CHECK(ring.Accumulate(clock, ones.data(), 256));
            std::vector<float> out(256);
            SA_CHECK_EQ(ring.ReadAt(clock, out.data(), 256), size_t(256));
            for (float v : out)
                SA_CHECK_NEAR(v, 1.f, 1e-6); // never 2.0: old cycle data was cleared
            clock += 256;
        }
    }
    // A gap (silence) followed by a write must read back as zeros then data.
    clock += 1024;
    SA_CHECK(ring.Accumulate(clock, ones.data(), 256));
    std::vector<float> gap(1024);
    SA_CHECK_EQ(ring.ReadAt(clock - 1024, gap.data(), 1024), size_t(1024));
    for (float v : gap)
        SA_CHECK_NEAR(v, 0.f, 1e-6);
}

SA_TEST(TimedSampleRing_LateDataIsDropped)
{
    TimedSampleRing ring(8192 * 2);
    ring.Reset(0);
    float out[64];
    ring.ReadAt(0, out, 64); // cursor now 64
    float block[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    SA_CHECK(!ring.Accumulate(0, block, 8));   // entirely late
    SA_CHECK(ring.Accumulate(60, block, 8));   // partially late: keeps [64,68)
    const size_t avail = ring.ReadAt(64, out, 8);
    SA_CHECK_EQ(avail, size_t(4));
    SA_CHECK_NEAR(out[0], 1.f, 1e-6);
    SA_CHECK_NEAR(out[3], 1.f, 1e-6);
    SA_CHECK_NEAR(out[4], 0.f, 1e-6);
}

SA_TEST(TimedSampleRing_FarFutureIsClamped)
{
    TimedSampleRing ring(8192 * 2);
    const size_t cap = ring.Capacity();
    ring.Reset(0);
    float block[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    SA_CHECK(!ring.Accumulate(cap * 2, block, 8));
    SA_CHECK(ring.WrittenUpTo() == 0);
}

SA_TEST(SeqLock_ConcurrentSnapshotsStayCoherent)
{
    struct Snapshot { uint64_t words[32]{}; };
    SeqLock<Snapshot> value;
    std::atomic<bool> done{false};
    std::atomic<bool> coherent{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            do {
                const Snapshot s = value.Load();
                for (uint64_t word : s.words)
                    if (word != s.words[0])
                        coherent.store(false);
            } while (!done.load());
        });
    }
    for (uint64_t n = 1; n <= 50000; ++n) {
        Snapshot s;
        for (uint64_t& word : s.words)
            word = n;
        value.Store(s);
    }
    done.store(true);
    for (auto& thread : readers)
        thread.join();
    SA_CHECK(coherent.load());
    SA_CHECK_EQ(value.Load().words[0], uint64_t(50000));
}

SA_TEST(SeqLock_RoundTrip)
{
    struct P {
        float a;
        int b;
    };
    SeqLock<P> lock(P{1.f, 2});
    P v = lock.Load();
    SA_CHECK_NEAR(v.a, 1.f, 1e-6);
    lock.Store(P{3.f, 4});
    v = lock.Load();
    SA_CHECK_EQ(v.b, 4);
}
