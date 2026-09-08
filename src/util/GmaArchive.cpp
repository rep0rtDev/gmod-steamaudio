// src/util/GmaArchive.cpp
#include "util/GmaArchive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "util/Lzma.h"

namespace sa {

namespace {

constexpr uint32_t kGmadIdent = ('D' << 24) | ('A' << 16) | ('M' << 8) | 'G';
constexpr size_t kMinHeader = 4 + 1 + 8 + 8;

struct Cursor {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool overflow = false;

    bool Need(size_t n)
    {
        if (overflow || n > size - pos) {
            overflow = true;
            return false;
        }
        return true;
    }
    uint8_t U8()
    {
        if (!Need(1))
            return 0;
        return data[pos++];
    }
    uint32_t U32()
    {
        if (!Need(4))
            return 0;
        uint32_t v = 0;
        std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    }
    uint64_t U64()
    {
        if (!Need(8))
            return 0;
        uint64_t v = 0;
        std::memcpy(&v, data + pos, 8);
        pos += 8;
        return v;
    }
    std::string CString()
    {
        if (overflow)
            return {};
        const size_t start = pos;
        while (pos < size && data[pos] != 0)
            ++pos;
        if (pos >= size) {
            overflow = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data + start), pos - start);
        ++pos; // terminator
        return s;
    }
};

bool ReadFileRange(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>& out,
                   std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        error = "cannot open " + path;
        return false;
    }
    f.seekg(static_cast<std::streamoff>(offset));
    out.resize(length);
    if (length == 0)
        return true;
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
    if (f.gcount() != static_cast<std::streamsize>(length)) {
        error = "short read from " + path;
        out.clear();
        return false;
    }
    return true;
}

uint64_t FileSizeOf(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

} // namespace

// ---------------------------------------------------------------------------
// GmaArchive
// ---------------------------------------------------------------------------
std::string GmaArchive::NormalizePath(const std::string& path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\')
            c = '/';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    size_t start = 0;
    while (start < out.size() && out[start] == '/')
        ++start;
    return out.substr(start);
}

void GmaArchive::Close()
{
    m_path.clear();
    m_memory.clear();
    m_memory.shrink_to_fit();
    m_inMemory = false;
    m_compressed = false;
    m_open = false;
    m_header = GmaHeader{};
    m_entries.clear();
    m_fileSize = 0;
}

bool GmaArchive::ParseIndex(const uint8_t* data, size_t size, bool& needMore, std::string& error)
{
    needMore = false;
    Cursor c{data, size};
    if (size < kMinHeader) {
        needMore = true;
        return false;
    }
    if (c.U32() != kGmadIdent) {
        error = "not a GMAD archive";
        return false;
    }
    GmaHeader header;
    header.version = c.U8();
    if (header.version == 0 || header.version > 3) {
        error = "unsupported GMAD version " + std::to_string(header.version);
        return false;
    }
    header.steamId = c.U64();
    header.timestamp = c.U64();
    if (header.version > 1) {
        for (;;) {
            std::string content = c.CString();
            if (c.overflow || content.empty())
                break;
            header.requiredContent.push_back(std::move(content));
            if (header.requiredContent.size() > 4096) {
                error = "GMAD required-content list too long";
                return false;
            }
        }
    }
    header.name = c.CString();
    header.description = c.CString();
    header.author = c.CString();
    header.addonVersion = static_cast<int32_t>(c.U32());
    if (c.overflow) {
        needMore = true;
        return false;
    }

    std::vector<GmaEntry> entries;
    uint64_t running = 0;
    for (;;) {
        const uint32_t fileNumber = c.U32();
        if (c.overflow) {
            needMore = true;
            return false;
        }
        if (fileNumber == 0)
            break;
        GmaEntry e;
        e.name = NormalizePath(c.CString());
        e.size = c.U64();
        e.crc = c.U32();
        if (c.overflow) {
            needMore = true;
            return false;
        }
        e.offset = running;
        if (e.size > (uint64_t(1) << 40) || running + e.size < running) {
            error = "GMAD entry size overflow";
            return false;
        }
        running += e.size;
        entries.push_back(std::move(e));
        if (entries.size() > 1u << 20) {
            error = "GMAD index too large";
            return false;
        }
    }
    const uint64_t dataStart = c.pos;
    for (GmaEntry& e : entries)
        e.offset += dataStart;
    if (m_fileSize && dataStart + running > m_fileSize) {
        error = "GMAD index describes more data than the archive holds";
        return false;
    }
    m_header = std::move(header);
    m_entries = std::move(entries);
    return true;
}

