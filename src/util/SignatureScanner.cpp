// src/util/SignatureScanner.cpp
#include "SignatureScanner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sa {

// ---------------------------------------------------------------------------
// ModuleImage
// ---------------------------------------------------------------------------

const Section* ModuleImage::FindSection(const std::string& sectionName) const
{
    for (const Section& s : sections)
        if (s.name == sectionName)
            return &s;
    return nullptr;
}

const Section* ModuleImage::SectionContaining(uintptr_t address) const
{
    for (const Section& s : sections)
        if (s.Contains(address))
            return &s;
    return nullptr;
}

bool ModuleImage::IsCodeAddress(uintptr_t address) const
{
    const Section* s = SectionContaining(address);
    return s != nullptr && s->executable;
}

bool ModuleImage::IsReadOnlyDataAddress(uintptr_t address) const
{
    const Section* s = SectionContaining(address);
    return s != nullptr && s->readable && !s->executable && !s->writable;
}

bool ModuleImage::IsDataAddress(uintptr_t address) const
{
    const Section* s = SectionContaining(address);
    return s != nullptr && s->readable && !s->executable;
}

std::optional<ModuleImage> GetLoadedModuleImage(const std::string& moduleName)
{
#ifdef _WIN32
    HMODULE module = GetModuleHandleA(moduleName.c_str());
    if (!module)
        return std::nullopt;

    const auto base = reinterpret_cast<uintptr_t>(module);
    if (!IsMemoryReadable(base, sizeof(IMAGE_DOS_HEADER)))
        return std::nullopt;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return std::nullopt;
    const uintptr_t ntAddress = base + static_cast<uintptr_t>(dos->e_lfanew);
    if (!IsMemoryReadable(ntAddress, sizeof(IMAGE_NT_HEADERS)))
        return std::nullopt;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return std::nullopt;

    ModuleImage image;
    image.name = moduleName;
    image.base = base;
    image.size = nt->OptionalHeader.SizeOfImage;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    const WORD count = nt->FileHeader.NumberOfSections;
    if (!IsMemoryReadable(reinterpret_cast<uintptr_t>(section), sizeof(IMAGE_SECTION_HEADER) * count))
        return std::nullopt;

    for (WORD i = 0; i < count; ++i, ++section) {
        Section s;
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        s.name = name;
        s.start = base + section->VirtualAddress;
        s.size = std::max<DWORD>(section->Misc.VirtualSize, section->SizeOfRawData);
        s.readable = (section->Characteristics & IMAGE_SCN_MEM_READ) != 0;
        s.writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        s.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (s.size == 0)
            continue;
        // Clamp to image bounds.
        if (s.start + s.size > base + image.size)
            s.size = base + image.size - s.start;
        image.sections.push_back(std::move(s));
    }
    return image;
#else
    (void)moduleName;
    return std::nullopt;
#endif
}

ModuleImage MakeBufferImage(const void* data, size_t size, const std::string& sectionName, bool executable)
{
    ModuleImage image;
    image.name = "<buffer>";
    image.base = reinterpret_cast<uintptr_t>(data);
    image.size = size;
    Section s;
    s.name = sectionName;
    s.start = image.base;
    s.size = size;
    s.readable = true;
    s.writable = false;
    s.executable = executable;
    image.sections.push_back(std::move(s));
    return image;
}

bool IsMemoryReadable(uintptr_t address, size_t length)
{
    if (address == 0 || length == 0)
        return false;
    const uintptr_t end = address + length;
    if (end < address)
        return false;
#ifdef _WIN32
    uintptr_t cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                              prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                              prot == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
            return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor || regionEnd < reinterpret_cast<uintptr_t>(mbi.BaseAddress))
            return false;
        cursor = regionEnd;
    }
    return true;
#else
    // Non-Windows builds only scan synthetic buffers owned by the caller.
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Pattern
// ---------------------------------------------------------------------------

