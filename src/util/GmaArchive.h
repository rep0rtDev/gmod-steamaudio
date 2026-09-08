// src/util/GmaArchive.h
//
// Reader for Garry's Mod addon archives (.gma, "GMAD" format as written by
// gmad.exe / the Workshop). Used as a fallback when the engine's IFileSystem
// cannot be reached: the locator scans the addon directories on disk, indexes
// each archive lazily and extracts single files (e.g. "maps/foo.bsp").
//
// Workshop downloads are LZMA-alone compressed archives; those are inflated
// to memory on open. Plain archives are read from disk on demand so a 1 GB
// content pack costs only its index.
//
// Thread-safety: none; use from one thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sa {

struct GmaEntry {
    std::string name;   // lowercase, forward slashes, as stored
    uint64_t offset = 0; // absolute offset of the payload within the (decompressed) archive
    uint64_t size = 0;
    uint32_t crc = 0;
};

struct GmaHeader {
    uint8_t version = 0;
    uint64_t steamId = 0;
    uint64_t timestamp = 0;
    std::vector<std::string> requiredContent;
    std::string name;
    std::string description;
    std::string author;
    int32_t addonVersion = 0;
};

class GmaArchive {
public:
    static constexpr size_t kMaxIndexBytes = 64u << 20;     // hard cap for the header + index
    static constexpr size_t kMaxCompressedArchive = size_t(2) << 30;

    GmaArchive() = default;

    // Opens the archive at `path`, reading only the index (whole file when
    // LZMA-alone compressed).
    bool Open(const std::string& path, std::string& error);
    // Parses an in-memory archive (compressed or plain); takes ownership.
    bool OpenMemory(std::vector<uint8_t>&& bytes, std::string& error);
    void Close();

    bool IsOpen() const { return m_open; }
    const std::string& Path() const { return m_path; }
    const GmaHeader& Header() const { return m_header; }
    const std::vector<GmaEntry>& Entries() const { return m_entries; }
    bool WasCompressed() const { return m_compressed; }

    // Case-insensitive lookup; accepts backslashes and a leading slash.
    const GmaEntry* Find(const std::string& virtualPath) const;
    bool Extract(const GmaEntry& entry, std::vector<uint8_t>& out, std::string& error) const;

    // Canonical form used for lookups: lowercase, '/' separators, no leading '/'.
    static std::string NormalizePath(const std::string& path);

private:
    bool ParseIndex(const uint8_t* data, size_t size, bool& needMore, std::string& error);

    std::string m_path;
    std::vector<uint8_t> m_memory; // whole archive when in memory
    bool m_inMemory = false;
    bool m_compressed = false;
    bool m_open = false;
    GmaHeader m_header;
    std::vector<GmaEntry> m_entries;
    uint64_t m_fileSize = 0;
};

// Finds files across every *.gma / *.cache in a set of directories. Indexes
// are parsed once per scan and cached; payloads are read on demand.
class GmaLocator {
public:
    void SetDirectories(std::vector<std::string> directories);
    const std::vector<std::string>& Directories() const { return m_directories; }

    // Drops the cached archive list; the next lookup rescans the directories.
    void Invalidate();

    // Extracts `virtualPath` from the first archive that contains it.
    bool Find(const std::string& virtualPath, std::vector<uint8_t>& out, std::string& error,
              std::string* archivePath = nullptr);
    // Index-only lookup; nothing is extracted.
    bool Contains(const std::string& virtualPath, std::string* archivePath = nullptr);
    // Archive paths discovered by the last scan (for diagnostics).
    std::vector<std::string> ScanArchives();

private:
    struct IndexedArchive {
        std::string path;
        bool compressed = false;
        std::vector<GmaEntry> entries;

        bool Contains(const std::string& key) const;
    };

    void IndexAll(std::string& lastError);

    std::vector<std::string> m_directories;
    std::vector<std::string> m_archives;
    std::vector<IndexedArchive> m_indexed;
    bool m_scanned = false;
};

} // namespace sa