bool GmaArchive::OpenMemory(std::vector<uint8_t>&& bytes, std::string& error)
{
    Close();
    m_memory = std::move(bytes);
    if (m_memory.size() >= 4) {
        uint32_t ident = 0;
        std::memcpy(&ident, m_memory.data(), 4);
        if (ident != kGmadIdent && lzma::IsLzmaAlone(m_memory.data(), m_memory.size())) {
            std::vector<uint8_t> plain;
            if (!lzma::DecompressAlone(m_memory.data(), m_memory.size(), plain, &error, kMaxCompressedArchive)) {
                Close();
                return false;
            }
            m_memory = std::move(plain);
            m_compressed = true;
        }
    }
    m_inMemory = true;
    m_fileSize = m_memory.size();
    bool needMore = false;
    if (!ParseIndex(m_memory.data(), m_memory.size(), needMore, error)) {
        if (needMore)
            error = "truncated GMAD index";
        Close();
        return false;
    }
    m_open = true;
    return true;
}

bool GmaArchive::Open(const std::string& path, std::string& error)
{
    Close();
    m_path = path;
    m_fileSize = FileSizeOf(path);
    if (m_fileSize == 0) {
        error = "cannot stat " + path;
        Close();
        return false;
    }
    std::vector<uint8_t> prefix;
    if (!ReadFileRange(path, 0, static_cast<size_t>(std::min<uint64_t>(m_fileSize, lzma::kAloneHeaderSize)), prefix,
                       error)) {
        Close();
        return false;
    }
    uint32_t ident = 0;
    if (prefix.size() >= 4)
        std::memcpy(&ident, prefix.data(), 4);
    if (ident != kGmadIdent) {
        // Possibly a compressed Workshop download: inflate the whole thing.
        if (!lzma::IsLzmaAlone(prefix.data(), prefix.size()) || m_fileSize > kMaxCompressedArchive) {
            error = path + " is not a GMAD archive";
            Close();
            return false;
        }
        std::vector<uint8_t> whole;
        if (!ReadFileRange(path, 0, static_cast<size_t>(m_fileSize), whole, error)) {
            Close();
            return false;
        }
        const std::string keep = path;
        if (!OpenMemory(std::move(whole), error))
            return false;
        m_path = keep;
        return true;
    }

    size_t window = static_cast<size_t>(std::min<uint64_t>(m_fileSize, 256 * 1024));
    for (;;) {
        if (!ReadFileRange(path, 0, window, prefix, error)) {
            Close();
            return false;
        }
        bool needMore = false;
        if (ParseIndex(prefix.data(), prefix.size(), needMore, error))
            break;
        if (!needMore || window >= m_fileSize || window >= kMaxIndexBytes) {
            if (needMore)
                error = "truncated GMAD index in " + path;
            Close();
            return false;
        }
        window = static_cast<size_t>(std::min<uint64_t>(m_fileSize, uint64_t(window) * 4));
    }
    m_open = true;
    return true;
}

const GmaEntry* GmaArchive::Find(const std::string& virtualPath) const
{
    const std::string key = NormalizePath(virtualPath);
    for (const GmaEntry& e : m_entries)
        if (e.name == key)
            return &e;
    return nullptr;
}

bool GmaArchive::Extract(const GmaEntry& entry, std::vector<uint8_t>& out, std::string& error) const
{
    out.clear();
    if (!m_open) {
        error = "archive not open";
        return false;
    }
    if (entry.offset > m_fileSize || entry.size > m_fileSize - entry.offset) {
        error = "entry " + entry.name + " extends past the archive";
        return false;
    }
    if (entry.size > (uint64_t(1) << 31)) {
        error = "entry " + entry.name + " is too large";
        return false;
    }
    if (m_inMemory) {
        out.assign(m_memory.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                   m_memory.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.size));
        return true;
    }
    return ReadFileRange(m_path, entry.offset, static_cast<size_t>(entry.size), out, error);
}

