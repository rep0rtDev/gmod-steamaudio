// src/core/VmtHook.h
//
// Virtual-method-table hooking. Two flavours:
//   * VmtHook       : patches entries of a shared vtable in place (affects every
//                     object of that class). Used for the engine's IAudioDevice
//                     whose vtable lives in engine.dll's .rdata.
//   * The original entries are restored in reverse order on Uninstall/destruction.
//
// Thread-safety: Install/Uninstall run on the game thread while the engine's
// mixer may be executing through the same vtable. Entries are swapped with a
// single aligned pointer store, which is atomic on x86/x64; the hooked
// functions must therefore be valid before Install and remain valid until the
// mixer can no longer be inside them after Uninstall (EngineHooks waits one
// paint cycle before tearing down capture state).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sa {

class VmtHook {
public:
    VmtHook() = default;
    ~VmtHook();
    VmtHook(const VmtHook&) = delete;
    VmtHook& operator=(const VmtHook&) = delete;

    // `vtable` = address of slot 0, `slotCount` = number of slots we may touch.
    bool Attach(void** vtable, size_t slotCount);
    void Detach();
    bool IsAttached() const { return m_vtable != nullptr; }

    // Replaces slot `index`, returns the original function or nullptr on error.
    void* Hook(size_t index, void* replacement);
    void Unhook(size_t index);
    void UnhookAll();

    template <typename Fn>
    Fn Original(size_t index) const
    {
        return index < m_originals.size() ? reinterpret_cast<Fn>(m_originals[index]) : nullptr;
    }
    void* OriginalRaw(size_t index) const { return index < m_originals.size() ? m_originals[index] : nullptr; }
    bool IsHooked(size_t index) const { return index < m_hooked.size() && m_hooked[index]; }

private:
    bool WriteSlot(size_t index, void* value);

    void** m_vtable = nullptr;
    size_t m_slotCount = 0;
    std::vector<void*> m_originals;
    std::vector<bool> m_hooked;
};

// Makes [address, address+size) writable for the lifetime of the object and
// restores the previous protection afterwards.
class ScopedWritable {
public:
    ScopedWritable(void* address, size_t size);
    ~ScopedWritable();
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;
    bool Ok() const { return m_ok; }

private:
    void* m_address;
    size_t m_size;
    uint32_t m_oldProtect = 0;
    bool m_ok = false;
};

} // namespace sa
