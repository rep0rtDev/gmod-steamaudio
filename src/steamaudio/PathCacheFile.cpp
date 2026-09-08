#include "steamaudio/PathCacheFile.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sa {
namespace {

struct Header {
    char magic[8];
    uint32_t version, probes, recordSize, flags;
    uint64_t key, indexOffset, fileSize;
    uint32_t completed, reserved, crc, reserved2;
};
struct RowHeader { uint32_t magic, row, count, rawBytes, packedBytes, crc; };
struct Index { uint64_t offset = 0; uint32_t packedBytes = 0, count = 0; };
static_assert(sizeof(Header) == 64 && sizeof(RowHeader) == 24 && sizeof(Index) == 16, "Path cache file layout changed");
constexpr uint32_t kRowMagic = 0x31574f52;
constexpr char kMagic[8] = {'S','A','P','A','T','H','0','1'};

uint32_t Crc(const void* bytes, size_t count)
{
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < values.size(); ++i) {
            uint32_t v = i;
            for (int bit = 0; bit < 8; ++bit) v = (v >> 1) ^ ((v & 1) ? 0xedb88320u : 0);
            values[i] = v;
        }
        return values;
    }();
    uint32_t result = ~uint32_t(0);
    const auto* data = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < count; ++i) result = table[(result ^ data[i]) & 255] ^ (result >> 8);
    return ~result;
}

#ifdef _WIN32
std::wstring Wide(const std::string& text)
{
    if (text.find('\0') != std::string::npos) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, &result[0], n);
    result.pop_back();
    return result;
}
#endif

FILE* OpenFile(const std::string& path, bool write, bool create = false)
{
#ifdef _WIN32
    const auto wide = Wide(path);
    if (wide.empty()) { errno = EINVAL; return nullptr; }
    return _wfopen(wide.c_str(), create ? L"wb+" : write ? L"rb+" : L"rb");
#else
    if (path.find('\0') != std::string::npos) { errno = EINVAL; return nullptr; }
    return std::fopen(path.c_str(), create ? "wb+" : write ? "rb+" : "rb");
#endif
}

bool Seek(FILE* file, uint64_t offset)
{
    if (offset > static_cast<uint64_t>(INT64_MAX)) return false;
#ifdef _WIN32
    return _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

uint64_t Size(FILE* file)
{
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END)) return UINT64_MAX;
    const auto n = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END)) return UINT64_MAX;
    const auto n = ftello(file);
#endif
    return n >= 0 ? static_cast<uint64_t>(n) : UINT64_MAX;
}

