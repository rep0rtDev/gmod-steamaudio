// src/core/Detour.cpp
#include "core/Detour.h"

#include <mutex>
#include <utility>

#include "util/Logging.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <MinHook.h>
#else
#include <dlfcn.h>
#endif

namespace sa {

namespace {

#ifdef _WIN32
std::mutex g_detourMutex;
int g_detourRefs = 0;

bool AcquireMinHook(std::string& error)
{
    if (g_detourRefs == 0) {
        const MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            error = std::string("MH_Initialize failed: ") + MH_StatusToString(st);
            return false;
        }
    }
    ++g_detourRefs;
    return true;
}

void ReleaseMinHook()
{
    if (g_detourRefs > 0 && --g_detourRefs == 0)
        MH_Uninitialize();
}
#endif

} // namespace

bool Detour::Supported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool Detour::Install(void* target, void* detour, void** original, const char* name, std::string& error, bool enable)
{
    Remove();
    if (!target || !detour) {
        error = "null detour target";
        return false;
    }
    m_name = name ? name : "";
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_detourMutex);
    if (!AcquireMinHook(error))
        return false;
    void* trampoline = nullptr;
    MH_STATUS st = MH_CreateHook(target, detour, &trampoline);
    if (st != MH_OK) {
        error = m_name + ": MH_CreateHook failed: " + MH_StatusToString(st);
        ReleaseMinHook();
        return false;
    }
    if (original)
        *original = trampoline;
    if (enable) {
        st = MH_EnableHook(target);
        if (st != MH_OK) {
            error = m_name + ": MH_EnableHook failed: " + MH_StatusToString(st);
            MH_RemoveHook(target);
            ReleaseMinHook();
            if (original)
                *original = nullptr;
            return false;
        }
    }
    m_target = target;
    m_original = trampoline;
    m_installed = true;
    SA_LOGD("detour installed: %s @ %p", m_name.c_str(), target);
    return true;
#else
    (void)original;
    (void)enable;
    error = m_name + ": function detours are not supported on this platform";
    return false;
#endif
}

bool Detour::Enable(std::string& error)
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_detourMutex);
    const MH_STATUS status = m_installed ? MH_EnableHook(m_target) : MH_ERROR_NOT_CREATED;
    if (status == MH_OK || status == MH_ERROR_ENABLED)
        return true;
    error = m_name + ": MH_EnableHook failed: " + MH_StatusToString(status);
#else
    error = "function detours are not supported on this platform";
#endif
    return false;
}

void Detour::Disable()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_detourMutex);
    if (m_installed)
        MH_DisableHook(m_target);
#endif
}

void Detour::Remove()
{
    if (!m_installed)
        return;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_detourMutex);
    MH_DisableHook(m_target);
    MH_RemoveHook(m_target);
    ReleaseMinHook();
    SA_LOGD("detour removed: %s", m_name.c_str());
#endif
    m_installed = false;
    m_target = nullptr;
    m_original = nullptr;
}

void* FindLoadedExport(const char* moduleName, const char* symbol)
{
    if (!moduleName || !symbol)
        return nullptr;
#ifdef _WIN32
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module)
        return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, symbol));
#else
    void* handle = dlopen(moduleName, RTLD_NOLOAD | RTLD_NOW);
    if (!handle)
        return nullptr;
    void* sym = dlsym(handle, symbol);
    dlclose(handle);
    return sym;
#endif
}

bool IsModuleLoaded(const char* moduleName)
{
    if (!moduleName)
        return false;
#ifdef _WIN32
    return GetModuleHandleA(moduleName) != nullptr;
#else
    void* handle = dlopen(moduleName, RTLD_NOLOAD | RTLD_NOW);
    if (!handle)
        return false;
    dlclose(handle);
    return true;
#endif
}

} // namespace sa