// ---------------------------------------------------------------------------
// GmaLocator
// ---------------------------------------------------------------------------
void GmaLocator::SetDirectories(std::vector<std::string> directories)
{
    m_directories = std::move(directories);
    Invalidate();
}

void GmaLocator::Invalidate()
{
    m_archives.clear();
    m_indexed.clear();
    m_scanned = false;
}

std::vector<std::string> GmaLocator::ScanArchives()
{
    if (m_scanned)
        return m_archives;
    m_archives.clear();
    m_indexed.clear();
    for (const std::string& dir : m_directories) {
        const size_t first = m_archives.size();
        std::error_code ec;
        std::filesystem::directory_iterator it(std::filesystem::u8path(dir), ec);
        if (ec)
            continue;
        for (const auto& entry : it) {
            std::error_code fec;
            if (!entry.is_regular_file(fec) || fec)
                continue;
            std::string ext = entry.path().extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            // ".cache" is what older clients named LZMA-compressed Workshop
            // downloads; Open() rejects anything that is not a GMAD payload.
            if (ext == ".gma" || ext == ".cache")
                m_archives.push_back(entry.path().u8string());
        }
        std::sort(m_archives.begin() + static_cast<std::ptrdiff_t>(first), m_archives.end());
    }
    m_scanned = true;
    return m_archives;
}

void GmaLocator::IndexAll(std::string& lastError)
{
    if (!m_indexed.empty() || m_archives.empty())
        return;
    m_indexed.reserve(m_archives.size());
    for (const std::string& path : m_archives) {
        GmaArchive archive;
        std::string openError;
        if (!archive.Open(path, openError)) {
            lastError = openError;
            continue;
        }
        IndexedArchive indexed;
        indexed.path = path;
        indexed.compressed = archive.WasCompressed();
        indexed.entries = archive.Entries();
        m_indexed.push_back(std::move(indexed));
    }
}

bool GmaLocator::Find(const std::string& virtualPath, std::vector<uint8_t>& out, std::string& error,
                      std::string* archivePath)
{
    out.clear();
    const std::vector<std::string> archives = ScanArchives();
    if (archives.empty()) {
        error = "no .gma archives found in the configured addon directories";
        return false;
    }
    std::string lastError;
    IndexAll(lastError);
    const std::string key = GmaArchive::NormalizePath(virtualPath);
    for (const IndexedArchive& indexed : m_indexed) {
        if (!indexed.Contains(key))
            continue;
        // Compressed archives are re-inflated per extraction so the index
        // cache never pins whole addons in memory.
        GmaArchive archive;
        if (!archive.Open(indexed.path, error))
            return false;
        const GmaEntry* entry = archive.Find(key);
        if (!entry) {
            error = key + " vanished from " + indexed.path;
            return false;
        }
        if (!archive.Extract(*entry, out, error))
            return false;
        if (archivePath)
            *archivePath = indexed.path;
        return true;
    }
    error = key + " not found in " + std::to_string(m_indexed.size()) + " of " + std::to_string(archives.size()) +
            " archive(s)" + (lastError.empty() ? "" : " (last open error: " + lastError + ")");
    return false;
}

bool GmaLocator::Contains(const std::string& virtualPath, std::string* archivePath)
{
    if (ScanArchives().empty())
        return false;
    std::string ignored;
    IndexAll(ignored);
    const std::string key = GmaArchive::NormalizePath(virtualPath);
    for (const IndexedArchive& indexed : m_indexed) {
        if (!indexed.Contains(key))
            continue;
        if (archivePath)
            *archivePath = indexed.path;
        return true;
    }
    return false;
}

bool GmaLocator::IndexedArchive::Contains(const std::string& key) const
{
    for (const GmaEntry& e : entries)
        if (e.name == key)
            return true;
    return false;
}

} // namespace sa