bool Sync(FILE* file)
{
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

bool Replace(const std::string& temporary, const std::string& target)
{
#ifdef _WIN32
    return MoveFileExW(Wide(temporary).c_str(), Wide(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temporary.c_str(), target.c_str()) == 0;
#endif
}

bool Valid(const std::vector<PathCacheRecord>& records, uint32_t row, uint32_t probes)
{
    uint32_t previous = 0;
    bool first = true;
    for (const auto& r : records) {
        if (r.target >= row || (!first && r.target <= previous) || r.flags > 1 ||
            !std::isfinite(r.distance) || !std::isfinite(r.deviation) || r.distance < 0 || r.deviation < 0)
            return false;
        if (r.flags == 0 && (r.first < 0 || r.last < 0)) return false;
        for (int32_t index : {r.first, r.last, r.afterFirst, r.beforeLast})
            if (index < -1 || index >= static_cast<int32_t>(probes)) return false;
        previous = r.target;
        first = false;
    }
    return true;
}

std::vector<uint8_t> Pack(const std::vector<PathCacheRecord>& records)
{
    std::vector<uint8_t> shuffled(records.size() * sizeof(PathCacheRecord));
    const auto* bytes = reinterpret_cast<const uint8_t*>(records.data());
    for (size_t column = 0; column < sizeof(PathCacheRecord); ++column)
        for (size_t i = 0; i < records.size(); ++i)
            shuffled[column * records.size() + i] = bytes[i * sizeof(PathCacheRecord) + column];
    std::vector<uint8_t> packed;
    packed.reserve(shuffled.size() + shuffled.size() / 128 + 1);
    size_t pos = 0;
    while (pos < shuffled.size()) {
        size_t run = 1;
        while (run < 130 && pos + run < shuffled.size() && shuffled[pos + run] == shuffled[pos]) ++run;
        if (run >= 3) {
            packed.push_back(static_cast<uint8_t>(128 + run - 3));
            packed.push_back(shuffled[pos]);
            pos += run;
        } else {
            const size_t begin = pos++;
            while (pos - begin < 128 && pos < shuffled.size()) {
                if (pos + 2 < shuffled.size() && shuffled[pos] == shuffled[pos + 1] && shuffled[pos] == shuffled[pos + 2]) break;
                ++pos;
            }
            packed.push_back(static_cast<uint8_t>(pos - begin - 1));
            packed.insert(packed.end(), shuffled.begin() + static_cast<ptrdiff_t>(begin), shuffled.begin() + static_cast<ptrdiff_t>(pos));
        }
    }
    return packed;
}

bool Unpack(const std::vector<uint8_t>& packed, uint32_t count, std::vector<PathCacheRecord>& records)
{
    const size_t required = static_cast<size_t>(count) * sizeof(PathCacheRecord);
    std::vector<uint8_t> shuffled;
    shuffled.reserve(required);
    size_t pos = 0;
    while (pos < packed.size()) {
        const uint8_t control = packed[pos++];
        const size_t length = control >= 128 ? static_cast<size_t>(control - 128) + 3 : static_cast<size_t>(control) + 1;
        if (length > required - shuffled.size()) return false;
        if (control >= 128) {
            if (pos == packed.size()) return false;
            shuffled.insert(shuffled.end(), length, packed[pos++]);
        } else {
            if (length > packed.size() - pos) return false;
            shuffled.insert(shuffled.end(), packed.begin() + static_cast<ptrdiff_t>(pos), packed.begin() + static_cast<ptrdiff_t>(pos + length));
            pos += length;
        }
    }
    if (shuffled.size() != required) return false;
    records.resize(count);
    auto* bytes = reinterpret_cast<uint8_t*>(records.data());
    for (size_t column = 0; column < sizeof(PathCacheRecord); ++column)
        for (size_t i = 0; i < count; ++i)
            bytes[i * sizeof(PathCacheRecord) + column] = shuffled[column * count + i];
    return true;
}

bool ReadHeader(FILE* file, Header& h, uint32_t probes, uint64_t key)
{
    return Seek(file, 0) && std::fread(&h, sizeof(h), 1, file) == 1 &&
           std::memcmp(h.magic, kMagic, sizeof(kMagic)) == 0 && h.version == 1 && h.recordSize == sizeof(PathCacheRecord) &&
           h.probes == probes && h.key == key && h.flags <= 1 && h.crc == Crc(&h, 56);
}

}

struct PathCacheFile::Impl {
    struct Page { std::vector<PathCacheRecord> records; uint64_t used = 0; };
    mutable std::mutex lock;
    FILE* file = nullptr;
    Header header{};
    std::vector<Index> index;
    std::unordered_map<uint32_t, Page> pages;
    std::string path, temporary;
    uint64_t cursor = sizeof(Header), cacheLimit = 0, cachedBytes = 0, ticks = 0, reads = 0;
    bool writer = false;

    ~Impl() { if (file) std::fclose(file); }

    bool Setup(uint32_t probes, uint64_t bytes, std::string& error)
    {
        if (probes == 0 || probes > 32768) { error = "Unsupported path probe count"; return false; }
        const uint64_t fixed = static_cast<uint64_t>(probes) * (sizeof(Index) + sizeof(PathCacheRecord) * 4) + 4096;
        if (bytes < fixed) { error = "Path page cache budget is too small"; return false; }
        cacheLimit = bytes - fixed;
        index.assign(probes, Index{});
        return true;
    }

    bool ReadPage(uint32_t row, const Index& entry, uint64_t end, std::vector<PathCacheRecord>& out, std::string& error)
    {
        RowHeader rh{};
        if (entry.offset < sizeof(Header) || entry.offset > end || end - entry.offset < sizeof(rh) ||
            !Seek(file, entry.offset) || std::fread(&rh, sizeof(rh), 1, file) != 1 || rh.magic != kRowMagic || rh.row != row ||
            rh.count > row || rh.rawBytes != rh.count * sizeof(PathCacheRecord) || rh.count != entry.count ||
            rh.packedBytes != entry.packedBytes || rh.packedBytes > rh.rawBytes + rh.rawBytes / 128 + 2 ||
            rh.packedBytes > end - entry.offset - sizeof(rh)) {
            error = "Invalid path cache row header";
            return false;
        }
        std::vector<uint8_t> packed(rh.packedBytes);
        if ((!packed.empty() && std::fread(packed.data(), 1, packed.size(), file) != packed.size()) ||
            rh.crc != Crc(packed.data(), packed.size()) || !Unpack(packed, rh.count, out) || !Valid(out, row, header.probes)) {
            error = "Corrupt path cache row";
            return false;
        }
        ++reads;
        return true;
    }
};

PathCacheFile::PathCacheFile() : m_impl(new Impl) {}
PathCacheFile::~PathCacheFile() = default;
void PathCacheFile::Close() { m_impl.reset(new Impl); }

uint64_t PathCacheFile::MaximumDiskBytes(uint32_t probes)
{
    if (probes == 0 || probes > 32768) return UINT64_MAX;
    const uint64_t raw = static_cast<uint64_t>(probes) * (probes - 1) / 2 * sizeof(PathCacheRecord);
    return sizeof(Header) + raw + raw / 128 + static_cast<uint64_t>(probes) * (sizeof(RowHeader) + sizeof(Index) + 2);
}

bool PathCacheFile::Open(const std::string& path, uint32_t probes, uint64_t key, uint64_t cacheBytes, std::string& error)
{
    Close();
    error.clear();
    auto& p = *m_impl;
    if (!p.Setup(probes, cacheBytes, error)) return false;
    p.file = OpenFile(path, false);
    if (!p.file) { error = "Could not open path cache"; return false; }
    const uint64_t size = Size(p.file);
    if (!ReadHeader(p.file, p.header, probes, key) || p.header.flags != 1 || p.header.completed != probes ||
        size != p.header.fileSize || p.header.indexOffset < sizeof(Header) || p.header.indexOffset > size ||
        size - p.header.indexOffset != static_cast<uint64_t>(probes) * sizeof(Index) ||
        !Seek(p.file, p.header.indexOffset) || std::fread(p.index.data(), sizeof(Index), probes, p.file) != probes) {
        error = "Invalid or stale path cache header";
        return false;
    }
    for (uint32_t i = 0; i < probes; ++i) {
        const auto& entry = p.index[i];
        if (entry.offset < sizeof(Header) || entry.offset >= p.header.indexOffset || entry.count > i ||
            entry.packedBytes > entry.count * sizeof(PathCacheRecord) + entry.count * sizeof(PathCacheRecord) / 128 + 2) {
            error = "Invalid path cache index";
            return false;
        }
    }
    p.path = path;
    p.cursor = p.header.indexOffset;
    return true;
}

bool PathCacheFile::Begin(const std::string& path, uint32_t probes, uint64_t key, uint64_t cacheBytes, std::string& error)
{
    if (Open(path, probes, key, cacheBytes, error)) return true;
    Close();
    error.clear();
    auto& p = *m_impl;
    if (!p.Setup(probes, cacheBytes, error)) return false;
    p.path = path;
    p.temporary = path + ".partial";
    p.file = OpenFile(p.temporary, true);
    if (!p.file && errno == ENOENT) {
        p.file = OpenFile(p.temporary, true, true);
        if (!p.file) { error = "Could not create temporary path cache"; return false; }
        std::memcpy(p.header.magic, kMagic, sizeof(kMagic));
        p.header.version = 1;
        p.header.probes = probes;
        p.header.recordSize = sizeof(PathCacheRecord);
        p.header.key = key;
        p.header.crc = Crc(&p.header, 56);
        if (std::fwrite(&p.header, sizeof(p.header), 1, p.file) != 1 || !Sync(p.file)) {
            error = "Could not write path cache header";
            return false;
        }
    } else {
        if (!p.file || !ReadHeader(p.file, p.header, probes, key)) {
            error = "Invalid temporary path cache; cannot resume";
            return false;
        }
        const uint64_t size = Size(p.file);
        const uint64_t end = p.header.flags ? p.header.indexOffset : size;
        if (end == UINT64_MAX || end > size) { error = "Invalid temporary path cache size"; return false; }
        p.header.completed = 0;
        p.header.flags = 0;
        while (p.cursor <= end && end - p.cursor >= sizeof(RowHeader)) {
            RowHeader row{};
            if (!Seek(p.file, p.cursor) || std::fread(&row, sizeof(row), 1, p.file) != 1 || row.row >= probes) break;
            Index entry{p.cursor, row.packedBytes, row.count};
            std::vector<PathCacheRecord> records;
            std::string rowError;
            if (!p.ReadPage(row.row, entry, end, records, rowError)) break;
            if (!p.index[row.row].offset) ++p.header.completed;
            p.index[row.row] = entry;
            p.cursor += sizeof(RowHeader) + row.packedBytes;
        }
    }
    p.writer = true;
    return true;
}

bool PathCacheFile::HasRow(uint32_t row) const
{
    std::lock_guard<std::mutex> lock(m_impl->lock);
    return row < m_impl->index.size() && m_impl->index[row].offset != 0;
}

bool PathCacheFile::WriteRow(uint32_t row, const std::vector<PathCacheRecord>& records, std::string& error)
{
    auto& p = *m_impl;
    std::lock_guard<std::mutex> lock(p.lock);
    error.clear();
    if (!p.writer || !p.file || row >= p.index.size() || records.size() > row || !Valid(records, row, p.header.probes)) {
        error = "Invalid path row or inactive writer";
        return false;
    }
    if (p.index[row].offset) return true;
    const auto packed = Pack(records);
    const RowHeader header{kRowMagic, row, static_cast<uint32_t>(records.size()),
                           static_cast<uint32_t>(records.size() * sizeof(PathCacheRecord)),
                           static_cast<uint32_t>(packed.size()), Crc(packed.data(), packed.size())};
    if (!Seek(p.file, p.cursor) || std::fwrite(&header, sizeof(header), 1, p.file) != 1 ||
        (!packed.empty() && std::fwrite(packed.data(), 1, packed.size(), p.file) != packed.size()) || std::fflush(p.file) != 0) {
        error = "Could not write path cache row (check free disk space)";
        return false;
    }
    p.index[row] = Index{p.cursor, header.packedBytes, header.count};
    p.cursor += sizeof(RowHeader) + packed.size();
    ++p.header.completed;
    return true;
}

bool PathCacheFile::Finish(std::string& error)
{
    auto& p = *m_impl;
    std::lock_guard<std::mutex> lock(p.lock);
    error.clear();
    if (!p.writer) return p.file && p.header.flags == 1;
    if (!p.file || p.header.completed != p.header.probes) { error = "Path cache is incomplete"; return false; }
    p.header.indexOffset = p.cursor;
    p.header.fileSize = p.cursor + p.index.size() * sizeof(Index);
    if (!Seek(p.file, p.cursor) || std::fwrite(p.index.data(), sizeof(Index), p.index.size(), p.file) != p.index.size() || !Sync(p.file)) {
        error = "Could not finalize path cache index";
        return false;
    }
#ifdef _WIN32
    if (_chsize_s(_fileno(p.file), p.header.fileSize) != 0) { error = "Could not finalize path cache size"; return false; }
#else
    if (ftruncate(fileno(p.file), static_cast<off_t>(p.header.fileSize)) != 0) { error = "Could not finalize path cache size"; return false; }
#endif
    p.header.flags = 1;
    p.header.crc = Crc(&p.header, 56);
    if (!Seek(p.file, 0) || std::fwrite(&p.header, sizeof(p.header), 1, p.file) != 1 || !Sync(p.file)) {
        error = "Could not finalize path cache header";
        return false;
    }
    const int closed = std::fclose(p.file);
    p.file = nullptr;
    if (closed != 0 || !Replace(p.temporary, p.path)) { error = "Could not publish path cache"; return false; }
    p.file = OpenFile(p.path, false);
    p.writer = false;
    if (!p.file) { error = "Could not reopen completed path cache"; return false; }
    return true;
}

bool PathCacheFile::Lookup(uint32_t row, uint32_t target, PathCacheRecord& record, std::string& error) const
{
    auto& p = *m_impl;
    std::lock_guard<std::mutex> lock(p.lock);
    error.clear();
    if (!p.file || row >= p.index.size() || target >= row || !p.index[row].offset) return false;
    auto found = p.pages.find(row);
    if (found == p.pages.end()) {
        const uint64_t bytes = static_cast<uint64_t>(p.index[row].count) * sizeof(PathCacheRecord) + 128;
        while (!p.pages.empty() && p.cachedBytes + bytes > p.cacheLimit) {
            auto oldest = std::min_element(p.pages.begin(), p.pages.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            p.cachedBytes -= oldest->second.records.capacity() * sizeof(PathCacheRecord) + 128;
            p.pages.erase(oldest);
        }
        Impl::Page page;
        if (!p.ReadPage(row, p.index[row], p.writer ? p.cursor : p.header.indexOffset, page.records, error)) return false;
        p.cachedBytes += page.records.capacity() * sizeof(PathCacheRecord) + 128;
        found = p.pages.emplace(row, std::move(page)).first;
    }
    found->second.used = ++p.ticks;
    const auto& records = found->second.records;
    const auto item = std::lower_bound(records.begin(), records.end(), target, [](const PathCacheRecord& r, uint32_t value) { return r.target < value; });
    if (item == records.end() || item->target != target) return false;
    record = *item;
    return true;
}

PathCacheFileStats PathCacheFile::Stats() const
{
    const auto& p = *m_impl;
    std::lock_guard<std::mutex> lock(p.lock);
    PathCacheFileStats result;
    result.probes = p.header.probes;
    result.key = p.header.key;
    result.completedRows = p.header.completed;
    result.complete = p.header.flags == 1;
    result.diskBytes = p.writer ? p.cursor : p.header.fileSize;
    result.residentBytes = p.index.capacity() * sizeof(Index) + p.cachedBytes;
    result.pageReads = p.reads;
    return result;
}

}
