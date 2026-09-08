#include "TestFramework.h"
#include "steamaudio/PathCacheFile.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

using namespace sa;

namespace {
struct TemporaryCache {
    std::string path;
    TemporaryCache()
    {
        static std::atomic<uint32_t> id{0};
        path = (std::filesystem::temp_directory_path() /
            ("sa_path_pages_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(id.fetch_add(1)) + ".paths")).u8string();
    }
    ~TemporaryCache()
    {
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::u8path(path), ignored);
        std::filesystem::remove(std::filesystem::u8path(path + ".partial"), ignored);
    }
};

std::vector<PathCacheRecord> Row(uint32_t row)
{
    std::vector<PathCacheRecord> records;
    for (uint32_t j = 0; j < row; ++j) {
        if (j % 3 == 1) continue;
        PathCacheRecord record;
        record.target = static_cast<uint16_t>(j);
        record.flags = (j % 2 == 0) ? 1 : 0;
        if (!record.flags) {
            record.first = 0;
            record.last = static_cast<int16_t>(j);
            record.distance = static_cast<float>(row * 1.25 + j * 0.5);
            record.deviation = static_cast<float>(j * 0.03125);
        }
        records.push_back(record);
    }
    return records;
}
}

SA_TEST(PathCache_ResumesIncompleteFileAndKeepsEveryRoute)
{
    TemporaryCache file;
    std::string error;
    constexpr uint64_t budget = 65536;
    constexpr uint32_t count = 80;
    {
        PathCacheFile cache;
        SA_CHECK(cache.Begin(file.path, count, 12345, budget, error));
        for (uint32_t row = 0; row < count; row += 2) SA_CHECK(cache.WriteRow(row, Row(row), error));
        SA_CHECK(!cache.Finish(error));
        SA_CHECK(!std::filesystem::exists(std::filesystem::u8path(file.path)));
    }
    {
        std::ofstream tail(std::filesystem::u8path(file.path + ".partial"), std::ios::binary | std::ios::app);
        tail << "incomplete row";
    }
    PathCacheFile cache;
    SA_CHECK(cache.Begin(file.path, count, 12345, budget, error));
    SA_CHECK_EQ(cache.Stats().completedRows, count / 2);
    for (uint32_t row = 0; row < count; ++row) {
        SA_CHECK_EQ(cache.HasRow(row), row % 2 == 0);
        if (!cache.HasRow(row)) SA_CHECK(cache.WriteRow(row, Row(row), error));
    }
    SA_CHECK(cache.Finish(error));
    SA_CHECK(cache.Stats().complete);
    SA_CHECK(!std::filesystem::exists(std::filesystem::u8path(file.path + ".partial")));
    for (uint32_t row = 0; row < count; ++row) {
        for (const auto& expected : Row(row)) {
            PathCacheRecord actual;
            SA_CHECK(cache.Lookup(row, expected.target, actual, error));
            SA_CHECK(std::memcmp(&expected, &actual, sizeof(actual)) == 0);
        }
        PathCacheRecord missing;
        if (row > 1) SA_CHECK(!cache.Lookup(row, 1, missing, error));
        SA_CHECK(error.empty());
        SA_CHECK(cache.Stats().residentBytes <= budget);
    }
    cache.Close();
    SA_CHECK(cache.Open(file.path, count, 12345, budget, error));
    PathCacheRecord actual;
    SA_CHECK(cache.Lookup(79, 0, actual, error));
    const uint64_t reads = cache.Stats().pageReads;
    SA_CHECK(cache.Lookup(79, 0, actual, error));
    SA_CHECK_EQ(cache.Stats().pageReads, reads);
    SA_CHECK(!cache.Open(file.path, count, 54321, budget, error));
}

SA_TEST(PathCache_ParallelWriterAndBoundedReader)
{
    TemporaryCache file;
    PathCacheFile cache;
    std::string error;
    constexpr uint32_t count = 512;
    constexpr uint64_t budget = 65536;
    SA_CHECK(cache.Begin(file.path, count, 78, budget, error));
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        std::string writeError;
        for (uint32_t row = t; row < count; row += 4)
            if (!cache.WriteRow(row, Row(row), writeError)) ok.store(false);
    });
    for (auto& thread : threads) thread.join();
    SA_CHECK(ok.load());
    SA_CHECK_EQ(cache.Stats().completedRows, count);
    SA_CHECK(cache.Finish(error));
    SA_CHECK(cache.Stats().diskBytes < PathCacheFile::MaximumDiskBytes(count));
    for (uint32_t pass = 0; pass < 3; ++pass) {
        for (uint32_t row = count - 1; row > 0; --row) {
            PathCacheRecord actual;
            SA_CHECK(cache.Lookup(row, 0, actual, error));
            SA_CHECK_EQ(actual.flags, uint16_t(1));
            SA_CHECK(cache.Stats().residentBytes <= budget);
        }
    }
}

SA_TEST(PathCache_RejectsInvalidRowsAndCorruption)
{
    TemporaryCache file;
    PathCacheFile cache;
    std::string error;
    SA_CHECK(!cache.Begin(file.path, UINT32_MAX, 1, 65536, error));
    SA_CHECK(!cache.Begin(file.path, 256, 1, 1, error));
    SA_CHECK(cache.Begin(file.path, 2, 1, 65536, error));
    auto bad = Row(1);
    bad[0].target = 1;
    SA_CHECK(!cache.WriteRow(1, bad, error));
    bad[0].target = 0;
    bad[0].distance = std::numeric_limits<float>::quiet_NaN();
    SA_CHECK(!cache.WriteRow(1, bad, error));
    SA_CHECK(cache.WriteRow(0, {}, error));
    SA_CHECK(cache.WriteRow(1, Row(1), error));
    SA_CHECK(cache.Finish(error));
    cache.Close();
    {
        std::fstream bytes(std::filesystem::u8path(file.path), std::ios::binary | std::ios::in | std::ios::out);
        bytes.seekg(112);
        char value = 0;
        bytes.read(&value, 1);
        value ^= 0x40;
        bytes.seekp(112);
        bytes.write(&value, 1);
    }
    SA_CHECK(cache.Open(file.path, 2, 1, 65536, error));
    PathCacheRecord record;
    SA_CHECK(!cache.Lookup(1, 0, record, error));
    SA_CHECK(!error.empty());
}
