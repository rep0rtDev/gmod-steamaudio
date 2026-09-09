// src/util/SignatureScanner.h
//
// Runtime resolution of non-exported engine symbols:
//   * IDA-style byte pattern search ("55 8B EC ? ? 83 EC 08")
//   * string literal search + code cross-reference search (x86 absolute
//     immediates, x64 RIP-relative displacements)
//   * pointer search in read-only data (vtable slots)
//   * vtable reconstruction around a known virtual function
//
// All lookups operate on a `ModuleImage`, which describes the loaded PE
// sections of a module. On Windows the image is built from the in-memory PE
// headers; a synthetic image can also be built from a plain byte buffer so
// the scanner can be unit-tested on any platform.
//
// Thread-safety: `ModuleImage` and `SignatureScanner` are immutable after
// construction and can be shared between threads. All memory reads are
// guarded by `IsMemoryReadable` so a bad configuration cannot crash the
// process during scanning.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace sa {

struct Section {
    std::string name;
    uintptr_t start = 0;
    size_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;

    bool Contains(uintptr_t address, size_t length = 1) const
    {
        return address >= start && address + length <= start + size;
    }
    uintptr_t End() const { return start + size; }
};

struct ModuleImage {
    std::string name;
    uintptr_t base = 0;
    size_t size = 0;
    std::vector<Section> sections;

    bool IsValid() const { return base != 0 && size != 0; }
    const Section* FindSection(const std::string& sectionName) const;
    const Section* SectionContaining(uintptr_t address) const;
    bool IsCodeAddress(uintptr_t address) const;
    bool IsReadOnlyDataAddress(uintptr_t address) const;
    bool IsDataAddress(uintptr_t address) const; // any non-executable readable section
    bool Contains(uintptr_t address) const { return address >= base && address < base + size; }
};

// Returns the image of an already-loaded module (e.g. "engine.dll").
// Returns nullopt when the module is not loaded or the headers are unreadable.
std::optional<ModuleImage> GetLoadedModuleImage(const std::string& moduleName);

// Builds a synthetic image over a byte buffer. The whole buffer is treated as
// one section with the given attributes (used by tests and by callers that
// already own a copy of code bytes).
ModuleImage MakeBufferImage(const void* data, size_t size, const std::string& sectionName = ".text",
                            bool executable = true);

// Best-effort check that [address, address+length) is committed readable
// memory. Uses VirtualQuery on Windows.
bool IsMemoryReadable(uintptr_t address, size_t length);
bool HasCodeVTableSlots(const void* object, const ModuleImage& image, std::initializer_list<int32_t> slots);

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask; // true = must match, false = wildcard

    // Parses "55 8B EC ?? ? 83 EC 08" (both "?" and "??" are wildcards).
    // Also accepts "\x55\x8B\xEC" with "xxx?" style mask via ParseCodeStyle.
    static std::optional<Pattern> Parse(const std::string& idaStyle);
    static std::optional<Pattern> ParseCodeStyle(const std::string& bytes, const std::string& mask);

    size_t Size() const { return bytes.size(); }
    bool Empty() const { return bytes.empty(); }
    std::string ToString() const;
};

struct VTableInfo {
    uintptr_t base = 0;        // address of slot 0
    size_t slotCount = 0;      // number of consecutive code pointers
    uintptr_t Slot(size_t index) const;
    uintptr_t SlotAddress(size_t index) const { return base + index * sizeof(void*); }
};

struct FunctionInVTable {
    uintptr_t function = 0;
    VTableInfo vtable;
    size_t slotIndex = 0;
};

class SignatureScanner {
public:
    explicit SignatureScanner(ModuleImage image);

    const ModuleImage& Image() const { return m_image; }

    // Pattern search. `sectionName` may be empty to search all readable sections.
    std::vector<uintptr_t> FindAll(const Pattern& pattern, const std::string& sectionName = ".text",
                                   size_t maxResults = SIZE_MAX) const;
    std::optional<uintptr_t> FindFirst(const Pattern& pattern, const std::string& sectionName = ".text") const;
    // Returns the match only if exactly one exists.
    std::optional<uintptr_t> FindUnique(const Pattern& pattern, const std::string& sectionName = ".text") const;

    // Finds NUL-terminated string literals in non-executable sections.
    std::vector<uintptr_t> FindString(const std::string& literal, bool requireNulTermination = true) const;

    // Finds code locations that reference `target`:
    //   x86: 4-byte absolute immediates equal to target
    //   x64: 4-byte RIP-relative displacements (disp at offset i resolves to
    //        i + 4 + disp == target), plus 8-byte absolute immediates.
    // Returned addresses point at the displacement/immediate itself.
    std::vector<uintptr_t> FindCodeReferences(uintptr_t target, const std::string& sectionName = ".text") const;

    // Finds pointer-sized values equal to `value` inside non-executable sections.
    std::vector<uintptr_t> FindPointers(uintptr_t value) const;

    // Given a slot address inside a vtable, expands to the full table.
    std::optional<VTableInfo> ExpandVTableFromSlot(uintptr_t slotAddress) const;

    // Given any code address inside a virtual function, walks backwards up to
    // `maxBackward` bytes looking for a function start that appears as a
    // vtable slot. Returns the closest match.
    std::optional<FunctionInVTable> FindVirtualFunctionEnclosing(uintptr_t codeAddress,
                                                                 size_t maxBackward = 512) const;

    // Resolves a relative call/jmp/lea target: instr + instrLen + disp32.
    static std::optional<uintptr_t> ResolveRelative(uintptr_t instruction, size_t dispOffset, size_t instrLen);

    // Reads a pointer-sized value; nullopt when not readable.
    static std::optional<uintptr_t> ReadPointer(uintptr_t address);
    static std::optional<int32_t> ReadInt32(uintptr_t address);

private:
    ModuleImage m_image;

    std::vector<const Section*> SelectSections(const std::string& sectionName, bool executableOnly,
                                               bool dataOnly) const;
    void ScanSection(const Section& section, const Pattern& pattern, std::vector<uintptr_t>& out,
                     size_t maxResults) const;
};

// Parses an integer that may be decimal or 0x-prefixed hex.
std::optional<int64_t> ParseInteger(const std::string& text);

} // namespace sa
