// src/core/VmtHook.cpp
#include "core/VmtHook.h"

#include <atomic>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "util/Logging.h"

namespace sa {

// ---------------------------------------------------------------------------
// ScopedWritable
// ---------------------------------------------------------------------------
ScopedWritable::ScopedWritable(void* address, size_t size) : m_address(address), m_size(size)
{
#ifdef _WIN32
    DWORD old = 0;
    if (VirtualProtect(address, size, PAGE_READWRITE, &old)) {
        m_oldProtect = old;
        m_ok = true;
    } else {
        // Some loaders map .rdata as execute-read; try the RWX variant.
        if (VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
            m_oldProtect = old;
            m_ok = true;
        } else {
            SA_LOGE("[vmt] VirtualProtect(%p, %zu) failed: %lu", address, size, GetLastError());
        }
    }
#else
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + size;
    m_ok = mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE) == 0;
    m_oldProtect = PROT_READ;
#endif
}

ScopedWritable::~ScopedWritable()
{
    if (!m_ok)
        return;
#ifdef _WIN32
    DWORD old = 0;
    VirtualProtect(m_address, m_size, m_oldProtect, &old);
    FlushInstructionCache(GetCurrentProcess(), m_address, m_size);
#else
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t start = reinterpret_cast<uintptr_t>(m_address) & ~static_cast<uintptr_t>(page - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(m_address) + m_size;
    mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ);
#endif
}

// ---------------------------------------------------------------------------
// VmtHook
// ---------------------------------------------------------------------------
VmtHook::~VmtHook()
{
    Detach();
}

bool VmtHook::Attach(void** vtable, size_t slotCount)
{
    Detach();
    if (!vtable || slotCount == 0)
        return false;
    m_vtable = vtable;
    m_slotCount = slotCount;
    m_originals.assign(slotCount, nullptr);
    m_hooked.assign(slotCount, false);
    for (size_t i = 0; i < slotCount; ++i)
        m_originals[i] = vtable[i];
    return true;
}

void VmtHook::Detach()
{
    if (!m_vtable)
        return;
    UnhookAll();
    m_vtable = nullptr;
    m_slotCount = 0;
    m_originals.clear();
    m_hooked.clear();
}

bool VmtHook::WriteSlot(size_t index, void* value)
{
    void** slot = m_vtable + index;
    ScopedWritable writable(slot, sizeof(void*));
    if (!writable.Ok())
        return false;
    // Aligned pointer store: atomic w.r.t. concurrent readers on x86/x64.
#ifdef _WIN32
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), value);
#else
    __atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
#endif
    return true;
}

void* VmtHook::Hook(size_t index, void* replacement)
{
    if (!m_vtable || index >= m_slotCount || !replacement)
        return nullptr;
    if (m_hooked[index])
        return m_originals[index];
    if (!WriteSlot(index, replacement))
        return nullptr;
    m_hooked[index] = true;
    return m_originals[index];
}

void VmtHook::Unhook(size_t index)
{
    if (!m_vtable || index >= m_slotCount || !m_hooked[index])
        return;
    if (WriteSlot(index, m_originals[index]))
        m_hooked[index] = false;
    else
        SA_LOGE("[vmt] failed to restore slot %zu", index);
}

void VmtHook::UnhookAll()
{
    for (size_t i = m_slotCount; i-- > 0;)
        Unhook(i);
}

} // namespace sa