std::optional<Pattern> Pattern::Parse(const std::string& idaStyle)
{
    Pattern p;
    std::istringstream in(idaStyle);
    std::string token;
    while (in >> token) {
        if (token == "?" || token == "??" || token == "*") {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            continue;
        }
        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            token = token.substr(2);
        if (token.size() != 2 || !std::isxdigit(static_cast<unsigned char>(token[0])) ||
            !std::isxdigit(static_cast<unsigned char>(token[1])))
            return std::nullopt;
        p.bytes.push_back(static_cast<uint8_t>(std::strtoul(token.c_str(), nullptr, 16)));
        p.mask.push_back(true);
    }
    if (p.bytes.empty())
        return std::nullopt;
    return p;
}

std::optional<Pattern> Pattern::ParseCodeStyle(const std::string& bytes, const std::string& mask)
{
    if (bytes.size() != mask.size() || bytes.empty())
        return std::nullopt;
    Pattern p;
    p.bytes.reserve(bytes.size());
    p.mask.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        p.bytes.push_back(static_cast<uint8_t>(bytes[i]));
        p.mask.push_back(mask[i] == 'x' || mask[i] == 'X');
    }
    return p;
}

std::string Pattern::ToString() const
{
    std::string out;
    char buf[8];
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (!mask[i]) {
            out += "? ";
        } else {
            std::snprintf(buf, sizeof(buf), "%02X ", bytes[i]);
            out += buf;
        }
    }
    if (!out.empty())
        out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// VTableInfo
// ---------------------------------------------------------------------------

uintptr_t VTableInfo::Slot(size_t index) const
{
    if (index >= slotCount)
        return 0;
    const auto value = SignatureScanner::ReadPointer(SlotAddress(index));
    return value.value_or(0);
}

// ---------------------------------------------------------------------------
// SignatureScanner
// ---------------------------------------------------------------------------

SignatureScanner::SignatureScanner(ModuleImage image) : m_image(std::move(image)) {}

std::vector<const Section*> SignatureScanner::SelectSections(const std::string& sectionName, bool executableOnly,
                                                             bool dataOnly) const
{
    std::vector<const Section*> out;
    for (const Section& s : m_image.sections) {
        if (!s.readable)
            continue;
        if (!sectionName.empty() && s.name != sectionName)
            continue;
        if (executableOnly && !s.executable)
            continue;
        if (dataOnly && s.executable)
            continue;
        out.push_back(&s);
    }
    return out;
}

