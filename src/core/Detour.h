// src/core/Detour.h
//
// RAII wrapper over MinHook function detours (Windows). On other platforms the
// class compiles to an always-failing stub so platform-independent code that
// owns detours still builds and links.
//
// MinHook is initialised lazily on the first Detour::Install and torn down when
// the last detour is removed (reference counted, guarded by a mutex).
//
// Thread-safety: Install/Remove are game-thread operations; the detour target
// executes on whatever thread the hooked function is called from.
#pragma once

#include <cstdint>
#include <string>

namespace sa {

class Detour {
public:
    Detour() = default;
    ~Detour() { Remove(); }
    Detour(const Detour&) = delete;
    Detour& operator=(const Detour&) = delete;
    Detour(Detour&& other) noexcept { *this = std::move(other); }
    Detour& operator=(Detour&& other) noexcept
    {
        if (this != &other) {
            Remove();
            m_target = other.m_target;
            m_original = other.m_original;
            m_installed = other.m_installed;
            m_name = std::move(other.m_name);
            other.m_target = nullptr;
            other.m_original = nullptr;
            other.m_installed = false;
        }
        return *this;
    }

    // Installs and enables a detour. `original` receives the trampoline.
    bool Install(void* target, void* detour, void** original, const char* name, std::string& error,
                 bool enable = true);
    bool Enable(std::string& error);
    void Disable();
    void Remove();

    bool Installed() const { return m_installed; }
    void* Target() const { return m_target; }
    const std::string& Name() const { return m_name; }

    template <typename Fn>
    Fn Original() const
    {
        return reinterpret_cast<Fn>(m_original);
    }

    // Returns true when detours are supported on this platform/build.
    static bool Supported();

private:
    void* m_target = nullptr;
    void* m_original = nullptr;
    bool m_installed = false;
    std::string m_name;
};

// Resolves an exported symbol from an already-loaded module (no load attempted).
void* FindLoadedExport(const char* moduleName, const char* symbol);
bool IsModuleLoaded(const char* moduleName);

} // namespace sa
