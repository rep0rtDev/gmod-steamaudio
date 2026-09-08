// src/core/RttiLocator.cpp
#include "core/RttiLocator.h"

#include <algorithm>
#include <cstring>

namespace sa {

namespace {

// MSVC RTTI layouts (see <rttidata.h> in the CRT sources).
struct TypeDescriptorHeader {
    uintptr_t vfptr;   // type_info vtable
    uintptr_t spare;   // runtime reference
    // char name[];    // ".?AVClass@@"
};
constexpr size_t kTypeDescriptorNameOffset = sizeof(TypeDescriptorHeader);

struct CompleteObjectLocator32 {
    uint32_t signature;      // 0
    uint32_t offset;         // vtable offset within the complete object
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;   // absolute
    uint32_t pClassDescriptor;  // absolute
};

struct CompleteObjectLocator64 {
    uint32_t signature;      // 1
    uint32_t offset;
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;   // RVA
    uint32_t pClassDescriptor;  // RVA
    uint32_t pSelf;             // RVA of this locator
};

constexpr bool kIs64 = sizeof(void*) == 8;

} // namespace

RttiLocator::RttiLocator(const ModuleImage& image) : m_image(image), m_scanner(image) {}

std::vector<uintptr_t> RttiLocator::FindTypeDescriptors(const std::string& mangled) const
{
    std::vector<uintptr_t> result;
    for (uintptr_t nameAddress : m_scanner.FindString(mangled, true)) {
        if (nameAddress < kTypeDescriptorNameOffset)
            continue;
        const uintptr_t descriptor = nameAddress - kTypeDescriptorNameOffset;
        if (!IsMemoryReadable(descriptor, sizeof(TypeDescriptorHeader)))
            continue;
        // The first word of a TypeDescriptor is the type_info vtable which
        // lives in this module's read-only data.
        const auto vf = SignatureScanner::ReadPointer(descriptor);
        if (!vf || !m_image.Contains(*vf))
            continue;
        result.push_back(descriptor);
    }
    return result;
}

std::optional<uintptr_t> RttiLocator::ResolveLocatorField(uintptr_t locator, size_t fieldOffset) const
{
    const auto raw = SignatureScanner::ReadInt32(locator + fieldOffset);
    if (!raw)
        return std::nullopt;
    if (kIs64)
        return m_image.base + static_cast<uint32_t>(*raw);
    return static_cast<uintptr_t>(static_cast<uint32_t>(*raw));
}

std::vector<uintptr_t> RttiLocator::FindObjectLocators(uintptr_t typeDescriptor) const
{
    std::vector<uintptr_t> locators;
    // The locator stores either an absolute pointer (x86) or an RVA (x64) to
    // the type descriptor at field offset 12.
    std::vector<uintptr_t> refs;
    if (kIs64) {
        const uint32_t rva = static_cast<uint32_t>(typeDescriptor - m_image.base);
        Pattern p;
        for (int i = 0; i < 4; ++i) {
            p.bytes.push_back(static_cast<uint8_t>((rva >> (8 * i)) & 0xFF));
            p.mask.push_back(true);
        }
        for (const Section& s : m_image.sections) {
            if (s.executable || !s.readable)
                continue;
            for (uintptr_t hit : m_scanner.FindAll(p, s.name))
                refs.push_back(hit);
        }
    } else {
        refs = m_scanner.FindPointers(typeDescriptor);
    }

    for (uintptr_t ref : refs) {
        const uintptr_t locator = ref - offsetof(CompleteObjectLocator32, pTypeDescriptor);
        const size_t size = kIs64 ? sizeof(CompleteObjectLocator64) : sizeof(CompleteObjectLocator32);
        if (!IsMemoryReadable(locator, size))
            continue;
        const auto signature = SignatureScanner::ReadInt32(locator);
        if (!signature || *signature != (kIs64 ? 1 : 0))
            continue;
        if (kIs64) {
            const auto self = SignatureScanner::ReadInt32(locator + offsetof(CompleteObjectLocator64, pSelf));
            if (!self || m_image.base + static_cast<uint32_t>(*self) != locator)
                continue;
        }
        // Class descriptor must point into the image as well.
        const auto classDesc = ResolveLocatorField(locator, offsetof(CompleteObjectLocator32, pClassDescriptor));
        if (!classDesc || !m_image.Contains(*classDesc))
            continue;
        locators.push_back(locator);
    }
    return locators;
}

std::vector<RttiVTable> RttiLocator::FindVTables(const std::string& className) const
{
    std::vector<RttiVTable> result;
    const std::string mangled = ".?AV" + className + "@@";
    for (uintptr_t descriptor : FindTypeDescriptors(mangled)) {
        for (uintptr_t locator : FindObjectLocators(descriptor)) {
            // vtable[-1] holds a pointer to the locator.
            for (uintptr_t metaSlot : m_scanner.FindPointers(locator)) {
                const uintptr_t vtable = metaSlot + sizeof(void*);
                const auto info = m_scanner.ExpandVTableFromSlot(vtable);
                if (!info || info->slotCount == 0)
                    continue;
                RttiVTable vt;
                vt.className = className;
                vt.typeDescriptor = descriptor;
                vt.objectLocator = locator;
                vt.vtable = vtable;
                vt.slotCount = info->slotCount;
                const auto offset = SignatureScanner::ReadInt32(locator + offsetof(CompleteObjectLocator32, offset));
                vt.offset = offset ? *offset : 0;
                result.push_back(vt);
            }
        }
    }
    std::sort(result.begin(), result.end(),
              [](const RttiVTable& a, const RttiVTable& b) { return a.offset < b.offset; });
    return result;
}

std::optional<RttiVTable> RttiLocator::FindPrimaryVTable(const std::string& className) const
{
    for (const RttiVTable& vt : FindVTables(className))
        if (vt.offset == 0)
            return vt;
    return std::nullopt;
}

std::vector<uintptr_t> RttiLocator::FindObjectPointers(uintptr_t vtable) const
{
    std::vector<uintptr_t> result;
    for (const Section& s : m_image.sections) {
        if (!s.writable || !s.readable || s.executable)
            continue;
        if (!IsMemoryReadable(s.start, s.size))
            continue;
        const uintptr_t end = s.End() - sizeof(void*);
        for (uintptr_t addr = s.start; addr <= end; addr += sizeof(void*)) {
            uintptr_t candidate;
            std::memcpy(&candidate, reinterpret_cast<const void*>(addr), sizeof(candidate));
            if (candidate < 0x10000 || (candidate & (sizeof(void*) - 1)) != 0)
                continue;
#if UINTPTR_MAX > 0xFFFFFFFFu
            if (candidate > 0x00007FFFFFFEFFFFull) // canonical user-space limit
                continue;
#endif
            if (!IsMemoryReadable(candidate, sizeof(void*)))
                continue;
            uintptr_t first;
            std::memcpy(&first, reinterpret_cast<const void*>(candidate), sizeof(first));
            if (first == vtable)
                result.push_back(addr);
        }
    }
    return result;
}

std::string RttiLocator::ClassNameOfObject(const void* object) const
{
    const auto objAddr = reinterpret_cast<uintptr_t>(object);
    const auto vtable = SignatureScanner::ReadPointer(objAddr);
    if (!vtable || !m_image.IsReadOnlyDataAddress(*vtable))
        return {};
    const auto locator = SignatureScanner::ReadPointer(*vtable - sizeof(void*));
    if (!locator || !m_image.Contains(*locator))
        return {};
    const auto descriptor = ResolveLocatorField(*locator, offsetof(CompleteObjectLocator32, pTypeDescriptor));
    if (!descriptor || !m_image.Contains(*descriptor))
        return {};
    const uintptr_t nameAddress = *descriptor + kTypeDescriptorNameOffset;
    if (!IsMemoryReadable(nameAddress, 64))
        return {};
    const char* name = reinterpret_cast<const char*>(nameAddress);
    size_t len = 0;
    while (len < 256 && name[len] != '\0' && IsMemoryReadable(nameAddress + len, 1))
        ++len;
    std::string mangled(name, len);
    if (mangled.rfind(".?AV", 0) == 0)
        mangled = mangled.substr(4);
    if (mangled.size() >= 2 && mangled.compare(mangled.size() - 2, 2, "@@") == 0)
        mangled.resize(mangled.size() - 2);
    return mangled;
}

} // namespace sa
