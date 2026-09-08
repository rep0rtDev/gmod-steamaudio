// src/core/RttiLocator.h
//
// Locates C++ vtables in MSVC-built modules through RTTI metadata, which the
// Source engine binaries ship with. This gives us a signature-free way to find
// the engine's IAudioDevice implementation (CAudioDirectSound etc.):
//
//   TypeDescriptor  ".?AVCAudioDirectSound@@"   (in .data)
//        ^ referenced by
//   RTTICompleteObjectLocator { signature, offset, cdOffset, pTypeDescriptor,
//                               pClassDescriptor[, pSelf on x64] }   (.rdata)
//        ^ referenced by
//   vtable[-1]  ->  vtable[0..n)                                     (.rdata)
//
// On x64 the locator stores image-relative RVAs; on x86 absolute pointers.
// All lookups are read-only scans over the module sections.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util/SignatureScanner.h"

namespace sa {

struct RttiVTable {
    std::string className;   // mangled name without the ".?AV" prefix / "@@" suffix
    uintptr_t typeDescriptor = 0;
    uintptr_t objectLocator = 0;
    uintptr_t vtable = 0;    // address of slot 0
    size_t slotCount = 0;
    int32_t offset = 0;      // this-adjustment of this vtable within the object
};

class RttiLocator {
public:
    explicit RttiLocator(const ModuleImage& image);

    // Finds all vtables for a class. `className` is the plain C++ name
    // ("CAudioDirectSound"). Returns them sorted by `offset`; the primary
    // vtable has offset 0.
    std::vector<RttiVTable> FindVTables(const std::string& className) const;

    // Convenience: primary vtable (offset 0) or nullopt.
    std::optional<RttiVTable> FindPrimaryVTable(const std::string& className) const;

    // Scans the module's writable data sections for pointers to heap objects
    // whose first word equals `vtable` (i.e. global `Class* g_pObject`
    // variables). Returns the addresses of the pointer variables.
    std::vector<uintptr_t> FindObjectPointers(uintptr_t vtable) const;

    // Reads the demangled-ish class name of an object by following its vtable
    // back to RTTI. Empty when RTTI is not present.
    std::string ClassNameOfObject(const void* object) const;

private:
    const ModuleImage& m_image;
    SignatureScanner m_scanner;

    std::vector<uintptr_t> FindTypeDescriptors(const std::string& mangled) const;
    std::vector<uintptr_t> FindObjectLocators(uintptr_t typeDescriptor) const;
    std::optional<uintptr_t> ResolveLocatorField(uintptr_t locator, size_t fieldOffset) const;
};

} // namespace sa