void SignatureScanner::ScanSection(const Section& section, const Pattern& pattern, std::vector<uintptr_t>& out,
                                   size_t maxResults) const
{
    const size_t patternSize = pattern.Size();
    if (patternSize == 0 || section.size < patternSize)
        return;
    if (!IsMemoryReadable(section.start, section.size))
        return;

    // Find first fixed byte for memchr acceleration.
    size_t firstFixed = 0;
    while (firstFixed < patternSize && !pattern.mask[firstFixed])
        ++firstFixed;
    const bool hasFixed = firstFixed < patternSize;

    const auto* begin = reinterpret_cast<const uint8_t*>(section.start);
    const uint8_t* const end = begin + section.size - patternSize + 1;
    const uint8_t* cursor = begin;

    while (cursor < end) {
        if (hasFixed) {
            const uint8_t needle = pattern.bytes[firstFixed];
            const size_t remaining = static_cast<size_t>(end - cursor);
            const auto* found =
                static_cast<const uint8_t*>(std::memchr(cursor + firstFixed, needle, remaining));
            if (!found)
                return;
            cursor = found - firstFixed;
        }
        bool match = true;
        for (size_t i = 0; i < patternSize; ++i) {
            if (pattern.mask[i] && cursor[i] != pattern.bytes[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            out.push_back(reinterpret_cast<uintptr_t>(cursor));
            if (out.size() >= maxResults)
                return;
        }
        ++cursor;
    }
}

std::vector<uintptr_t> SignatureScanner::FindAll(const Pattern& pattern, const std::string& sectionName,
                                                 size_t maxResults) const
{
    std::vector<uintptr_t> out;
    for (const Section* s : SelectSections(sectionName, false, false)) {
        ScanSection(*s, pattern, out, maxResults);
        if (out.size() >= maxResults)
            break;
    }
    return out;
}

std::optional<uintptr_t> SignatureScanner::FindFirst(const Pattern& pattern, const std::string& sectionName) const
{
    const auto results = FindAll(pattern, sectionName, 1);
    if (results.empty())
        return std::nullopt;
    return results.front();
}

std::optional<uintptr_t> SignatureScanner::FindUnique(const Pattern& pattern, const std::string& sectionName) const
{
    const auto results = FindAll(pattern, sectionName, 2);
    if (results.size() != 1)
        return std::nullopt;
    return results.front();
}

std::vector<uintptr_t> SignatureScanner::FindString(const std::string& literal, bool requireNulTermination) const
{
    std::vector<uintptr_t> out;
    if (literal.empty())
        return out;
    Pattern p;
    for (const char c : literal) {
        p.bytes.push_back(static_cast<uint8_t>(c));
        p.mask.push_back(true);
    }
    if (requireNulTermination) {
        p.bytes.push_back(0);
        p.mask.push_back(true);
    }
    for (const Section* s : SelectSections("", false, true))
        ScanSection(*s, p, out, SIZE_MAX);

    if (requireNulTermination) {
        // Drop matches that are the tail of a longer string (preceded by a
        // printable, non-NUL byte).
        std::vector<uintptr_t> filtered;
        for (const uintptr_t addr : out) {
            const Section* s = m_image.SectionContaining(addr);
            if (s && addr > s->start) {
                const uint8_t prev = *reinterpret_cast<const uint8_t*>(addr - 1);
                if (prev != 0)
                    continue;
            }
            filtered.push_back(addr);
        }
        return filtered;
    }
    return out;
}

std::vector<uintptr_t> SignatureScanner::FindCodeReferences(uintptr_t target, const std::string& sectionName) const
{
    std::vector<uintptr_t> out;
    for (const Section* s : SelectSections(sectionName, true, false)) {
        if (s->size < 4 || !IsMemoryReadable(s->start, s->size))
            continue;
        const auto* begin = reinterpret_cast<const uint8_t*>(s->start);
        const size_t last = s->size - 4;
        for (size_t i = 0; i <= last; ++i) {
            int32_t disp;
            std::memcpy(&disp, begin + i, sizeof(disp));
#if UINTPTR_MAX == 0xFFFFFFFFu
            if (static_cast<uint32_t>(disp) == static_cast<uint32_t>(target)) {
                out.push_back(s->start + i);
                continue;
            }
#else
            // RIP-relative: displacement is relative to the end of the instruction,
            // which is the byte after the 4-byte displacement for lea/mov/call
            // with no immediate following. Instructions with a trailing imm8/imm32
            // (e.g. cmp [rip+x], imm8) are covered by also testing +1 and +4.
            const uintptr_t dispEnd = s->start + i + 4;
            if (dispEnd + static_cast<intptr_t>(disp) == target ||
                dispEnd + 1 + static_cast<intptr_t>(disp) == target ||
                dispEnd + 4 + static_cast<intptr_t>(disp) == target) {
                out.push_back(s->start + i);
                continue;
            }
            if (i + 8 <= s->size) {
                uint64_t abs;
                std::memcpy(&abs, begin + i, sizeof(abs));
                if (abs == static_cast<uint64_t>(target))
                    out.push_back(s->start + i);
            }
#endif
        }
    }
    return out;
}

std::vector<uintptr_t> SignatureScanner::FindPointers(uintptr_t value) const
{
    std::vector<uintptr_t> out;
    for (const Section* s : SelectSections("", false, true)) {
        if (s->size < sizeof(uintptr_t) || !IsMemoryReadable(s->start, s->size))
            continue;
        const auto* begin = reinterpret_cast<const uint8_t*>(s->start);
        const size_t last = s->size - sizeof(uintptr_t);
        for (size_t i = 0; i <= last; i += sizeof(uintptr_t)) {
            uintptr_t candidate;
            std::memcpy(&candidate, begin + i, sizeof(candidate));
            if (candidate == value)
                out.push_back(s->start + i);
        }
    }
    return out;
}

std::optional<VTableInfo> SignatureScanner::ExpandVTableFromSlot(uintptr_t slotAddress) const
{
    const Section* s = m_image.SectionContaining(slotAddress);
    if (!s || s->executable)
        return std::nullopt;
    if (slotAddress % sizeof(uintptr_t) != 0)
        return std::nullopt;

    auto isCodeSlot = [&](uintptr_t addr) {
        if (!s->Contains(addr, sizeof(uintptr_t)))
            return false;
        const auto v = ReadPointer(addr);
        return v.has_value() && m_image.IsCodeAddress(*v);
    };
    if (!isCodeSlot(slotAddress))
        return std::nullopt;

    // Walk backward to the first slot. MSVC places the RTTI complete-object
    // locator pointer (a data pointer, not code) immediately before slot 0.
    uintptr_t base = slotAddress;
    while (isCodeSlot(base - sizeof(uintptr_t)))
        base -= sizeof(uintptr_t);

    // Walk forward to count slots.
    uintptr_t cursor = base;
    while (isCodeSlot(cursor))
        cursor += sizeof(uintptr_t);

    VTableInfo info;
    info.base = base;
    info.slotCount = (cursor - base) / sizeof(uintptr_t);
    return info;
}

std::optional<FunctionInVTable> SignatureScanner::FindVirtualFunctionEnclosing(uintptr_t codeAddress,
                                                                               size_t maxBackward) const
{
    if (!m_image.IsCodeAddress(codeAddress))
        return std::nullopt;

    // Collect all vtable slot candidates in read-only data whose target lies
    // within [codeAddress - maxBackward, codeAddress].
    const uintptr_t low = codeAddress > maxBackward ? codeAddress - maxBackward : 0;
    std::optional<FunctionInVTable> best;

    for (const Section* s : SelectSections("", false, true)) {
        if (s->writable || s->size < sizeof(uintptr_t) || !IsMemoryReadable(s->start, s->size))
            continue;
        const auto* begin = reinterpret_cast<const uint8_t*>(s->start);
        const size_t last = s->size - sizeof(uintptr_t);
        for (size_t i = 0; i <= last; i += sizeof(uintptr_t)) {
            uintptr_t candidate;
            std::memcpy(&candidate, begin + i, sizeof(candidate));
            if (candidate < low || candidate > codeAddress)
                continue;
            const auto vt = ExpandVTableFromSlot(s->start + i);
            if (!vt || vt->slotCount < 4)
                continue;
            if (!best || candidate > best->function) {
                FunctionInVTable f;
                f.function = candidate;
                f.vtable = *vt;
                f.slotIndex = (s->start + i - vt->base) / sizeof(uintptr_t);
                best = f;
            }
        }
    }
    return best;
}

std::optional<uintptr_t> SignatureScanner::ResolveRelative(uintptr_t instruction, size_t dispOffset,
                                                           size_t instrLen)
{
    const auto disp = ReadInt32(instruction + dispOffset);
    if (!disp)
        return std::nullopt;
    return instruction + instrLen + static_cast<intptr_t>(*disp);
}

std::optional<uintptr_t> SignatureScanner::ReadPointer(uintptr_t address)
{
    if (!IsMemoryReadable(address, sizeof(uintptr_t)))
        return std::nullopt;
    uintptr_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::optional<int32_t> SignatureScanner::ReadInt32(uintptr_t address)
{
    if (!IsMemoryReadable(address, sizeof(int32_t)))
        return std::nullopt;
    int32_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::optional<int64_t> ParseInteger(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 0);
    if (!end || end == text.c_str())
        return std::nullopt;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return std::nullopt;
    return static_cast<int64_t>(v);
}

} // namespace sa
