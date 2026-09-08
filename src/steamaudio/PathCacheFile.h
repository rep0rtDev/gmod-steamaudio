#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sa {

struct PathCacheRecord {
    uint16_t target = 0;
    int16_t first = -1;
    int16_t last = -1;
    int16_t afterFirst = -1;
    int16_t beforeLast = -1;
    uint16_t flags = 0;
    float distance = 0.f;
    float deviation = 0.f;
};
static_assert(sizeof(PathCacheRecord) == 20, "Path cache record layout changed");

struct PathCacheFileStats {
    uint32_t probes = 0;
    uint32_t completedRows = 0;
    uint64_t key = 0;
    uint64_t diskBytes = 0;
    uint64_t residentBytes = 0;
    uint64_t pageReads = 0;
    bool complete = false;
};

class PathCacheFile {
public:
    PathCacheFile();
    ~PathCacheFile();
    PathCacheFile(const PathCacheFile&) = delete;
    PathCacheFile& operator=(const PathCacheFile&) = delete;

    bool Begin(const std::string& path, uint32_t probes, uint64_t key, uint64_t cacheBytes, std::string& error);
    bool Open(const std::string& path, uint32_t probes, uint64_t key, uint64_t cacheBytes, std::string& error);
    bool HasRow(uint32_t row) const;
    bool WriteRow(uint32_t row, const std::vector<PathCacheRecord>& records, std::string& error);
    bool Finish(std::string& error);
    bool Lookup(uint32_t row, uint32_t target, PathCacheRecord& record, std::string& error) const;
    PathCacheFileStats Stats() const;
    void Close();
    static uint64_t MaximumDiskBytes(uint32_t probes);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
